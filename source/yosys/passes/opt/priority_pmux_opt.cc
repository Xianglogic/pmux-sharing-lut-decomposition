#include "kernel/yosys.h"
#include "kernel/sigtools.h"

#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct PriorityPmuxOptPass : public Pass
{
	PriorityPmuxOptPass() : Pass("priority_pmux_opt", "fold constant-default priority muxes into LUT pairs") {}

	void help() override
	{
		log("\n    priority_pmux_opt [selection]\n\n");
		log("Recognize three or four same-width priority muxes with a known constant default.\n");
		log("The B branches are ordered high to low priority and the A\n");
		log("branch carries the lower-priority result. For width 1..256,\n");
		log("encode priority locally in two LUT pairs per data bit and\n");
		log("combine the pairs using a final priority-aware LUT4. Three levels\n");
		log("share an upper-pair valid predicate (minimum width two).\n\n");
	}

	static RTLIL::Cell *add_lut4(RTLIL::Module *module,
			const RTLIL::SigSpec &inputs, const RTLIL::SigSpec &output, int mask)
	{
		log_assert(GetSize(inputs) == 4 && GetSize(output) == 1);
		auto cell = module->addCell(NEW_ID, RTLIL::escape_id("cycloneiv_lcell_comb"));
		cell->setParam(RTLIL::escape_id("lut_mask"), RTLIL::Const(mask, 16));
		cell->setParam(RTLIL::escape_id("sum_lutc_input"), RTLIL::Const("datac"));
		cell->setPort(RTLIL::escape_id("dataa"), inputs.extract(0, 1));
		cell->setPort(RTLIL::escape_id("datab"), inputs.extract(1, 1));
		cell->setPort(RTLIL::escape_id("datac"), inputs.extract(2, 1));
		cell->setPort(RTLIL::escape_id("datad"), inputs.extract(3, 1));
		cell->setPort(RTLIL::escape_id("cin"), RTLIL::State::S0);
		cell->setPort(RTLIL::escape_id("combout"), output);
		cell->setPort(RTLIL::escape_id("cout"), module->addWire(NEW_ID));
		return cell;
	}


    static RTLIL::SigSpec word_lut(RTLIL::Module *m,const RTLIL::SigSpec &pins,const std::vector<int> &masks) {
        int width=GetSize(masks);log_assert(GetSize(pins)==4*width);
        auto type=RTLIL::escape_id("__pmux_priority_lut_"+std::to_string(width));auto d=m->design;
        if(!d->module(type)) {
            auto lib=d->addModule(type);lib->set_bool_attribute(ID::blackbox);
            lib->set_bool_attribute(RTLIL::escape_id("pmux_owned_priority"));
            auto a=lib->addWire(ID::A,4*width);a->port_input=true;
            auto y=lib->addWire(ID::Y,width);y->port_output=true;lib->fixup_ports();
            lib->parameter_default_values[ID::LUT]=RTLIL::Const(0,16*width);
        }
        log_assert(d->module(type)->get_bool_attribute(RTLIL::escape_id("pmux_owned_priority")));
        std::vector<RTLIL::State> bits;for(auto mask:masks)for(int j=0;j<16;j++)bits.push_back((mask>>j)&1?RTLIL::State::S1:RTLIL::State::S0);
        auto out=m->addWire(NEW_ID,width);auto c=m->addCell(NEW_ID,type);c->setPort(ID::A,pins);c->setPort(ID::Y,out);c->setParam(ID::LUT,RTLIL::Const(bits));return out;
    }
	struct Chain
	{
		RTLIL::SigSpec select[4];
		RTLIL::SigSpec data[4];
		RTLIL::SigSpec fallback;
		int depth = 0;
	};

	static bool match(const RTLIL::SigSpec &root_y, int width,
			const dict<RTLIL::SigSpec, RTLIL::Cell *> &drivers,
			const SigMap &sigmap, Chain &chain)
	{
		RTLIL::SigSpec current = root_y;
		for (int i = 0; i < 4; i++) {
			if (!drivers.count(current)) return false;
			RTLIL::Cell *cell = drivers.at(current);
			if (cell->type != ID($mux) ||
					cell->getParam(ID::WIDTH).as_int() != width ||
					cell->get_bool_attribute(ID::keep) || cell->get_bool_attribute(RTLIL::escape_id("dont_touch")))
				return false;
			chain.select[i] = sigmap(cell->getPort(ID::S));
			chain.data[i] = sigmap(cell->getPort(ID::B));
			if (GetSize(chain.select[i]) != 1 ||
					chain.select[i][0].wire == nullptr ||
					GetSize(chain.data[i]) != width ||
					chain.data[i].has_const())
				return false;
			current = sigmap(cell->getPort(ID::A));
			if (GetSize(current) != width) return false;
			if (current.is_fully_const()) {
				if (!current.as_const().is_fully_def() || i < 2) return false;
				chain.depth = i + 1;
				chain.fallback = current;
				return chain.depth == 4 || width >= 2;
			}
		}
		return false;
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		extra_args(args, 1, design);
		log_header(design, "Executing PRIORITY_PMUX_OPT pass.\n");
		int examined = 0, rewritten = 0;
		for (auto module : design->selected_modules()) {
			int eligible=0;
            for(auto c:module->selected_cells())if(c->type==ID($mux)) {
                int width=c->getParam(ID::WIDTH).as_int();
                if(width>=1&&width<=256)eligible++;
            }
            if(eligible<3)continue;
			SigMap sigmap(module);
			dict<RTLIL::SigSpec, RTLIL::Cell *> drivers;
			pool<RTLIL::SigSpec> duplicate;
			for (auto cell : module->cells()) {
				if (cell->type != ID($mux)) continue;
				auto y = sigmap(cell->getPort(ID::Y));
				if (drivers.count(y)) duplicate.insert(y);
				else drivers[y] = cell;
			}
			for (auto y : duplicate) drivers.erase(y);
			std::vector<RTLIL::Cell *> roots;
			for (auto cell : module->selected_cells())
				if (cell->type == ID($mux)) roots.push_back(cell);
			// Snapshot all matches before deleting cells: a three-level suffix may
			// also belong to a four-level chain. No stale driver pointers are used.
			std::vector<std::pair<RTLIL::Cell *, Chain>> plans;
			pool<RTLIL::SigBit> protected_bits;
			for (auto wire : module->wires())
				if (wire->get_bool_attribute(ID::keep) || wire->get_bool_attribute(RTLIL::escape_id("dont_touch")))
					for (auto bit : sigmap(wire)) protected_bits.insert(bit);
			for (auto root : roots) {
				examined++;
				int width = root->getParam(ID::WIDTH).as_int();
				if (width < 1 || width > 256) continue;
				bool protect = false;
				for (auto bit : sigmap(root->getPort(ID::Y)))
					if (protected_bits.count(bit)) protect = true;
				if (protect) continue;
				Chain chain;
				if (!match(sigmap(root->getPort(ID::Y)), width,
						drivers, sigmap, chain)) continue;
				plans.emplace_back(root, chain);
			}
            // A matched suffix with a single matched-parent consumer is
            // removed by the parent rewrite. Do not construct speculative LUTs.
            dict<RTLIL::SigBit,int> uses;pool<RTLIL::Cell*> planned,subsumed;
            for(auto &p:plans)planned.insert(p.first);
            for(auto c:module->cells())for(auto &p:c->connections())if(c->input(p.first))for(auto b:sigmap(p.second))uses[b]++;
            for(auto w:module->wires())if(w->port_output||w->get_bool_attribute(ID::keep))for(auto b:sigmap(w))uses[b]++;
            for(auto &p:plans) {
                auto a=sigmap(p.first->getPort(ID::A));if(!drivers.count(a)||!planned.count(drivers.at(a)))continue;
                bool only_parent=true;for(auto b:a)if(uses[b]!=1)only_parent=false;
                if(only_parent)subsumed.insert(drivers.at(a));
            }
			for (auto &plan : plans) {
				auto root = plan.first;
                if(subsumed.count(root))continue;
				auto &chain = plan.second;
				int width = root->getParam(ID::WIDTH).as_int();

                // Encode four priority choices plus a uniform constant default
                // into shared mode/code bits. Each data plane then uses two LUT4s.
                bool uniform=true;auto fallback=chain.fallback[0];for(auto bit:chain.fallback)if(bit!=fallback)uniform=false;
                if(chain.depth==4&&width>=4&&uniform) {
                    RTLIL::SigSpec control;for(int k=0;k<4;k++)control.append(chain.select[k]);
                    int masks[3]={0,0,0};
                    for(int x=0;x<16;x++) {
                        int branch=4;for(int k=0;k<4;k++)if((x>>k)&1){branch=k;break;}
                        int code=branch==0?0:branch==1?2:branch==2?1:branch==3?3:(fallback==RTLIL::State::S1?1:0);
                        bool mode=branch==1||branch==3||branch==4;
                        for(int k=0;k<2;k++)if((code>>k)&1)masks[k]|=1<<x;
                        if(mode)masks[2]|=1<<x;
                    }
                    RTLIL::SigSpec code[3];for(int k=0;k<3;k++)code[k]=word_lut(module,control,std::vector<int>{masks[k]});
                    int inner_mask=0,outer_mask=0;
                    for(int x=0;x<16;x++) {
                        int select=(x>>2)&3;bool g=select==0?false:select==1?true:select==2?bool(x&1):bool(x&2);
                        if(g)inner_mask|=1<<x;
                        bool y=(x&4)?bool(x&8):((x&8)?bool(x&2):bool(x&1));if(y)outer_mask|=1<<x;
                    }
                    RTLIL::SigSpec inner;
                    for(int bit=0;bit<width;bit++){inner.append(chain.data[1].extract(bit,1));inner.append(chain.data[3].extract(bit,1));inner.append(code[0]);inner.append(code[1]);}
                    auto g=word_lut(module,inner,std::vector<int>(width,inner_mask));RTLIL::SigSpec outer;
                    for(int bit=0;bit<width;bit++){outer.append(chain.data[0].extract(bit,1));outer.append(chain.data[2].extract(bit,1));outer.append(code[2]);outer.append(g.extract(bit,1));}
                    auto output=word_lut(module,outer,std::vector<int>(width,outer_mask));auto y=root->getPort(ID::Y);
                    module->remove(root);module->connect(y,output);drivers.erase(sigmap(y));rewritten++;
                    log("priority_pmux_opt: shared five-state encoding for %d data planes.\n",width);continue;
                }
			RTLIL::SigSpec upper_valid;
			if (chain.depth == 3) {
				upper_valid = module->addWire(NEW_ID);
				module->addOr(NEW_ID, chain.select[0], chain.select[1], upper_valid);
			}
            RTLIL::SigSpec pairs[2];
            for(int pair=0;pair<(chain.depth==4?2:1);pair++) {
                RTLIL::SigSpec pins;std::vector<int> masks;
                for(int bit=0;bit<width;bit++) {
                    pins.append(chain.data[2*pair].extract(bit,1));pins.append(chain.data[2*pair+1].extract(bit,1));
                    pins.append(chain.select[2*pair]);pins.append(chain.select[2*pair+1]);
                    masks.push_back(0xACA0|((pair==1&&chain.fallback[bit]==RTLIL::State::S1)?0x000F:0));
                }
                pairs[pair]=word_lut(module,pins,masks);
            }
            RTLIL::SigSpec pins;std::vector<int> masks;
            for(int bit=0;bit<width;bit++) {
                pins.append(pairs[0].extract(bit,1));
                if(chain.depth==4) {
                    pins.append(pairs[1].extract(bit,1));pins.append(chain.select[0]);pins.append(chain.select[1]);masks.push_back(0xAAAE);
                } else {
                    pins.append(upper_valid);pins.append(chain.data[2].extract(bit,1));pins.append(chain.select[2]);
                    int fallback=chain.fallback[bit]==RTLIL::State::S1,mask=0;
                    for(int x=0;x<16;x++)if((x&2)?(x&1):((x&8)?(x&4):fallback))mask|=1<<x;
                    masks.push_back(mask);
                }
            }
            RTLIL::SigSpec output=word_lut(module,pins,masks);
			RTLIL::SigSpec y = root->getPort(ID::Y);
			module->remove(root);
			module->connect(y, output);
			drivers.erase(sigmap(y));
			rewritten++;
		}
		}
        if(rewritten)design->scratchpad_set_bool("pmux_v31.changed",true);
		log("priority_pmux_opt: examined %d muxes; rewrote %d constant-default priority chains.\n",
			examined, rewritten);
	}
} PriorityPmuxOptPass;


