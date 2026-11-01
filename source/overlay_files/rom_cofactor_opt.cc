#include "kernel/yosys.h"
#include "kernel/sigtools.h"

#include <vector>
#include <cstdint>
#include <array>
#include <map>
#include <set>
#include <algorithm>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct RomCofactorOptPass : public Pass
{
    RomCofactorOptPass() : Pass("rom_cofactor_opt", "decompose small constant ROMs into shared four-input LUT cofactors") {}

    void help() override
    {
        log("\n    rom_cofactor_opt [selection]\n\n");
        log("For a fully defined asynchronous 32xN constant ROM, search XOR bases to implement\n");
        log("each output as two 4-input address cofactors and one select LUT.\n");
        log("A unique zero-default mux consumer can be folded into the select\n");
        log("LUT without changing other ROM consumers.\n\n");
    }

    static RTLIL::SigBit lut4(RTLIL::Module *module, const RTLIL::SigSpec &input, int mask)
    {
        log_assert(GetSize(input) == 4);
        auto wire = module->addWire(NEW_ID);
        // An owned, deferred LUT descriptor preserves the algorithm's cut
        // while stock technology mapping runs. Only our finish phase lowers it.
        auto type=RTLIL::escape_id("__pmux_rom_lut4");auto design=module->design;
        if(!design->module(type)) {
            auto lib=design->addModule(type);lib->set_bool_attribute(ID::blackbox);
            lib->set_bool_attribute(RTLIL::escape_id("pmux_owned_descriptor"));
            auto a=lib->addWire(ID::A,4);a->port_input=true;
            auto y=lib->addWire(ID::Y,1);y->port_output=true;lib->fixup_ports();
            lib->parameter_default_values[ID::LUT]=RTLIL::Const(0,16);
        }
        log_assert(design->module(type)->get_bool_attribute(RTLIL::escape_id("pmux_owned_descriptor")));
        auto cell=module->addCell(NEW_ID,type);cell->setParam(ID::LUT,RTLIL::Const(mask,16));
        cell->setPort(ID::A,input);cell->setPort(ID::Y,wire);
        return RTLIL::SigBit(wire);
    }

    struct Cofactor {
        int support = 0, truth = 0;
        bool inverted = false;
        std::pair<int,int> key() const { return {support,truth}; }
        bool needs_lut() const { return support && (support & (support-1)); }
    };

    static Cofactor describe(uint32_t plane,int split,int half)
    {
        int raw=0,low_mask=(1<<split)-1;
        for(int word=0;word<16;word++) {
            int index=(word&low_mask)|((word&~low_mask)<<1);
            bool low=(plane>>index)&1,high=(plane>>(index|(1<<split)))&1;
            if(half==0?low:half==1?high:(low!=high))raw|=1<<word;
        }
        int local[4],count=0;const int masks[]={0x5555,0x3333,0x0f0f,0x00ff};Cofactor desc;
        for(int k=0;k<4;k++)if(((raw^(raw>>(1<<k)))&masks[k])!=0){local[count++]=k;desc.support|=1<<(k+(k>=split));}
        for(int v=0;v<(1<<count);v++) {
            int index=0;for(int k=0;k<count;k++)index|=((v>>k)&1)<<local[k];
            if((raw>>index)&1)desc.truth|=1<<v;
        }
        if(count==1){desc.inverted=desc.truth==1;desc.truth=2;}
        else {int complement=((1<<(1<<count))-1)^desc.truth;if(complement<desc.truth){desc.truth=complement;desc.inverted=true;}}
        return desc;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        extra_args(args, 1, design);
        log_header(design, "Executing ROM_COFACTOR_OPT pass.\n");
        int roms = 0, folded_muxes = 0, luts = 0;
        for (auto module : design->selected_modules()) {
            std::vector<RTLIL::Cell *> memories;
            for (auto cell : module->selected_cells())
                if (cell->type == ID($mem_v2)) memories.push_back(cell);
            if(memories.empty())continue;
            SigMap sigmap(module);
            for (auto mem : memories) {
                if (mem->getParam(ID::WIDTH).as_int() < 1 ||
                    mem->getParam(ID::WIDTH).as_int() > 32 ||
                    mem->getParam(ID::SIZE).as_int() != 32 ||
                    mem->getParam(ID::ABITS).as_int() != 5 ||
                    mem->getParam(ID::OFFSET).as_int() != 0 ||
                    mem->getParam(ID::RD_PORTS).as_int() != 1 ||
                    mem->getParam(ID::WR_PORTS).as_int() != 0 ||
                    mem->getParam(ID::RD_CLK_ENABLE).as_int() != 0)
                    continue;
                int width = mem->getParam(ID::WIDTH).as_int();
                RTLIL::SigSpec address = sigmap(mem->getPort(ID::RD_ADDR));
                RTLIL::SigSpec output = sigmap(mem->getPort(ID::RD_DATA));
                RTLIL::SigSpec enable = sigmap(mem->getPort(ID::RD_EN));
                RTLIL::SigSpec arst = sigmap(mem->getPort(ID::RD_ARST));
                RTLIL::SigSpec srst = sigmap(mem->getPort(ID::RD_SRST));
                RTLIL::Const init = mem->getParam(ID::INIT);
                if (GetSize(address) != 5 || GetSize(output) != width ||
                    enable != RTLIL::SigSpec(RTLIL::State::S1) ||
                    arst != RTLIL::SigSpec(RTLIL::State::S0) ||
                    srst != RTLIL::SigSpec(RTLIL::State::S0) ||
                    GetSize(init) != width * 32 || !init.is_fully_def())
                    continue;

                dict<RTLIL::SigBit, int> bit_index, uses;
                for (int bit = 0; bit < width; bit++) bit_index[output[bit]] = bit;
                if (GetSize(bit_index) != width) continue;
                for (auto cell : module->cells()) {
                    if (cell == mem) continue;
                    for (auto &connection : cell->connections())
                        for (auto bit : sigmap(connection.second))
                            if (bit_index.count(bit)) uses[bit]++;
                }
                for (auto wire : module->wires())
                    if (wire->port_output)
                        for (auto bit : sigmap(wire))
                            if (bit_index.count(bit)) uses[bit]++;

                std::vector<RTLIL::SigBit> gating(width, RTLIL::State::S1);
                std::vector<RTLIL::Cell *> removable;
                for (auto cell : module->selected_cells()) {
                    if (cell->type != ID($mux)) continue;
                    int count = cell->getParam(ID::WIDTH).as_int();
                    RTLIL::SigSpec a = sigmap(cell->getPort(ID::A));
                    RTLIL::SigSpec b = sigmap(cell->getPort(ID::B));
                    RTLIL::SigSpec s = sigmap(cell->getPort(ID::S));
                    if (GetSize(a) != count || GetSize(b) != count ||
                        GetSize(s) != 1 || s[0].wire == nullptr ||
                        a != RTLIL::SigSpec(RTLIL::State::S0, count))
                        continue;
                    bool safe = true;
                    for (auto bit : b)
                        if (!bit_index.count(bit) || uses[bit] != 1 ||
                            gating[bit_index.at(bit)] != RTLIL::State::S1)
                            safe = false;
                    if (!safe) continue;
                    for (auto bit : b) gating[bit_index.at(bit)] = s[0];
                    removable.push_back(cell);
                }
                if (removable.empty()) continue;

                std::vector<uint32_t> planes(width,0);
                for(int bit=0;bit<width;bit++)for(int word=0;word<32;word++)if(init[word*width+bit]==RTLIL::State::S1)planes[bit]|=uint32_t(1)<<word;
                std::vector<std::array<std::array<Cofactor,2>,15>> choices(width);
                // Three unique cofactors per cut; all six basis slots reuse them.
                for(int bit=0;bit<width;bit++) for(int split=0;split<5;split++) {
                    std::array<Cofactor,3> functions;
                    for(int f=0;f<3;f++)functions[f]=describe(planes[bit],split,f);
                    for(int basis=0;basis<3;basis++) for(int part=0;part<2;part++) {
                        int function=basis==0?part:(part==1?2:basis-1);
                        choices[bit][split*3+basis][part]=functions[function];
                    }
                }
                auto cost = [&](const std::vector<int> &order) {
                    std::set<std::pair<int,int>> unique;
                    for(int bit=0;bit<width;bit++)
                        for(auto &part:choices[bit][order[bit]]) if(part.needs_lut()) unique.insert(part.key());
                    return int(unique.size());
                };
                std::vector<int> best(width,12);
                int best_cost=cost(best);
                // Bound search to the 15 Shannon/XOR cut choices per output.
                // Intern each shared cofactor once and use reference counts:
                // changing one output requires at most two membership checks.
                std::map<std::pair<int,int>,int> feature_ids;
                std::vector<std::array<std::array<int,2>,15>> choice_ids(width);
                for(int bit=0;bit<width;bit++) for(int option=0;option<15;option++) {
                    auto &ids=choice_ids[bit][option];ids={{-1,-1}};
                    for(int k=0;k<2;k++) if(choices[bit][option][k].needs_lut()) {
                        auto key=choices[bit][option][k].key();auto it=feature_ids.find(key);
                        if(it==feature_ids.end())it=feature_ids.emplace(key,int(feature_ids.size())).first;
                        ids[k]=it->second;
                    }
                }
                // Fifteen deterministic starts cover all cuts and three invertible bases; each coordinate update must improve.
                // This bounds work while allowing different outputs to share cuts.
                for(int seed=0;seed<15;seed++) {
                    std::vector<int> order(width,seed);
                    std::vector<int> references(feature_ids.size(),0);int active=0;
                    auto add=[&](const std::array<int,2>& ids){for(int id:ids)if(id>=0&&references[id]++==0)active++;};
                    auto remove=[&](const std::array<int,2>& ids){for(int id:ids)if(id>=0&&--references[id]==0)active--;};
                    for(int bit=0;bit<width;bit++)add(choice_ids[bit][order[bit]]);
                    int current_cost=active;
                    bool changed=true;
                    while(changed) {
                        changed=false;
                        for(int bit=0;bit<width;bit++) {
                            int selected=order[bit],selected_cost=current_cost;
                            remove(choice_ids[bit][order[bit]]);
                            for(int split=0;split<15;split++) {
                                const auto &ids=choice_ids[bit][split];int trial=active;
                                if(ids[0]>=0&&references[ids[0]]==0)trial++;
                                if(ids[1]>=0&&ids[1]!=ids[0]&&references[ids[1]]==0)trial++;
                                if(trial<selected_cost) {selected=split;selected_cost=trial;}
                            }
                            order[bit]=selected;
                            add(choice_ids[bit][selected]);log_assert(active==selected_cost);
                            if(selected_cost<current_cost) changed=true;
                            current_cost=selected_cost;
                        }
                    }
                    log_assert(active==cost(order));
                    if(current_cost<best_cost) {best=order;best_cost=current_cost;}
                }
                int xor_outputs=0;for(auto option:best)if(option%3)xor_outputs++;
                log("rom_cofactor_opt: searched output-specific cuts and XOR bases, shared cofactor LUTs=%d; XOR outputs=%d.\n",best_cost,xor_outputs);
                std::map<std::pair<int,int>,RTLIL::SigBit> cofactor_cache;
                auto cofactor = [&](const Cofactor &part) -> RTLIL::SigBit {
                    if(!part.support) return RTLIL::SigBit(RTLIL::State::S0);
                    RTLIL::SigSpec pins;
                    for(int k=0;k<5;k++) if(part.support&(1<<k)) pins.append(address[k]);
                    if(GetSize(pins)==1) return pins[0];
                    if(!cofactor_cache.count(part.key())) {
                        int n=GetSize(pins),mask=0;
                        for(int v=0;v<16;v++) if((part.truth>>(v&((1<<n)-1)))&1) mask|=1<<v;
                        while(GetSize(pins)<4) pins.append(RTLIL::State::S0);
                        cofactor_cache[part.key()]=lut4(module,pins,mask);
                        luts++;
                    }
                    return cofactor_cache.at(part.key());
                };
                RTLIL::SigSpec result;
                for (int bit = 0; bit < width; bit++) {
                    auto &parts=choices[bit][best[bit]];
                    RTLIL::SigBit low = cofactor(parts[0]);
                    RTLIL::SigBit high = cofactor(parts[1]);
                    if (best[bit]%3==0 && low == high && !parts[0].inverted && !parts[1].inverted && gating[bit] == RTLIL::State::S1) {
                        result.append(low);
                        continue;
                    }
                    RTLIL::SigSpec inputs;
                    inputs.append(low);
                    inputs.append(high);
                    inputs.append(address[best[bit]/3]);
                    inputs.append(gating[bit]);
                    int mask = 0;
                    for (int value = 0; value < 16; value++) {
                        bool first=bool(value&1)^parts[0].inverted;
                        bool second=bool(value&2)^parts[1].inverted;
                        bool select=bool(value&4),selected;
                        if(best[bit]%3==0)selected=select?second:first;
                        else if(best[bit]%3==1)selected=first^(select&&second);
                        else selected=first^(!select&&second);
                        if ((value & 8) && selected) mask |= 1 << value;
                    }
                    result.append(lut4(module, inputs, mask));
                    luts++;
                }
                for (auto gate : removable) {
                    module->connect(gate->getPort(ID::Y), gate->getPort(ID::B));
                    module->remove(gate);
                    folded_muxes++;
                }
                module->remove(mem);
                module->connect(output, result);
                roms++;
            }
        }
        if(roms)design->scratchpad_set_bool("pmux_v31.changed",true);
        log("rom_cofactor_opt: converted %d ROMs, folded %d zero muxes, added %d LUTs.\n",
            roms, folded_muxes, luts);
    }
} RomCofactorOptPass;


