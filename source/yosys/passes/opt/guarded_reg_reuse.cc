#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/ff.h"

#include <memory>
#include <vector>
#include <algorithm>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Conservative, bounded symbolic propagation. Unknown values never prove equality.
struct ReuseEval {
    SigMap &map;
    dict<SigBit, Cell*> &drivers;
    dict<SigBit, SigBit> bindings, cache;
    pool<SigBit> busy;
    ReuseEval(SigMap &map, dict<SigBit, Cell*> &drivers) : map(map), drivers(drivers) {}
    static bool known(SigBit b) { return b == State::S0 || b == State::S1; }
    static SigBit meet(SigBit a, SigBit b) {
        return a == b && (a.wire || known(a)) ? a : SigBit(State::Sx);
    }
    void bind(SigSpec q, Const value) {
        q = map(q);
        for (int i=0; i<GetSize(q); i++) bindings[q[i]] = value[i];
    }
    SigSpec vec(SigSpec sig) {
        SigSpec result;
        for (auto b : sig) result.append(bit(b));
        return result;
    }
    SigBit truth(SigSpec sig) {
        bool unknown = false;
        for (auto b : sig) {
            b = bit(b);
            if (b == State::S1) return State::S1;
            if (b != State::S0) unknown = true;
        }
        return unknown ? State::Sx : State::S0;
    }
    SigBit bit(SigBit raw) {
        SigBit b = map(raw);
        if (!b.wire) return known(b) ? b : SigBit(State::Sx);
        if (bindings.count(b)) return bindings.at(b);
        if (cache.count(b)) return cache.at(b);
        if (busy.count(b) || GetSize(busy) >= 128) return State::Sx;
        if (!drivers.count(b)) return b;
        Cell *c = drivers.at(b);
        if (!c->hasPort(ID::Y)) return b; // FF Q and opaque outputs stay symbolic.
        SigSpec y = map(c->getPort(ID::Y));
        int offset = -1;
        for (int i=0; i<GetSize(y); i++) if (y[i] == b) offset = i;
        if (offset < 0) return b;
        busy.insert(b);
        SigBit result = b;
        if (c->type == ID($mux)) {
            auto a = c->getPort(ID::A), d = c->getPort(ID::B);
            auto s = bit(c->getPort(ID::S)[0]);
            if (s == State::S0) result = bit(a[offset]);
            else if (s == State::S1) result = bit(d[offset]);
            else result = meet(bit(a[offset]), bit(d[offset]));
        } else if (c->type == ID($pmux)) {
            int selected = -1;
            bool ambiguous = false;
            auto s = vec(c->getPort(ID::S));
            for (int i=0; i<GetSize(s); i++) {
                if (!known(s[i])) ambiguous = true;
                if (s[i] == State::S1) {
                    if (selected >= 0) ambiguous = true;
                    selected = i;
                }
            }
            if (ambiguous) result = State::Sx;
            else result = bit(selected < 0 ? c->getPort(ID::A)[offset] : c->getPort(ID::B)[selected*GetSize(y)+offset]);
        } else if (offset == 0 && c->type.in(ID($logic_not), ID($reduce_or), ID($reduce_bool))) {
            result = truth(c->getPort(ID::A));
            if (c->type == ID($logic_not)) result = known(result) ? SigBit(result == State::S0 ? State::S1 : State::S0) : SigBit(State::Sx);
        } else if (offset == 0 && c->type == ID($reduce_and)) {
            result = State::S1;
            for (auto a : vec(c->getPort(ID::A))) {
                if (a == State::S0) { result = State::S0; break; }
                if (a != State::S1) result = State::Sx;
            }
        } else if (offset == 0 && c->type.in(ID($eq), ID($ne)) && GetSize(c->getPort(ID::A)) == GetSize(c->getPort(ID::B))) {
            auto a = vec(c->getPort(ID::A)), d = vec(c->getPort(ID::B));
            result = State::S1;
            for (int i=0; i<GetSize(a); i++) {
                if (known(a[i]) && known(d[i]) && a[i] != d[i]) { result = State::S0; break; }
                if (meet(a[i], d[i]) == State::Sx) result = State::Sx;
            }
            if (c->type == ID($ne)) result = known(result) ? SigBit(result == State::S0 ? State::S1 : State::S0) : SigBit(State::Sx);
        } else if (c->type.in(ID($and), ID($or), ID($not)) && GetSize(c->getPort(ID::A)) == GetSize(y)) {
            auto a = bit(c->getPort(ID::A)[offset]);
            result = State::Sx;
            if (c->type == ID($not)) {
                if (known(a)) result = a == State::S0 ? State::S1 : State::S0;
            } else if (GetSize(c->getPort(ID::B)) == GetSize(y)) {
                auto d = bit(c->getPort(ID::B)[offset]);
                if (c->type == ID($and)) {
                    if (a == State::S0 || d == State::S0) result = State::S0;
                    else if (a == State::S1) result = d;
                    else if (d == State::S1) result = a;
                    else result = meet(a, d);
                } else {
                    if (a == State::S1 || d == State::S1) result = State::S1;
                    else if (a == State::S0) result = d;
                    else if (d == State::S0) result = a;
                    else result = meet(a, d);
                }
            }
        }
        busy.erase(b);
        cache[b] = result;
        return result;
    }
};