struct PmuxPriorityFinishPass : public Pass {
    PmuxPriorityFinishPass():Pass("pmux_priority_finish","emit deferred word-parallel priority LUT planes"){}
    void execute(std::vector<std::string> args,RTLIL::Design *d)override {
        extra_args(args,1,d);int count=0;
        for(auto m:d->selected_modules())for(auto c:m->selected_cells()) {
            auto lib=d->module(c->type);if(!lib||!lib->get_bool_attribute(RTLIL::escape_id("pmux_owned_priority")))continue;
            auto a=c->getPort(ID::A),y=c->getPort(ID::Y);auto masks=c->getParam(ID::LUT);
            log_assert(GetSize(a)==4*GetSize(y)&&GetSize(masks)==16*GetSize(y)&&masks.is_fully_def());
            for(int bit=0;bit<GetSize(y);bit++){m->addLut(NEW_ID,a.extract(4*bit,4),y.extract(bit,1),masks.extract(16*bit,16));count++;}
            m->remove(c);
        }
        std::vector<RTLIL::Module*> unused;
        for(auto lib:d->modules())if(lib->get_bool_attribute(RTLIL::escape_id("pmux_owned_priority"))) {
            bool used=false;for(auto m:d->modules())for(auto c:m->cells())if(c->type==lib->name)used=true;
            if(!used)unused.push_back(lib);
        }
        for(auto m:unused)d->remove(m);
        log("pmux_priority_finish: emitted %d priority LUT planes.\n",count);
    }
} PmuxPriorityFinishPass;
PRIVATE_NAMESPACE_END