// Complete our ROM decomposition after stock default technology mapping.
// This phase touches only owned descriptors, never imported cells or backend code.
struct PmuxRomFinishPass : public Pass {
    PmuxRomFinishPass():Pass("pmux_rom_finish","emit the deferred ROM cofactor LUT decomposition"){}
    void execute(std::vector<std::string> args,RTLIL::Design *d)override {
        extra_args(args,1,d);auto type=RTLIL::escape_id("__pmux_rom_lut4");auto lib=d->module(type);if(!lib)return;
        log_assert(lib->get_bool_attribute(RTLIL::escape_id("pmux_owned_descriptor")));int count=0;
        for(auto m:d->selected_modules())for(auto c:m->selected_cells())if(c->type==type) {
            auto mask=c->getParam(ID::LUT);log_assert(GetSize(mask)==16&&mask.is_fully_def());
            log_assert(GetSize(c->getPort(ID::A))==4&&GetSize(c->getPort(ID::Y))==1);
            c->type=ID($lut);c->setParam(ID::WIDTH,4);count++;
        }
        bool referenced=false;for(auto m:d->modules())for(auto c:m->cells())if(c->type==type)referenced=true;
        if(!referenced)d->remove(lib);
        log("pmux_rom_finish: emitted %d owned cofactor LUTs.\n",count);
    }
} PmuxRomFinishPass;
PRIVATE_NAMESPACE_END