struct GuardedRegReuse {
    struct Model {
        FfData control;
        std::vector<std::unique_ptr<ReuseEval>> evals;
        std::vector<SigSpec> next_control;
        std::vector<dict<Cell*, SigSpec>> next_regs;
        Model(FfData control, SigMap &map, dict<SigBit, Cell*> &drivers) : control(control) {
            for (int value=0; value<(1<<control.width); value++) {
                auto e = std::make_unique<ReuseEval>(map, drivers);
                e->bind(control.sig_q, Const(value, control.width));
                e->bind(control.sig_arst, Const(!control.pol_arst, 1));
                next_control.push_back(e->vec(control.sig_d));
                evals.push_back(std::move(e));
                next_regs.emplace_back();
            }
        }
        SigSpec next(FfData &reg, int state) {
            auto &cache = next_regs[state];
            if (cache.count(reg.cell)) return cache.at(reg.cell);
            auto &e = *evals[state];
            SigSpec d = e.vec(reg.sig_d), q = e.vec(reg.sig_q), result;
            auto en = reg.has_ce ? e.bit(reg.sig_ce[0]) : SigBit(State::S1);
            if (reg.has_ce && !reg.pol_ce && ReuseEval::known(en)) en = en == State::S0 ? State::S1 : State::S0;
            if (en == State::S1) result = d;
            else if (en == State::S0) result = q;
            else for (int i=0; i<reg.width; i++) result.append(ReuseEval::meet(d[i], q[i]));
            cache[reg.cell] = result;
            return result;
        }
        bool proves(Const guard, FfData &source, FfData &target) {
            if (control.val_init == guard || control.val_arst == guard) return false;
            bool predecessor = false;
            for (int state=0; state<GetSize(evals); state++) {
                bool possible = true;
                for (int i=0; i<control.width; i++)
                    if (ReuseEval::known(next_control[state][i]) && next_control[state][i] != SigBit(guard[i])) possible = false;
                if (!possible) continue;
                predecessor = true;
                auto a = next(source, state), b = next(target, state);
                for (int i=0; i<source.width; i++)
                    if (ReuseEval::meet(a[i], b[i]) == State::Sx) return false;
            }
            return predecessor;
        }
    };
    void execute(Design *design) {
        int rewrites = 0;
        for (auto module : design->selected_modules()) {
            if (module->has_processes() || module->cells().size() > 3000) continue;
            // Reject modules without a supported controller before building maps.
            bool has_controller = false;
            for (auto c : module->selected_cells())
                if (c->type == ID($adff) && GetSize(c->getPort(ID::Q)) >= 2 && GetSize(c->getPort(ID::Q)) <= 6)
                    has_controller = true;
            if (!has_controller) continue;
            SigMap map(module);
            FfInitVals init; init.set(&map, module);
            dict<SigBit, Cell*> drivers;
            pool<SigBit> inputs;
            bool duplicate = false;
            for (auto wire : module->wires()) if (wire->port_input)
                for (auto b : map(SigSpec(wire))) inputs.insert(b);
            for (auto c : module->cells()) for (auto &port : c->connections()) if (c->output(port.first))
                for (auto b : map(port.second)) {
                    if (!b.wire || inputs.count(b) || drivers.count(b)) duplicate = true;
                    drivers[b] = c;
                }
            if (duplicate) continue;
            dict<SigSpec, Cell*> data_by_q, control_by_q;
            dict<Cell*, FfData> regs;
            std::vector<Cell*> data;
            for (auto c : module->selected_cells()) {
                if (!c->type.in(ID($dff), ID($dffe), ID($adff))) continue;
                FfData f(&init, c);
                if (f.width < 2 || f.width > 32 || !inputs.count(map(f.sig_clk)[0]) || c->get_bool_attribute(ID::keep)) continue;
                regs[c] = f;
                if (c->type == ID($adff)) {
                    if (f.width <= 6 && f.val_init.is_fully_def() && f.val_arst.is_fully_def() && inputs.count(map(f.sig_arst)[0])) control_by_q[map(f.sig_q)] = c;
                } else { data_by_q[map(f.sig_q)] = c; data.push_back(c); }
            }
            if (control_by_q.empty() || data.empty()) continue;
            std::sort(data.begin(), data.end(), [](Cell *a, Cell *b) { return a->name < b->name; });
            dict<Cell*, std::shared_ptr<Model>> models;
            struct Change { Cell *mux; int offset; SigSpec value; };
            std::vector<Change> changes;
            for (auto mux : module->selected_cells()) {
                if (mux->type != ID($pmux) || mux->get_bool_attribute(ID::keep)) continue;
                int width = GetSize(mux->getPort(ID::A));
                auto selects = map(mux->getPort(ID::S)), branches = map(mux->getPort(ID::B));
                for (int k=0; k<GetSize(selects); k++) {
                    auto value = branches.extract(k*width, width);
                    if (!data_by_q.count(value) || !drivers.count(selects[k])) continue;
                    Cell *decode = drivers.at(selects[k]);
                    if (decode->type != ID($eq)) continue;
                    auto a = map(decode->getPort(ID::A)), b = map(decode->getPort(ID::B));
                    if (a.is_fully_def()) std::swap(a,b);
                    if (!b.is_fully_def() || GetSize(a) != GetSize(b) || !control_by_q.count(a)) continue;
                    Cell *ctrl = control_by_q.at(a), *src = data_by_q.at(value);
                    auto &cf = regs.at(ctrl), &sf = regs.at(src);
                    if (sf.pol_clk != cf.pol_clk || map(sf.sig_clk) != map(cf.sig_clk)) continue;
                    if (!models.count(ctrl)) models[ctrl] = std::make_shared<Model>(cf, map, drivers);
                    for (auto dst : data) {
                        auto &tf = regs.at(dst);
                        if (dst == src || tf.width != width || tf.pol_clk != sf.pol_clk || map(tf.sig_clk) != map(sf.sig_clk)) continue;
                        if (!models.at(ctrl)->proves(b.as_const(), sf, tf)) continue;
                        changes.push_back({mux, k*width, tf.sig_q});
                        log("guarded_reg_reuse: %s -> %s under state %s = %s.\n", log_signal(sf.sig_q), log_signal(tf.sig_q), log_signal(cf.sig_q), log_signal(b));
                        break;
                    }
                }
            }
            // All proofs use the original transition relation before any rewiring.
            for (auto &change : changes) {
                auto b = change.mux->getPort(ID::B);
                b.replace(change.offset, change.value);
                change.mux->setPort(ID::B, b);
                rewrites++;
            }
        }
        log("guarded_reg_reuse: rewrote %d guarded register reads.\n", rewrites);
        if (rewrites) {
            design->scratchpad_set_bool("pmux_v31.changed",true);
            Pass::call(design, "opt -fast");
        }
    }
};

