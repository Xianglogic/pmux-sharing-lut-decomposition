#include "kernel/yosys.h"
#include "kernel/sigtools.h"

#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct OnehotReadPass : public Pass
{
	OnehotReadPass() : Pass("pmux_shared_read", "pair decoded read terms into Cyclone IV LUT4 primitives") {}

	void help() override
	{
		log("\n    pmux_shared_read [selection]\n\n");
		log("Replace a complete three-level, same-width binary mux read tree\n");
		log("with four two-term sums and a final four-way OR. Invoke after memory_map.\n");
		log("Each tree level must share one selector bit across its nodes.\n");
		log("All eight minterms are generated from the original selector bits.\n");
		log("Only widths of at least eight bits are considered.\n\n");
	}

	struct Tree
	{
		RTLIL::SigSpec selectors[3];
		std::vector<RTLIL::SigSpec> leaves;
	};

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
		RTLIL::Wire *unused_cout = module->addWire(NEW_ID);
		cell->setPort(RTLIL::escape_id("cout"), unused_cout);
		return cell;
	}

	static bool collect(const RTLIL::SigSpec &signal, int level, int width,
			const dict<RTLIL::SigSpec, RTLIL::Cell *> &drivers,
			const SigMap &sigmap, Tree &tree)
	{
		if (level == 3) {
			if (drivers.count(signal))
				return false;
			tree.leaves.push_back(signal);
			return true;
		}
		if (!drivers.count(signal))
			return false;
		RTLIL::Cell *cell = drivers.at(signal);
		if (cell->type != ID($mux) || !cell->hasPort(ID::S) ||
				!cell->hasPort(ID::A) || !cell->hasPort(ID::B) ||
				cell->getParam(ID::WIDTH).as_int() != width)
			return false;
		RTLIL::SigSpec select = sigmap(cell->getPort(ID::S));
		if (GetSize(select) != 1 || select[0].wire == nullptr)
			return false;
		if (tree.selectors[level].empty())
			tree.selectors[level] = select;
		else if (tree.selectors[level] != select)
			return false;
		RTLIL::SigSpec a = sigmap(cell->getPort(ID::A));
		RTLIL::SigSpec b = sigmap(cell->getPort(ID::B));
		return GetSize(a) == width && GetSize(b) == width &&
			collect(a, level + 1, width, drivers, sigmap, tree) &&
			collect(b, level + 1, width, drivers, sigmap, tree);
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		size_t argidx = 1;
		extra_args(args, argidx, design);
		log_header(design, "Executing PMUX_SHARED_READ pass.\n");
		int examined = 0, rewritten = 0;
		for (auto module : design->selected_modules()) {
			std::vector<RTLIL::Cell *> muxes;
			for (auto cell : module->selected_cells())
				if (cell->type == ID($mux)) muxes.push_back(cell);
			bool eligible=false;
			for(auto c:muxes) {
				int width=c->getParam(ID::WIDTH).as_int();
				if(width>=8&&width<=512){eligible=true;break;}
			}
			if(!eligible){examined+=GetSize(muxes);continue;}
			SigMap sigmap(module);
			dict<RTLIL::SigSpec, RTLIL::Cell *> drivers;
			pool<RTLIL::SigSpec> duplicate_outputs;
			for (auto cell : module->cells()) {
				if (cell->type != ID($mux) || !cell->hasPort(ID::Y))
					continue;
				RTLIL::SigSpec y = sigmap(cell->getPort(ID::Y));
				if (drivers.count(y)) duplicate_outputs.insert(y);
				else drivers[y] = cell;
			}
			for (auto y : duplicate_outputs) drivers.erase(y);
			dict<RTLIL::SigSpec, RTLIL::SigSpec> decode_cache;
			dict<RTLIL::SigSpec, RTLIL::SigSpec> pair_cache;
			for (auto cell : muxes) {
				examined++;
				int width = cell->getParam(ID::WIDTH).as_int();
				if (width < 8 || width > 512)
					continue;
				Tree tree;
				RTLIL::SigSpec y = sigmap(cell->getPort(ID::Y));
				if (!collect(y, 0, width, drivers, sigmap, tree) ||
						GetSize(tree.leaves) != 8)
					continue;
				if (tree.selectors[0] == tree.selectors[1] ||
						tree.selectors[0] == tree.selectors[2] ||
						tree.selectors[1] == tree.selectors[2])
					continue;
				RTLIL::SigSpec key;
				for (int i = 0; i < 3; i++) key.append(tree.selectors[i]);
				RTLIL::SigSpec decoded;
				if (decode_cache.count(key)) decoded = decode_cache.at(key);
				else {
					RTLIL::SigSpec inverted[3];
					for (int i = 0; i < 3; i++) {
						RTLIL::Wire *wire = module->addWire(NEW_ID);
						module->addNot(NEW_ID, tree.selectors[i], wire);
						inverted[i] = wire;
					}
					for (int branch = 0; branch < 8; branch++) {
						RTLIL::SigSpec term;
						for (int level = 0; level < 3; level++)
							term.append((branch & (1 << (2 - level))) ?
								tree.selectors[level] : inverted[level]);
						RTLIL::Wire *wire = module->addWire(NEW_ID);
						module->addReduceAnd(NEW_ID, term, wire);
						decoded.append(wire);
					}
					decode_cache[key] = decoded;
				}
			RTLIL::SigSpec output;
			for (int bit = 0; bit < width; bit++) {
				RTLIL::SigSpec partials;
				for (int pair = 0; pair < 4; pair++) {
					RTLIL::SigSpec inputs;
					inputs.append(tree.leaves[2 * pair].extract(bit, 1));
					inputs.append(tree.leaves[2 * pair + 1].extract(bit, 1));
					inputs.append(decoded.extract(2 * pair, 1));
					inputs.append(decoded.extract(2 * pair + 1, 1));
					if (!pair_cache.count(inputs)) {
						RTLIL::Wire *partial = module->addWire(NEW_ID);
						add_lut4(module, inputs, partial, 0xECA0);
						pair_cache[inputs] = partial;
					}
					partials.append(pair_cache.at(inputs));
				}
				RTLIL::Wire *result_bit = module->addWire(NEW_ID);
				add_lut4(module, partials, result_bit, 0xFFFE);
				output.append(result_bit);
			}
			RTLIL::SigSpec original_y = cell->getPort(ID::Y);
			module->remove(cell);
			module->connect(original_y, output);
			drivers.erase(y);
			rewritten++;
		}
		if (rewritten)
			log("  %s: factored eight-way read trees with LUT4 pairs.\n", log_id(module));
		}
        if(rewritten)design->scratchpad_set_bool("pmux_v31.changed",true);
		log("pmux_shared_read: examined %d muxes, rewrote %d eight-way trees.\n",
			examined, rewritten);
	}
} OnehotReadPass;

PRIVATE_NAMESPACE_END

