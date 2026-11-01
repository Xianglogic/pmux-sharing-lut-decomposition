#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Only original $pmux cells are candidates. Generated muxes/LUTs implement
// those cells; this pass never searches ordinary muxes, registers or memories.
struct PmuxCofactorSharePass : public Pass
{
    PmuxCofactorSharePass() : Pass("pmux_cofactor_share", "share crossed cofactors of decoded PMUX tables for LUT4") {}
    void help() override
    {
        log("\n    pmux_cofactor_share [selection]\n\n");
        log("For Cyclone IV LUT4: match two complete 16-arm $pmux tables\n");
        log("with one common four-bit equality key and equal width 8..512.\n");
        log("If toggling one key bit swaps their cofactors except for at\n");
        log("most one leaf in each half, share two eight-entry data tables.\n");
        log("All generated logic derives from the two original PMUX cells.\n\n");
    }
    struct Candidate {
        RTLIL::Cell *cell;
        RTLIL::SigSpec key;
        std::vector<RTLIL::SigSpec> table;
        int width;
    };
    static bool equality(RTLIL::Cell *cell, const SigMap &map, RTLIL::SigSpec &key, int &value)
    {
        auto y=map(cell->getPort(ID::Y));
        if (GetSize(y)!=1) return false;
        if (cell->type==ID($logic_not)) {
            key=map(cell->getPort(ID::A)); value=0;
            return GetSize(key)==4 && !key.has_const();
        }
        if (cell->type!=ID($eq)) return false;
        auto a=map(cell->getPort(ID::A)), b=map(cell->getPort(ID::B));
        if (GetSize(a)!=4 || GetSize(b)!=4) return false;
        if (a.is_fully_const()) std::swap(a,b);
        if (a.has_const() || !b.is_fully_const() || !b.as_const().is_fully_def()) return false;
        key=a;value=b.as_const().as_int(false);
        return true;
    }
    static RTLIL::SigSpec lut4(RTLIL::Module *m, RTLIL::SigSpec in, int mask)
    {
        log_assert(GetSize(in)==4);
        auto out=m->addWire(NEW_ID);
        auto c=m->addCell(NEW_ID, RTLIL::escape_id("cycloneiv_lcell_comb"));
        c->setParam(RTLIL::escape_id("lut_mask"),RTLIL::Const(mask,16));
        c->setParam(RTLIL::escape_id("sum_lutc_input"),RTLIL::Const("datac"));
        const char *pins[]={"dataa","datab","datac","datad"};
        for (int k=0;k<4;k++) c->setPort(RTLIL::escape_id(pins[k]),in.extract(k,1));
        c->setPort(RTLIL::escape_id("cin"),RTLIL::State::S0);
        c->setPort(RTLIL::escape_id("combout"),out);
        c->setPort(RTLIL::escape_id("cout"),m->addWire(NEW_ID));
        return out;
    }
    static RTLIL::SigSpec read8(RTLIL::Module *m, RTLIL::SigSpec key,
            const std::vector<RTLIL::SigSpec> &leaves, int width, RTLIL::SigSpec enables)
    {
        log_assert(GetSize(key)==3 && GetSize(leaves)==8 && GetSize(enables)==4);
        RTLIL::SigSpec result;
        for (int bit=0;bit<width;bit++) {
            RTLIL::SigSpec terms;
            for (int pair=0;pair<4;pair++) {
                RTLIL::SigSpec in(leaves[2*pair][bit]);
                in.append(leaves[2*pair+1][bit]);in.append(key.extract(0,1));
                in.append(enables.extract(pair,1));
                terms.append(lut4(m,in,0xCA00)); // e & (s ? high : low)
            }
            result.append(lut4(m,terms,0xFFFE));
        }
        return result;
    }
    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        extra_args(args,1,design);
        log_header(design,"Executing PMUX_COFACTOR_SHARE pass.\n");
        int examined=0,rewritten=0;
        for (auto m:design->selected_modules()) {
            int eligible=0, selected_pmux=0;
            for(auto c:m->selected_cells())if(c->type==ID($pmux)) {
                selected_pmux++;int width=c->getParam(ID::WIDTH).as_int();
                if(width>=8&&width<=512&&c->getParam(ID::S_WIDTH).as_int()==16)eligible++;
            }
            if(eligible<2){examined+=selected_pmux;continue;}
            SigMap map(m);
            dict<RTLIL::SigBit,RTLIL::Cell *> drivers;
            pool<RTLIL::SigBit> ambiguous;
            for (auto c:m->cells()) {
                if (c->type!=ID($eq) && c->type!=ID($logic_not)) continue;
                auto y=map(c->getPort(ID::Y));if(GetSize(y)!=1)continue;
                if(drivers.count(y[0]))ambiguous.insert(y[0]);else drivers[y[0]]=c;
            }
            std::vector<Candidate> candidates;
            for (auto c:m->selected_cells()) {
                if(c->type!=ID($pmux))continue;
                examined++;
                int width=c->getParam(ID::WIDTH).as_int();
                if(width<8 || width>512 || c->getParam(ID::S_WIDTH).as_int()!=16)continue;
                auto s=map(c->getPort(ID::S)),b=map(c->getPort(ID::B));
                if(GetSize(s)!=16 || GetSize(b)!=16*width)continue;
                Candidate x; x.cell=c;x.width=width;x.table.resize(16);
                pool<int> seen;bool valid=true;
                for(int i=0;i<16;i++) {
                    RTLIL::SigSpec key;int value=-1;
                    if(ambiguous.count(s[i]) || !drivers.count(s[i]) ||
                       !equality(drivers.at(s[i]),map,key,value) || seen.count(value)) {valid=false;break;}
                    if(i==0)x.key=key;else if(x.key!=key){valid=false;break;}
                    seen.insert(value);x.table[value]=b.extract(i*width,width);
                    if(x.table[value].has_const()){valid=false;break;}
                }
                pool<RTLIL::SigBit> keybits;
                for(auto bit:x.key)keybits.insert(bit);
                if(valid && GetSize(seen)==16 && GetSize(keybits)==4)candidates.push_back(x);
            }
            pool<RTLIL::Cell *> used;
            dict<RTLIL::SigSpec,RTLIL::SigSpec> decoders;
            for(int i=0;i<GetSize(candidates);i++) {
                auto &a=candidates[i];if(used.count(a.cell))continue;
                for(int j=i+1;j<GetSize(candidates);j++) {
                    auto &b=candidates[j];
                    if(used.count(b.cell)||a.width!=b.width||a.key!=b.key)continue;
                    int chosen=-1,lo_diffs=0,hi_diffs=0;
                    for(int split=0;split<4;split++) {
                        int lo=0,hi=0;
                        for(int n=0;n<16;n++) {
                            if(n&(1<<split))continue;
                            lo+=a.table[n]!=b.table[n|(1<<split)];
                            hi+=b.table[n]!=a.table[n|(1<<split)];
                        }
                        if(lo<=1 && hi<=1){chosen=split;lo_diffs=lo;hi_diffs=hi;break;}
                    }
                    if(chosen<0)continue;
                    RTLIL::SigSpec key,outer=a.key.extract(chosen,1);
                    std::vector<int> bits;
                    for(int k=0;k<4;k++)if(k!=chosen){key.append(a.key.extract(k,1));bits.push_back(k);}
                    RTLIL::SigSpec decode_key=key.extract(1,2);
                    if(!decoders.count(decode_key)) {
                        RTLIL::SigSpec enable;
                        for(int n=0;n<4;n++) {
                            auto wire=m->addWire(NEW_ID);
                            m->addEq(NEW_ID,decode_key,RTLIL::Const(n,2),wire);
                            enable.append(wire);
                        }
                        decoders[decode_key]=enable;
                    }
                    std::vector<RTLIL::SigSpec> left,right;
                    for(int n=0;n<8;n++) {
                        int value=0;for(int k=0;k<3;k++)if(n&(1<<k))value|=1<<bits[k];
                        auto x=a.table[value],y=b.table[value];
                        if(x!=b.table[value|(1<<chosen)]) {
                            auto w=m->addWire(NEW_ID,a.width);
                            m->addMux(NEW_ID,x,b.table[value|(1<<chosen)],outer,w);x=w;
                        }
                        if(y!=a.table[value|(1<<chosen)]) {
                            auto w=m->addWire(NEW_ID,a.width);
                            m->addMux(NEW_ID,y,a.table[value|(1<<chosen)],outer,w);y=w;
                        }
                        left.push_back(x);right.push_back(y);
                    }
                    auto x=read8(m,key,left,a.width,decoders.at(decode_key));
                    auto y=read8(m,key,right,a.width,decoders.at(decode_key));
                    auto ao=a.cell->getPort(ID::Y),bo=b.cell->getPort(ID::Y);
                    m->addMux(NEW_ID,x,y,outer,ao);m->addMux(NEW_ID,y,x,outer,bo);
                    log("  PMUX cofactor pair %s.%s / %s: WIDTH=%d, split=%d, mismatching leaves=%d+%d.\n",
                        log_id(m),log_id(a.cell),log_id(b.cell),a.width,chosen,lo_diffs,hi_diffs);
                    used.insert(a.cell);used.insert(b.cell);m->remove(a.cell);m->remove(b.cell);
                    rewritten++;break;
                }
            }
        }
        if(rewritten)design->scratchpad_set_bool("pmux_v31.changed",true);
        log("pmux_cofactor_share: examined %d original PMUX cells; rewrote %d pairs.\n",examined,rewritten);
    }
} PmuxCofactorSharePass;