PRIVATE_NAMESPACE_END

YOSYS_NAMESPACE_BEGIN
void run_guarded_reg_reuse(Design *design)
{
    GuardedRegReuse helper;
    helper.execute(design);
}
YOSYS_NAMESPACE_END

PRIVATE_NAMESPACE_BEGIN
struct GuardedReuseStagePass : public Pass {
 GuardedReuseStagePass():Pass("guarded_reuse_stage","reuse state-guarded register reads before PMUX sharing"){}
 void help()override{log("    guarded_reuse_stage [selection]\n");}
 void execute(std::vector<std::string> args,Design *d)override{extra_args(args,1,d);run_guarded_reg_reuse(d);
   // Permit initialized asynchronous-reset controllers to use Yosys FSM
   // extraction when initialization agrees exactly with reset.
   pool<Module*> marked; dict<Module*,pool<IdString>> prior;
   for(auto m:d->selected_modules()) {
     // A 2..6-bit asynchronous-reset register is necessary.
     bool eligible=false;
     for(auto c:m->selected_cells())if(c->type==ID($adff)) {
       int width=GetSize(c->getPort(ID::Q));
       if(width>=2&&width<=6&&!c->get_bool_attribute(ID::keep)){eligible=true;break;}
     }
     if(!eligible)continue;
     bool premarked=false,has_init=false;
     for(auto w:m->wires()) {
       if(w->attributes.count(ID::fsm_encoding)&&w->attributes.at(ID::fsm_encoding).decode_string()!="none")premarked=true;
       if(w->attributes.count(ID::init))has_init=true;
     }
     for(auto c:m->cells())if(c->type==ID($fsm))premarked=true;
     if(premarked||!has_init)continue;
     SigMap map(m);FfInitVals init;init.set(&map,m);
     pool<SigBit> external;for(auto wire:m->wires())if(wire->port_output)for(auto b:map(SigSpec(wire)))external.insert(b);
     for(auto c:m->selected_cells())if(c->type==ID($adff)&&!c->get_bool_attribute(ID::keep)) {
       FfData ff(&init,c);auto q=ff.sig_q;
       if(ff.width<2||ff.width>6||!ff.val_init.is_fully_def()||ff.val_init!=ff.val_arst||!q.is_wire())continue;
       bool exposed=false;for(auto b:map(q))if(external.count(b))exposed=true;if(exposed)continue;
       auto w=q.as_wire();if(w->port_id||w->get_bool_attribute(ID::keep)||w->attributes.count(ID::fsm_encoding))continue;
       w->attributes[ID::fsm_encoding]=Const("one-hot");marked.insert(m);log("fsm_init_recode: marked initialized reset-matching controller.\n");
     }
   }
   if(!marked.empty()){
     d->scratchpad_set_bool("pmux_v31.changed",true);
     // Marking only changes wire attributes; snapshot cells immediately before FSM.
     for(auto m:marked)for(auto c:m->cells())prior[m].insert(c->name);
     for(auto m:marked)Pass::call_on_module(d,m,"fsm -nodetect");
     for(auto &entry:prior)if(marked.count(entry.first))for(auto c:entry.first->cells())if(c->type==ID($adff)&&!entry.second.count(c->name)) {
       auto q=c->getPort(ID::Q);if(!q.is_wire())continue;
       auto value=c->getParam(ID::ARST_VALUE);for(int k=0;k<GetSize(value);k++)if(value[k]!=State::S1)value.set(k,State::S0);
       q.as_wire()->attributes[ID::init]=value;
     }
   }
 }
} GuardedReuseStagePass;
PRIVATE_NAMESPACE_END
