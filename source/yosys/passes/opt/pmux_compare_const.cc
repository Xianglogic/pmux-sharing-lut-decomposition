#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/ff.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct PmuxCompareConstPass : public Pass
{
    PmuxCompareConstPass() : Pass("pmux_compare_const", "propagate equality constants into selected mux arms") {}

    void help() override
    {
        log("\n    pmux_compare_const [-preserve-powerup-low] [selection]\n\n");
        log("When a mux arm is selected by equality to a fully defined constant,\n");
        log("replace copies of compared input bits in that arm by their known\n");
        log("values. Equality widths must match and duplicate bits must agree.\n\n");
        log("-preserve-powerup-low records the default Intel low power-up only\n");
        log("for uninitialized FF bits whose nonconstant D becomes constant one.\n");
    }

    static bool forced_bits(RTLIL::Cell *compare, const SigMap &sigmap,
                            dict<RTLIL::SigBit, RTLIL::SigBit> &known)
    {
        if (compare->type != ID($eq) ||
            compare->getParam(ID::Y_WIDTH).as_int() != 1 ||
            compare->getParam(ID::A_WIDTH).as_int() != compare->getParam(ID::B_WIDTH).as_int())
            return false;
        RTLIL::SigSpec a = sigmap(compare->getPort(ID::A));
        RTLIL::SigSpec b = sigmap(compare->getPort(ID::B));
        RTLIL::SigSpec vars, values;
        if (a.is_fully_const() && !b.is_fully_const()) {
            vars = b;
            values = a;
        } else if (b.is_fully_const() && !a.is_fully_const()) {
            vars = a;
            values = b;
        } else {
            return false;
        }
        if (GetSize(vars) != GetSize(values)) return false;
        for (int i = 0; i < GetSize(vars); i++) {
            if (values[i] != RTLIL::State::S0 && values[i] != RTLIL::State::S1)
                return false;
            if (vars[i].wire == nullptr) continue;
            if (known.count(vars[i]) && known.at(vars[i]) != values[i])
                return false;
            known[vars[i]] = values[i];
        }
        return !known.empty();
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        bool preserve_powerup_low = false;
        size_t argidx = 1;
        if (argidx < args.size() && args[argidx] == "-preserve-powerup-low") {
            preserve_powerup_low = true;
            argidx++;
        }
        extra_args(args, argidx, design);
        log_header(design, "Executing PMUX_COMPARE_CONST pass.\n");
        int changed_arms = 0, changed_bits = 0, projected_bits = 0, redundant_arms = 0;
        int retained_bits = 0;
        for (auto module : design->selected_modules()) {
            bool has_eq=false, has_mux=false;
            for(auto c:module->cells())if(c->type==ID($eq)){has_eq=true;break;}
            if(!has_eq)continue;
            for(auto c:module->selected_cells())if(c->type==ID($mux)||c->type==ID($pmux)){has_mux=true;break;}
            if(!has_mux)continue;
            SigMap sigmap(module);
            pool<SigBit> uninitialized_nonconstant_q;
            if (preserve_powerup_low) {
                FfInitVals init(&sigmap, module);
                for (auto ffcell : module->cells()) {
                    if (!RTLIL::builtin_ff_cell_types().count(ffcell->type)) continue;
                    FfData ff(&init, ffcell);
                    if (!ff.has_clk || ff.has_arst || ff.has_srst || ff.has_sr || ff.has_aload || GetSize(ff.sig_d) != ff.width) continue;
                    auto d = sigmap(ff.sig_d), q = sigmap(ff.sig_q);
                    for (int bit = 0; bit < ff.width; bit++)
                        if (q[bit].wire && d[bit].wire &&
                            ff.val_init[bit] != State::S0 && ff.val_init[bit] != State::S1)
                            uninitialized_nonconstant_q.insert(q[bit]);
                }
            }
            int before_changes = changed_bits + projected_bits + redundant_arms;
            dict<RTLIL::SigBit, RTLIL::Cell *> drivers;
            pool<RTLIL::SigBit> duplicate;
            for (auto cell : module->cells()) {
                if (cell->type != ID($eq)) continue;
                RTLIL::SigSpec y = sigmap(cell->getPort(ID::Y));
                if (GetSize(y) != 1) continue;
                if (drivers.count(y[0])) duplicate.insert(y[0]);
                else drivers[y[0]] = cell;
            }
            for (auto bit : duplicate) drivers.erase(bit);
            for (auto cell : module->selected_cells()) {
                if (cell->type != ID($pmux) && cell->type != ID($mux)) continue;
                RTLIL::SigSpec selects = sigmap(cell->getPort(ID::S));
                RTLIL::SigSpec raw_data = cell->getPort(ID::B);
                RTLIL::SigSpec data = sigmap(raw_data);
                RTLIL::SigSpec raw_selects = cell->getPort(ID::S);
                RTLIL::SigSpec default_data = sigmap(cell->getPort(ID::A));
                const int width = cell->getParam(ID::WIDTH).as_int();
                if (GetSize(data) != width * GetSize(selects)) continue;
                for (int arm = 0; arm < GetSize(selects); arm++) {
                    if (!drivers.count(selects[arm])) continue;
                    dict<RTLIL::SigBit, RTLIL::SigBit> known;
                    if (!forced_bits(drivers.at(selects[arm]), sigmap, known)) continue;
                    int arm_changes = 0;
                    for (int bit = 0; bit < width; bit++) {
                        int pos = arm * width + bit;
                        if (known.count(data[pos]) && raw_data[pos] != known.at(data[pos])) {
                            raw_data[pos] = known.at(data[pos]);
                            arm_changes++;
                        }
                    }
                    if (arm_changes) {
                        changed_arms++;
                        changed_bits += arm_changes;
                    }
                    bool redundant = true;
                    for (int bit = 0; bit < width; bit++) {
                        auto a = default_data[bit], b = raw_data[arm*width+bit];
                        if (known.count(a)) a = known.at(a);
                        if (known.count(b)) b = known.at(b);
                        if (a != b) redundant = false;
                    }
                    if (redundant) {
                        raw_selects[arm] = RTLIL::State::S0;
                        redundant_arms++;
                    }
                }
                cell->setPort(ID::B, raw_data);
                cell->setPort(ID::S, raw_selects);
                // With an undefined default, only selected arms constrain a bit.
                // Recover a compared input literal when it agrees in every arm.
                if (cell->type != ID($pmux)) continue;
                std::vector<dict<RTLIL::SigBit, RTLIL::SigBit>> facts;
                for (auto select : selects) {
                    dict<RTLIL::SigBit, RTLIL::SigBit> known;
                    if (!drivers.count(select) || !forced_bits(drivers.at(select), sigmap, known)) break;
                    facts.push_back(known);
                }
                if (GetSize(facts) != GetSize(selects) || facts.empty()) continue;
                auto defaults = sigmap(cell->getPort(ID::A));
                auto outputs = cell->getPort(ID::Y);
                auto projected_outputs = outputs;
                for (int bit = 0; bit < width; bit++) {
                    if (defaults[bit] != RTLIL::State::Sx) continue;
                    bool zero = false, one = false, defined = true;
                    for (int arm = 0; arm < GetSize(selects); arm++) {
                        auto value = raw_data[arm*width+bit];
                        zero |= value == RTLIL::State::S0;
                        one |= value == RTLIL::State::S1;
                        defined &= value == RTLIL::State::S0 || value == RTLIL::State::S1;
                    }
                    if (!defined || !zero || !one) continue;
                    for (auto &entry : facts[0]) {
                        bool matches = true;
                        for (int arm = 0; arm < GetSize(selects); arm++)
                            if (!facts[arm].count(entry.first) ||
                                facts[arm].at(entry.first) != raw_data[arm*width+bit]) matches = false;
                        if (!matches) continue;
                        module->connect(outputs.extract(bit, 1), RTLIL::SigSpec(entry.first));
                        projected_outputs[bit] = module->addWire(NEW_ID);
                        projected_bits++;
                        break;
                    }
                }
                cell->setPort(ID::Y, projected_outputs);
            }
            if (preserve_powerup_low && !uninitialized_nonconstant_q.empty() &&
                before_changes != changed_bits + projected_bits + redundant_arms) {
                // Reset/set variants may invert Q during legalization and have a
                // different logical power-up value; those FFs are excluded.
                // Expose newly constant data before any sequential optimization.
                // The original Cyclone IV ff_map maps an undefined init to low.
                // Record that same implementation choice here so opt_dff cannot
                // erase the transient between power-up and first enabled write.
                Pass::call_on_module(design, module, "opt -noff -mux_undef -undriven -fine -full");
                SigMap after(module);
                FfInitVals init(&after, module);
                for (auto ffcell : module->cells()) {
                    if (!RTLIL::builtin_ff_cell_types().count(ffcell->type)) continue;
                    FfData ff(&init, ffcell);
                    if (!ff.has_clk || ff.has_arst || ff.has_srst || ff.has_sr || ff.has_aload || GetSize(ff.sig_d) != ff.width) continue;
                    auto d = after(ff.sig_d), q = after(ff.sig_q);
                    for (int bit = 0; bit < ff.width; bit++) {
                        if (d[bit] != State::S1 || !q[bit].wire ||
                            !uninitialized_nonconstant_q.count(q[bit]) ||
                            ff.val_init[bit] == State::S0 || ff.val_init[bit] == State::S1) continue;
                        init.set_init(ff.sig_q[bit], State::S0);
                        retained_bits++;
                    }
                }
            }
        }
        if(changed_bits||projected_bits||redundant_arms)design->scratchpad_set_bool("pmux_v31.changed",true);
        log("pmux_compare_const: changed %d arms and %d data bits.\n",
            changed_arms, changed_bits);
        log("pmux_compare_const: projected %d decoded data bits.\n", projected_bits);
        log("pmux_compare_const: removed %d conditionally redundant arms.\n", redundant_arms);
        log("V47 power-up preservation: retained %d newly constant-one FF bits with original Intel default low.\n", retained_bits);
    }
} PmuxCompareConstPass;

PRIVATE_NAMESPACE_END
