#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct CrossSharePass : public Pass
{
    CrossSharePass() : Pass("pmux_cross_share", "share near-identical read trees across crossed output muxes") {}
    void help() override
    {
        log("\n    pmux_cross_share [selection]\n\n");
        log("Match two mux outputs with the same outer selector and complete\n");
        log("eight-way subtrees. When crossed subtrees differ in at most one\n");
        log("leaf each, push the outer selector into those leaves and share\n");
        log("the two resulting trees. Width 8..512; Cyclone IV LUT4 target.\n\n");
    }
    struct Tree {
        RTLIL::SigSpec select[3];
        std::vector<RTLIL::SigSpec> leaf;
    };
    struct Root {
        RTLIL::Cell *cell;
        RTLIL::SigSpec outer;
        int width;
        Tree side[2];
    };
    static bool collect(RTLIL::SigSpec y, int level, int width,
                        const dict<RTLIL::SigSpec, RTLIL::Cell *> &drivers,
                        const SigMap &map, Tree &tree)
    {
        if (level == 3) {
            if (drivers.count(y)) return false;
            tree.leaf.push_back(y);
            return true;
        }
        if (!drivers.count(y)) return false;
        auto cell = drivers.at(y);
        if (cell->getParam(ID::WIDTH).as_int() != width) return false;
        auto s = map(cell->getPort(ID::S));
        if (GetSize(s) != 1 || s[0].wire == nullptr) return false;
        if (tree.select[level].empty()) tree.select[level] = s;
        else if (tree.select[level] != s) return false;
        auto a = map(cell->getPort(ID::A)), b = map(cell->getPort(ID::B));
        if (GetSize(a) != width || GetSize(b) != width) return false;
        return collect(a, level+1, width, drivers, map, tree) &&
               collect(b, level+1, width, drivers, map, tree);
    }
    static RTLIL::SigSpec lut4(RTLIL::Module *module, RTLIL::SigSpec input, int mask)
    {
        log_assert(GetSize(input) == 4);
        auto y = module->addWire(NEW_ID);
        auto cell = module->addCell(NEW_ID, RTLIL::escape_id("cycloneiv_lcell_comb"));
        cell->setParam(RTLIL::escape_id("lut_mask"), RTLIL::Const(mask, 16));
        cell->setParam(RTLIL::escape_id("sum_lutc_input"), RTLIL::Const("datac"));
        const char *names[] = {"dataa", "datab", "datac", "datad"};
        for (int i = 0; i < 4; i++) cell->setPort(RTLIL::escape_id(names[i]), input.extract(i, 1));
        cell->setPort(RTLIL::escape_id("cin"), RTLIL::State::S0);
        cell->setPort(RTLIL::escape_id("combout"), y);
        cell->setPort(RTLIL::escape_id("cout"), module->addWire(NEW_ID));
        return y;
    }
    static int differences(const Tree &a, const Tree &b)
    {
        int count = 0;
        for (int i = 0; i < 8; i++) if (a.leaf[i] != b.leaf[i]) count++;
        return count;
    }
    static RTLIL::SigSpec merge_tree(RTLIL::Module *module, const Tree &low,
            const Tree &high, RTLIL::SigSpec outer, int width, RTLIL::SigSpec enables)
    {
        std::vector<RTLIL::SigSpec> leaves;
        for (int i = 0; i < 8; i++) {
            auto value = low.leaf[i];
            if (value != high.leaf[i]) {
                auto merged = module->addWire(NEW_ID, width);
                module->addMux(NEW_ID, value, high.leaf[i], outer, merged);
                value = merged;
            }
            leaves.push_back(value);
        }
        RTLIL::SigSpec result;
        for (int bit = 0; bit < width; bit++) {
            RTLIL::SigSpec terms;
            for (int pair = 0; pair < 4; pair++) {
                RTLIL::SigSpec input(leaves[2*pair][bit]);
                input.append(leaves[2*pair+1][bit]);
                input.append(low.select[2]);
                input.append(enables[pair]);
                // enable & (last_selector ? high_data : low_data)
                terms.append(lut4(module, input, 0xCA00));
            }
            result.append(lut4(module, terms, 0xFFFE));
        }
        return result;
    }
    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        extra_args(args, 1, design);
        log_header(design, "Executing PMUX_CROSS_SHARE pass.\n");
        int rewritten = 0;
        for (auto module : design->selected_modules()) {
            int eligible=0;
            for(auto c:module->selected_cells())if(c->type==ID($mux)) {
                int width=c->getParam(ID::WIDTH).as_int();
                if(width>=8&&width<=512)eligible++;
            }
            if(eligible<2)continue;
            SigMap map(module);
            dict<RTLIL::SigSpec, RTLIL::Cell *> drivers;
            pool<RTLIL::SigSpec> duplicate;
            for (auto cell : module->cells()) {
                if (cell->type != ID($mux)) continue;
                auto y = map(cell->getPort(ID::Y));
                if (drivers.count(y)) duplicate.insert(y);
                else drivers[y] = cell;
            }
            for (auto y : duplicate) drivers.erase(y);
            std::vector<Root> roots;
            for (auto cell : module->selected_cells()) {
                if (cell->type != ID($mux)) continue;
                Root root;
                root.cell = cell;
                root.width = cell->getParam(ID::WIDTH).as_int();
                root.outer = map(cell->getPort(ID::S));
                if (root.width < 8 || root.width > 512 || GetSize(root.outer) != 1 ||
                    root.outer[0].wire == nullptr) continue;
                if (!collect(map(cell->getPort(ID::A)), 0, root.width, drivers, map, root.side[0]) ||
                    !collect(map(cell->getPort(ID::B)), 0, root.width, drivers, map, root.side[1])) continue;
                pool<RTLIL::SigSpec> selects;
                bool valid = true;
                selects.insert(root.outer);
                for (int level = 0; level < 3; level++) {
                    if (root.side[0].select[level] != root.side[1].select[level]) valid = false;
                    selects.insert(root.side[0].select[level]);
                }
                if (valid && GetSize(selects) == 4) roots.push_back(root);
            }
            pool<RTLIL::Cell *> used;
            dict<RTLIL::SigSpec, RTLIL::SigSpec> decoder_cache;
            for (int i = 0; i < GetSize(roots); i++) {
                auto &a = roots[i];
                if (used.count(a.cell)) continue;
                for (int j = i+1; j < GetSize(roots); j++) {
                    auto &b = roots[j];
                    if (used.count(b.cell) || a.width != b.width || a.outer != b.outer) continue;
                    bool same = true;
                    for (int k = 0; k < 3; k++)
                        if (a.side[0].select[k] != b.side[0].select[k]) same = false;
                    if (!same) continue;
                    int d0 = differences(a.side[0], b.side[1]);
                    int d1 = differences(b.side[0], a.side[1]);
                    if (d0 > 1 || d1 > 1 || d0+d1 == 0) continue;
                    RTLIL::SigSpec key = a.side[0].select[0];
                    key.append(a.side[0].select[1]);
                    if (!decoder_cache.count(key)) {
                        RTLIL::SigSpec enables;
                        for (int pair = 0; pair < 4; pair++) {
                            auto e = module->addWire(NEW_ID);
                            int code = ((pair & 1) << 1) | ((pair & 2) >> 1);
                            module->addEq(NEW_ID, key, RTLIL::Const(code, 2), e);
                            enables.append(e);
                        }
                        decoder_cache[key] = enables;
                    }
                    auto x = merge_tree(module, a.side[0], b.side[1], a.outer, a.width, decoder_cache.at(key));
                    auto y = merge_tree(module, b.side[0], a.side[1], a.outer, a.width, decoder_cache.at(key));
                    a.cell->setPort(ID::A, x); a.cell->setPort(ID::B, y);
                    b.cell->setPort(ID::A, y); b.cell->setPort(ID::B, x);
                    used.insert(a.cell); used.insert(b.cell);
                    rewritten++;
                    log("  Cross-sharing WIDTH=%d, differing leaves=%d+%d.\n", a.width, d0, d1);
                    break;
                }
            }
        }
        if(rewritten)design->scratchpad_set_bool("pmux_v31.changed",true);
        log("pmux_cross_share: rewrote %d crossed tree pairs.\n", rewritten);
    }
} CrossSharePass;
PRIVATE_NAMESPACE_END