// Four decoded choices use a non-disjoint LUT cut: the low selector is
// shared by the inner and outer LUTs. Arithmetic branches stay opaque inputs.
struct PmuxPairCutPass : public Pass {
    using Cell=RTLIL::Cell;using Module=RTLIL::Module;using SigSpec=RTLIL::SigSpec;using SigBit=RTLIL::SigBit;using Const=RTLIL::Const;
    PmuxPairCutPass():Pass("pmux_pair_cut","decompose decoded four-way PMUXes into shared-select LUT pairs"){}
    static bool decode(Cell *c,const SigMap &map,SigSpec &key,int &value) {
        if(c->type==ID($logic_not)) {key=map(c->getPort(ID::A));value=0;return GetSize(key)==2&&!key.has_const();}
        if(c->type!=ID($eq))return false;
        auto a=map(c->getPort(ID::A)),b=map(c->getPort(ID::B));if(a.is_fully_const())std::swap(a,b);
        if(GetSize(a)!=2||GetSize(b)!=2||a.has_const()||!b.is_fully_const()||!b.as_const().is_fully_def())return false;
        key=a;value=b.as_const().as_int(false);return true;
    }
    void execute(std::vector<std::string> args,RTLIL::Design *d)override {
        extra_args(args,1,d);if(d->scratchpad_get_bool("v53.disable_pair"))return;int changed=0;
        for(auto m:d->selected_modules()) {
            if(m->get_blackbox_attribute()||!m->processes.empty()||!m->memories.empty())continue;
            SigMap map(m);dict<SigBit,Cell*> driver;pool<SigBit> duplicate;
            pool<SigBit> mux_outputs,sequential_inputs;dict<SigSpec,Cell*> word_driver;dict<SigBit,int> uses;
            for(auto w:m->wires())if(w->port_output||w->get_bool_attribute(ID::keep))for(auto b:map(w))uses[b]++;
            for(auto cell:m->cells()) {
                if(cell->type==ID($pmux))for(auto b:map(cell->getPort(ID::Y)))mux_outputs.insert(b);
                if(cell->hasPort(ID::D)&&cell->hasPort(ID::Q))for(auto b:map(cell->getPort(ID::D)))sequential_inputs.insert(b);
                if(cell->type.in(ID($xor),ID($and),ID($or),ID($add)))word_driver[map(cell->getPort(ID::Y))]=cell;
                for(auto &port:cell->connections())if(cell->input(port.first))for(auto b:map(port.second))uses[b]++;
            }

            for(auto c:m->cells())if(c->type.in(ID($eq),ID($logic_not))) {
                auto y=map(c->getPort(ID::Y));if(GetSize(y)==1){if(driver.count(y[0]))duplicate.insert(y[0]);else driver[y[0]]=c;}
            }
            for(auto c:m->selected_cells()) {
                if(c->type!=ID($pmux)||c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                int width=c->getParam(ID::WIDTH).as_int(),n=c->getParam(ID::S_WIDTH).as_int();
                if(width<2||width>256||n<3||n>4)continue;
                auto selects=map(c->getPort(ID::S));auto data=map(c->getPort(ID::B));auto fallback=map(c->getPort(ID::A));
                if(GetSize(selects)!=n||GetSize(data)!=n*width||GetSize(fallback)!=width)continue;
                SigSpec key;bool valid=true;std::vector<SigSpec> table(4,fallback);pool<int> seen;
                for(int j=0;j<n;j++) {
                    if(duplicate.count(selects[j])||!driver.count(selects[j])){valid=false;break;}
                    SigSpec k;int value;if(!decode(driver.at(selects[j]),map,k,value)||seen.count(value)){valid=false;break;}
                    if(key.empty())key=k;else if(key!=k){valid=false;break;}
                    seen.insert(value);table[value]=data.extract(j*width,width);
                }
                if(!valid)continue;
                bool nested=false;for(auto bit:map(c->getPort(ID::Y)))if(sequential_inputs.count(bit))nested=true;
                if(nested)continue; // Keep outer muxes visible to ABC9 and FF input absorption.
                RTLIL::IdString factored_type;SigSpec common;bool factored=false;
                if(word_driver.count(table[0])) {
                    auto first=word_driver.at(table[0]);
                    for(auto which:{ID::A,ID::B}) {
                        auto possible=map(first->getPort(which));if(GetSize(possible)!=width)continue;
                        std::vector<SigSpec> alternatives;bool ok=true;
                        for(auto &entry:table) {
                            if(!word_driver.count(entry)){ok=false;break;}auto op=word_driver.at(entry);
                            if(op->type!=first->type||op->get_bool_attribute(ID::keep)||op->get_bool_attribute(RTLIL::escape_id("dont_touch"))){ok=false;break;}
                            auto a=map(op->getPort(ID::A)),b=map(op->getPort(ID::B));
                            if(GetSize(a)!=width||GetSize(b)!=width||GetSize(op->getPort(ID::Y))!=width){ok=false;break;}
                            for(auto bit:entry)if(uses[bit]!=1)ok=false;
                            if(a==possible)alternatives.push_back(b);else if(b==possible)alternatives.push_back(a);else ok=false;
                            if(!ok)break;
                        }
                        if(ok){table=alternatives;common=possible;factored_type=first->type;factored=true;break;}
                    }
                }
                int variables=0,total=0;pool<SigBit> distinct;
                for(auto &entry:table)for(auto b:entry){total++;if(b.wire){variables++;distinct.insert(b);}else if(b!=RTLIL::State::S0&&b!=RTLIL::State::S1)valid=false;}
                if(!valid||variables*4<3*total||GetSize(distinct)<2*width)continue;
                auto type=RTLIL::escape_id("__pmux_pair_cut_"+std::to_string(width));auto lib=d->module(type);
                if(!lib) {
                    lib=d->addModule(type);lib->set_bool_attribute(ID::blackbox);lib->set_bool_attribute(RTLIL::escape_id("pmux_pair_owned"));
                    for(auto name:{ID::A,ID::B,ID::C,ID::D}){auto w=lib->addWire(name,width);w->port_input=true;}
                    auto sel=lib->addWire(ID::S,2);sel->port_input=true;auto y=lib->addWire(ID::Y,width);y->port_output=true;lib->fixup_ports();
                }
                log_assert(lib->get_bool_attribute(RTLIL::escape_id("pmux_pair_owned")));
                auto node=m->addCell(NEW_ID,type);int j=0;for(auto name:{ID::A,ID::B,ID::C,ID::D})node->setPort(name,table[j++]);
                node->setPort(ID::S,key);
                auto output=c->getPort(ID::Y);
                if(factored) {
                    auto selected=m->addWire(NEW_ID,width);node->setPort(ID::Y,selected);
                    auto op=m->addCell(NEW_ID,factored_type);op->setParam(ID::A_WIDTH,width);op->setParam(ID::B_WIDTH,width);op->setParam(ID::Y_WIDTH,width);
                    op->setParam(ID::A_SIGNED,0);op->setParam(ID::B_SIGNED,0);op->setPort(ID::A,selected);op->setPort(ID::B,common);op->setPort(ID::Y,output);
                    log("pmux_pair_cut: moved common %s operation after the decoded selector.\n",log_id(factored_type));
                } else node->setPort(ID::Y,output);
                m->remove(c);changed++;
            }
        }
        if(changed){d->scratchpad_set_bool("pmux_v31.changed",true);d->scratchpad_set_bool("v53.changed",true);}
        log("pmux_pair_cut: decomposed %d decoded four-way PMUXes.\n",changed);
    }
} PmuxPairCutPass;


// Resource sharing is confined to disjoint decoded branches of one PMUX.
struct PmuxAddSubSharePass : public Pass {
    PmuxAddSubSharePass():Pass("pmux_addsub_share","share add/sub operators in mutually exclusive PMUX branches"){}
    void execute(std::vector<std::string> args,RTLIL::Design *d)override {
        extra_args(args,1,d);int changed=0;
        for(auto m:d->selected_modules()) {
            if(m->get_blackbox_attribute())continue;SigMap map(m);dict<RTLIL::SigSpec,RTLIL::Cell*> arith;
            dict<RTLIL::SigBit,RTLIL::Cell*> dec;dict<RTLIL::SigBit,int> uses;
            for(auto w:m->wires())if(w->port_output||w->get_bool_attribute(ID::keep))for(auto b:map(w))uses[b]++;
            for(auto c:m->cells()) {
                if(c->type.in(ID($add),ID($sub)))arith[map(c->getPort(ID::Y))]=c;
                if(c->type.in(ID($eq),ID($logic_not))&&GetSize(c->getPort(ID::Y))==1)dec[map(c->getPort(ID::Y))[0]]=c;
                for(auto &p:c->connections())if(c->input(p.first))for(auto b:map(p.second))uses[b]++;
            }
            for(auto c:m->selected_cells()) {
                if(c->type!=ID($pmux)||c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                int width=c->getParam(ID::WIDTH).as_int(),n=c->getParam(ID::S_WIDTH).as_int();if(width<2||width>256||n<2||n>4)continue;
                auto sel=map(c->getPort(ID::S)),data=map(c->getPort(ID::B));RTLIL::SigSpec key;pool<int> values;bool valid=true;
                for(auto b:sel){RTLIL::SigSpec k;int value;if(!dec.count(b)||!PmuxPairCutPass::decode(dec.at(b),map,k,value)||values.count(value)){valid=false;break;}if(key.empty())key=k;else if(key!=k){valid=false;break;}values.insert(value);}
                if(!valid)continue;bool done=false;
                for(int si=0;si<n&&!done;si++) {
                    auto subword=data.extract(si*width,width);if(!arith.count(subword)||arith.at(subword)->type!=ID($sub))continue;auto sub=arith.at(subword);
                    auto left=map(sub->getPort(ID::A)),common=map(sub->getPort(ID::B));if(GetSize(left)!=width||GetSize(common)!=width)continue;
                    for(int ai=0;ai<n&&!done;ai++) {
                        auto addword=data.extract(ai*width,width);if(!arith.count(addword)||arith.at(addword)->type!=ID($add))continue;auto add=arith.at(addword);
                        if(sub->get_bool_attribute(ID::keep)||add->get_bool_attribute(ID::keep)||sub->get_bool_attribute(RTLIL::escape_id("dont_touch"))||add->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                        auto a=map(add->getPort(ID::A)),b=map(add->getPort(ID::B));if(GetSize(a)!=width||GetSize(b)!=width)continue;
                        RTLIL::SigSpec right;if(a==common)right=b;else if(b==common)right=a;else continue;
                        bool unique=true;for(auto bit:subword)if(uses[bit]!=1)unique=false;for(auto bit:addword)if(uses[bit]!=1)unique=false;if(!unique)continue;
                        int control_bit=-1;for(int k=0;k<2;k++) {
                            RTLIL::SigSpec unused;int sv=0,av=0;PmuxPairCutPass::decode(dec.at(sel[si]),map,unused,sv);PmuxPairCutPass::decode(dec.at(sel[ai]),map,unused,av);
                            if(((sv>>k)&1)!=((av>>k)&1)){control_bit=k;break;}
                        }
                        log_assert(control_bit>=0);RTLIL::SigSpec unused;int sv=0;PmuxPairCutPass::decode(dec.at(sel[si]),map,unused,sv);
                        RTLIL::SigSpec mode=key.extract(control_bit,1);if(!((sv>>control_bit)&1))mode=m->Not(NEW_ID,mode);
                        auto operand=m->addWire(NEW_ID,width),result=m->addWire(NEW_ID,width);m->addMux(NEW_ID,right,left,mode,operand);
                        auto alu=m->addCell(NEW_ID,ID($alu));for(auto p:{ID::A_WIDTH,ID::B_WIDTH,ID::Y_WIDTH})alu->setParam(p,width);alu->setParam(ID::A_SIGNED,0);alu->setParam(ID::B_SIGNED,0);
                        alu->setPort(ID::A,operand);alu->setPort(ID::B,common);alu->setPort(ID::BI,mode);alu->setPort(ID::CI,mode);alu->setPort(ID::Y,result);
                        alu->setPort(ID::X,m->addWire(NEW_ID,width));alu->setPort(ID::CO,m->addWire(NEW_ID,width));
                        auto replacement=c->getPort(ID::B);replacement.replace(si*width,result);replacement.replace(ai*width,result);c->setPort(ID::B,replacement);done=true;changed++;
                    }
                }
            }
        }
        if(changed){d->scratchpad_set_bool("pmux_v31.changed",true);d->scratchpad_set_bool("v53.changed",true);}
        log("pmux_addsub_share: shared %d mutually exclusive add/sub pairs.\n",changed);
    }
} PmuxAddSubSharePass;


struct PmuxNestedAddSubPass : public Pass {
    using Cell=RTLIL::Cell;using SigSpec=RTLIL::SigSpec;using SigBit=RTLIL::SigBit;
    PmuxNestedAddSubPass():Pass("pmux_nested_addsub","share add/sub leaves across disjoint nested PMUX branches"){}
    void execute(std::vector<std::string> args,RTLIL::Design *d)override {
        extra_args(args,1,d);int changed=0;
        for(auto m:d->selected_modules()) {
            if(m->get_blackbox_attribute())continue;SigMap map(m);dict<SigSpec,Cell*> driver;dict<SigBit,Cell*> dec;dict<SigBit,int> uses;std::vector<Cell*> roots;
            for(auto w:m->wires())if(w->port_output||w->get_bool_attribute(ID::keep))for(auto b:map(w))uses[b]++;
            for(auto c:m->cells()) {
                if(c->type.in(ID($pmux),ID($add),ID($sub)))driver[map(c->getPort(ID::Y))]=c;
                if(c->type==ID($pmux))roots.push_back(c);
                if(c->type.in(ID($eq),ID($logic_not))&&GetSize(c->getPort(ID::Y))==1)dec[map(c->getPort(ID::Y))[0]]=c;
                for(auto &p:c->connections())if(c->input(p.first))for(auto b:map(p.second))uses[b]++;
            }
            for(auto root:roots) {
                if(root->get_bool_attribute(ID::keep)||root->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                int width=root->getParam(ID::WIDTH).as_int(),n=root->getParam(ID::S_WIDTH).as_int();if(width<2||width>256||n<3||n>4)continue;
                auto sels=map(root->getPort(ID::S)),data=map(root->getPort(ID::B));SigSpec key;pool<int> values;std::vector<SigSpec> table(4,map(root->getPort(ID::A)));bool valid=true;
                for(int j=0;j<n;j++) {
                    SigSpec k;int v;if(!dec.count(sels[j])||!PmuxPairCutPass::decode(dec.at(sels[j]),map,k,v)||values.count(v)){valid=false;break;}
                    if(key.empty())key=k;else if(key!=k){valid=false;break;}values.insert(v);table[v]=data.extract(j*width,width);
                }
                if(!valid)continue;
                struct Leaf{Cell *cell;int branch;};std::vector<Leaf> leaves;
                std::function<void(SigSpec,int,int)> collect=[&](SigSpec word,int branch,int depth) {
                    if(depth>3||leaves.size()>32)return;word=map(word);if(!driver.count(word))return;auto c=driver.at(word);
                    if(c==root||c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch")))return;
                    for(auto b:word)if(uses[b]!=1)return;
                    if(c->type.in(ID($add),ID($sub))){leaves.push_back({c,branch});return;}
                    if(c->type!=ID($pmux)||c->getParam(ID::WIDTH).as_int()!=width)return;
                    auto choices=map(c->getPort(ID::B));int count=c->getParam(ID::S_WIDTH).as_int();if(count>16)return;
                    collect(c->getPort(ID::A),branch,depth+1);for(int j=0;j<count;j++)collect(choices.extract(j*width,width),branch,depth+1);
                };
                for(int branch=0;branch<4;branch++)collect(table[branch],branch,0);
                Cell *best_sub=nullptr,*best_add=nullptr;int best_bit=0,best_value=0,best_score=100000;SigSpec best_a,best_b,best_common;
                for(auto &s:leaves)if(s.cell->type==ID($sub))for(auto &a:leaves)if(a.cell->type==ID($add)&&s.branch!=a.branch) {
                    auto sa=map(s.cell->getPort(ID::A)),common=map(s.cell->getPort(ID::B)),aa=map(a.cell->getPort(ID::A)),ab=map(a.cell->getPort(ID::B));
                    if(GetSize(sa)!=width||GetSize(common)!=width||GetSize(aa)!=width||GetSize(ab)!=width)continue;
                    SigSpec left;if(aa==common)left=ab;else if(ab==common)left=aa;else continue;
                    int bit=0;while(bit<2&&((s.branch>>bit)&1)==((a.branch>>bit)&1))bit++;if(bit==2)continue;
                    int score=(sa==left?0:width)+(((s.branch>>bit)&1)?0:1);
                    if(score<best_score){best_score=score;best_sub=s.cell;best_add=a.cell;best_bit=bit;best_value=(s.branch>>bit)&1;best_a=left;best_b=sa;best_common=common;}
                }
                if(!best_sub)continue;
                SigSpec mode=key.extract(best_bit,1);if(!best_value)mode=m->Not(NEW_ID,mode);SigSpec operand=best_a;
                if(best_a!=best_b){auto w=m->addWire(NEW_ID,width);m->addMux(NEW_ID,best_a,best_b,mode,w);operand=w;}
                auto y=m->addWire(NEW_ID,width);auto alu=m->addCell(NEW_ID,ID($alu));for(auto p:{ID::A_WIDTH,ID::B_WIDTH,ID::Y_WIDTH})alu->setParam(p,width);alu->setParam(ID::A_SIGNED,0);alu->setParam(ID::B_SIGNED,0);
                alu->setPort(ID::A,operand);alu->setPort(ID::B,best_common);alu->setPort(ID::BI,mode);alu->setPort(ID::CI,mode);alu->setPort(ID::Y,y);alu->setPort(ID::X,m->addWire(NEW_ID,width));alu->setPort(ID::CO,m->addWire(NEW_ID,width));
                for(auto c:{best_sub,best_add}){auto old=map(c->getPort(ID::Y));driver.erase(old);m->connect(c->getPort(ID::Y),y);m->remove(c);}changed++;
            }
        }
        if(changed){d->scratchpad_set_bool("pmux_v31.changed",true);d->scratchpad_set_bool("v53.changed",true);}
        log("pmux_nested_addsub: shared %d branch-constrained arithmetic pairs.\n",changed);
    }
} PmuxNestedAddSubPass;

struct PmuxPairFinishPass : public Pass {
    PmuxPairFinishPass():Pass("pmux_pair_finish","emit deferred shared-select PMUX LUT pairs"){}
    void execute(std::vector<std::string> args,RTLIL::Design *d)override {
        extra_args(args,1,d);int count=0;
        for(auto m:d->selected_modules())for(auto c:m->selected_cells()) {
            auto lib=d->module(c->type);if(!lib||!lib->get_bool_attribute(RTLIL::escape_id("pmux_pair_owned")))continue;
            auto a=c->getPort(ID::A),b=c->getPort(ID::B),v=c->getPort(ID::C),z=c->getPort(ID::D),s=c->getPort(ID::S),y=c->getPort(ID::Y);
            log_assert(GetSize(s)==2&&GetSize(a)==GetSize(y)&&GetSize(b)==GetSize(y)&&GetSize(v)==GetSize(y)&&GetSize(z)==GetSize(y));
            for(int bit=0;bit<GetSize(y);bit++) {
                RTLIL::SigSpec inner;inner.append(b[bit]);inner.append(s[0]);inner.append(s[1]);inner.append(z[bit]);
                auto g=m->addWire(NEW_ID);m->addLut(NEW_ID,inner,g,RTLIL::Const(0xf838,16));
                RTLIL::SigSpec outer;outer.append(a[bit]);outer.append(v[bit]);outer.append(s[0]);outer.append(g);
                m->addLut(NEW_ID,outer,y.extract(bit,1),RTLIL::Const(0xfc0a,16));count+=2;
            }
            m->remove(c);
        }
        std::vector<RTLIL::Module*> unused;
        for(auto lib:d->modules())if(lib->get_bool_attribute(RTLIL::escape_id("pmux_pair_owned"))) {
            bool used=false;for(auto m:d->modules())for(auto c:m->cells())if(c->type==lib->name)used=true;if(!used)unused.push_back(lib);
        }
        for(auto lib:unused)d->remove(lib);
        log("pmux_pair_finish: emitted %d exact LUT4 cuts.\n",count);
    }
} PmuxPairFinishPass;

PRIVATE_NAMESPACE_END
