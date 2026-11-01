#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/ffinit.h"
#include <algorithm>
#include <vector>
#include <functional>
USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// C114 permits ABC script configuration. All mapper stages and cleanup are upstream.
struct PmuxAbc9ScriptPass : public Pass {
    PmuxAbc9ScriptPass():Pass("pmux_abc9_script","configure area script and run complete upstream ABC9"){}
    void execute(std::vector<std::string> args,Design *design) override {
        args[0]="abc9";
        auto prior_default=RTLIL::constpad.at("abc9.script.default");
        struct RestoreScript { std::string old; ~RestoreScript(){RTLIL::constpad["abc9.script.default"]=old;} } restore{prior_default};
        // C130: one fixed command strategy for every supported input.
        RTLIL::constpad["abc9.script.default"]="+&sweep; &dc2; &dc2; &if -a -C 8 -F 2 -A 4 {W} {D} {R}; &put; lutpack -s -N 3 -Q 1 -S 2 -L 1; &get -m";
        Pass::call(design,args);
    }
} PmuxAbc9ScriptPass;

// Cyclone IV/eLinx dffeas supports clock enable followed by synchronous clear.
// For reset-over-enable RTL, E'=E|R restores that priority exactly. Reset is
// synchronous, not connected to clrn, and each original FF is retained.
struct PmuxSyncClearPass : public Pass {
    PmuxSyncClearPass():Pass("pmux_sync_clear","map zero synchronous reset into the native FF synchronous-clear port"){}
    void execute(std::vector<std::string> args,Design *d)override {
        extra_args(args,1,d);int count=0;
        for(auto m:d->selected_modules()) {
            SigMap map(m);FfInitVals init(&map,m);dict<std::pair<SigBit,SigBit>,SigBit> enables;
            for(auto c:m->selected_cells()) {
                bool plain=c->type==ID($_SDFF_PP0_),over=c->type==ID($_SDFFE_PP0P_),under=c->type==ID($_SDFFCE_PP0P_);
                if((!plain&&!over&&!under)||c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                auto q=c->getPort(ID::Q),data=c->getPort(ID::D),clk=c->getPort(ID::C),rst=c->getPort(ID::R);
                if(GetSize(q)!=1||GetSize(data)!=1||GetSize(clk)!=1||GetSize(rst)!=1)continue;
                SigSpec en=State::S1;
                if(!plain)en=c->getPort(ID::E);
                if(GetSize(en)!=1)continue;
                if(over) {
                    auto key=std::make_pair(map(en)[0],map(rst)[0]);
                    if(!enables.count(key))enables[key]=SigBit(m->Or(NEW_ID,en,rst));
                    en=enables.at(key);
                }
                State value=init(q)[0];init.remove_init(q);
                std::vector<IdString> old;for(auto &p:c->connections())old.push_back(p.first);for(auto p:old)c->unsetPort(p);
                c->parameters.clear();c->type=RTLIL::escape_id("dffeas");
                c->setParam(RTLIL::escape_id("is_wysiwyg"),Const("TRUE"));c->setParam(RTLIL::escape_id("power_up"),Const(value==State::S1?"high":"low"));
                const char *names[]={"d","q","clk","ena","sclr"};SigSpec sigs[]={data,q,clk,en,rst};
                for(int k=0;k<5;k++)c->setPort(RTLIL::escape_id(names[k]),sigs[k]);
                for(auto name:{"clrn","prn"})c->setPort(RTLIL::escape_id(name),State::S1);
                for(auto name:{"asdata","aload","sload"})c->setPort(RTLIL::escape_id(name),State::S0);
                count++;
            }
        }
        log("pmux_sync_clear: mapped %d zero-reset FFs without changing FF count or reset/enable priority.\n",count);
    }
} PmuxSyncClearPass;

// Move a unique last data MUX into the existing FF synchronous-load mux.
// ENA and SCLR keep their original, higher priorities. No register is added.
struct PmuxFfInputMuxPass : public Pass {
    PmuxFfInputMuxPass():Pass("pmux_ff_input_mux","absorb unique FF data muxes into native synchronous-load inputs"){}
    void execute(std::vector<std::string> args,Design *d)override {
        extra_args(args,1,d);int removed=0;
        for(auto m:d->selected_modules()) {
            SigMap map(m);dict<SigBit,int> uses;dict<SigBit,Cell*> drivers;pool<SigBit> ambiguous;
            for(auto c:m->cells())for(auto &p:c->connections()) {
                if(c->input(p.first))for(auto b:map(p.second))uses[b]++;
                if(c->output(p.first))for(auto b:map(p.second)){if(drivers.count(b))ambiguous.insert(b);else drivers[b]=c;}
            }
            for(auto w:m->wires())if(w->port_output||w->get_bool_attribute(ID::keep))for(auto b:map(w))uses[b]++;
            dict<SigBit,int> bus_selects;
            for(auto ff:m->selected_cells())if(ff->type==RTLIL::escape_id("dffeas")) {
                auto p=map(ff->getPort(RTLIL::escape_id("d")));
                if(GetSize(p)==1&&uses[p[0]]==1&&drivers.count(p[0])&&drivers.at(p[0])->type==ID($_MUX_)) {
                    auto sel=map(drivers.at(p[0])->getPort(ID::S));if(GetSize(sel)==1)bus_selects[sel[0]]++;
                }
            }
            for(auto ff:m->selected_cells()) {
                if(ff->type!=RTLIL::escape_id("dffeas")||ff->get_bool_attribute(ID::keep)||ff->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                bool ok=true;for(auto name:{"aload","sload","asdata"})if(!ff->hasPort(RTLIL::escape_id(name))||map(ff->getPort(RTLIL::escape_id(name)))!=SigSpec(State::S0))ok=false;
                if(!ok)continue;
                auto p=map(ff->getPort(RTLIL::escape_id("d")));if(GetSize(p)!=1||uses[p[0]]!=1||ambiguous.count(p[0])||!drivers.count(p[0]))continue;
                auto c=drivers.at(p[0]);if(!d->selected(m,c)||c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch")))continue;
                if(c->type!=ID($_MUX_))continue;
                auto selector=map(c->getPort(ID::S));if(GetSize(selector)!=1||bus_selects[selector[0]]<4)continue;
                SigSpec a=c->getPort(ID::A),b=State::S0,select;
                if(c->type==ID($_MUX_)){b=c->getPort(ID::B);select=c->getPort(ID::S);}
                else if(c->type==ID($_AND_)){b=a;a=State::S0;select=c->getPort(ID::B);}
                else if(c->type==ID($_OR_)){b=State::S1;select=c->getPort(ID::B);}
                else {select=a;a=State::S1;b=State::S0;}
                if(GetSize(a)!=1||GetSize(b)!=1||GetSize(select)!=1)continue;
                ff->setPort(RTLIL::escape_id("d"),a);ff->setPort(RTLIL::escape_id("asdata"),b);ff->setPort(RTLIL::escape_id("sload"),select);
                drivers.erase(p[0]);m->remove(c);removed++;
            }
        }
        log("pmux_ff_input_mux: absorbed %d unique data gates into existing FF synchronous-load muxes.\n",removed);
    }
} PmuxFfInputMuxPass;

// Finish small scalar combinational networks exactly. FF Q and existing native
// LUT outputs remain independent boundaries. All roots must fit in four inputs;
// otherwise the entire module is left to upstream ABC9 without partial edits.
struct PmuxSmallConesPass : public Pass {
    PmuxSmallConesPass():Pass("pmux_small_cones","map bounded scalar cones by exact LUT4 truth tables"){}
    static int opcode(Cell *c) {
        if(c->type==ID($_NOT_))return 1;
        if(c->type==ID($_BUF_))return 2;
        if(c->type==ID($_AND_))return 3;
        if(c->type==ID($_OR_))return 4;
        if(c->type==ID($_XOR_))return 5;
        if(c->type==ID($_XNOR_))return 6;
        if(c->type==ID($_NAND_))return 7;
        if(c->type==ID($_NOR_))return 8;
        if(c->type==ID($_ANDNOT_))return 9;
        if(c->type==ID($_ORNOT_))return 10;
        if(c->type==ID($_MUX_))return 11;
        return 0;
    }
    void execute(std::vector<std::string> args,Design *d)override {
        extra_args(args,1,d);int mapped=0;
        for(auto m:d->selected_modules()) {
            if(m->get_blackbox_attribute()||!m->processes.empty()||!m->memories.empty())continue;
            SigMap map(m);std::vector<Cell*> gates;dict<SigBit,Cell*> driver;dict<SigBit,int> all_drivers;pool<SigBit> roots;bool valid=true;
            for(auto w:m->wires())if(w->port_input)for(auto b:map(w))all_drivers[b]++;
            for(auto c:m->cells()) {
                for(auto &p:c->connections())if(c->output(p.first))for(auto b:map(p.second))all_drivers[b]++;
                int op=opcode(c);
                if(!op) {
                    if(!c->type.in(ID($lut),RTLIL::escape_id("dffeas"),RTLIL::escape_id("cycloneiv_lcell_comb")))valid=false;
                    continue;
                }
                if(!d->selected(m,c)||c->get_bool_attribute(ID::keep)||c->get_bool_attribute(RTLIL::escape_id("dont_touch")))valid=false;
                for(auto &p:c->connections())if(GetSize(p.second)!=1)valid=false;
                if(!c->hasPort(ID::Y)||!c->hasPort(ID::A)||
                   (op>=3&&!c->hasPort(ID::B))||(op==11&&!c->hasPort(ID::S))) {valid=false;continue;}
                auto y=map(c->getPort(ID::Y));if(GetSize(y)!=1||!y[0].wire){valid=false;continue;}
                driver[y[0]]=c;gates.push_back(c);
            }
            if(!valid||gates.empty()||GetSize(gates)>128)continue;
            for(auto &p:driver)if(all_drivers[p.first]!=1)valid=false;
            for(auto c:m->cells())if(!opcode(c))for(auto &p:c->connections())if(c->input(p.first))
                for(auto b:map(p.second))if(driver.count(b))roots.insert(b);
            for(auto w:m->wires())if(w->port_output)for(auto b:map(w))if(driver.count(b))roots.insert(b);
            if(!valid||roots.empty())continue;
            struct Table {SigBit y;std::vector<SigBit> vars;int mask;};
            std::vector<Table> tables;
            for(auto root:roots) {
                pool<SigBit> active,seen,boundary;
                std::function<void(SigBit)> visit=[&](SigBit b) {
                    if(!valid||seen.count(b))return;
                    if(!b.wire){if(b!=State::S0&&b!=State::S1)valid=false;return;}
                    if(!driver.count(b)){boundary.insert(b);if(GetSize(boundary)>4)valid=false;return;}
                    if(active.count(b)){valid=false;return;}
                    active.insert(b);auto c=driver.at(b);
                    for(auto &p:c->connections())if(p.first!=ID::Y)visit(map(p.second)[0]);
                    active.erase(b);seen.insert(b);
                };
                visit(root);if(!valid)break;
                Table t{root,{},0};for(auto b:boundary)t.vars.push_back(b);std::sort(t.vars.begin(),t.vars.end());
                for(int v=0;v<16;v++) {
                    dict<SigBit,bool> values;
                    for(int k=0;k<GetSize(t.vars);k++)values[t.vars[k]]=(v>>k)&1;
                    std::function<bool(SigBit)> eval=[&](SigBit b)->bool {
                        if(!b.wire)return b==State::S1;
                        if(values.count(b))return values.at(b);
                        auto c=driver.at(b);int op=opcode(c);
                        bool a=eval(map(c->getPort(ID::A))[0]),z=false;
                        if(op==1)z=!a;else if(op==2)z=a;
                        else {bool q=eval(map(c->getPort(ID::B))[0]);
                            if(op==3)z=a&&q;else if(op==4)z=a||q;else if(op==5)z=a!=q;
                            else if(op==6)z=a==q;else if(op==7)z=!(a&&q);else if(op==8)z=!(a||q);
                            else if(op==9)z=a&&!q;else if(op==10)z=a||!q;
                            else z=eval(map(c->getPort(ID::S))[0])?q:a;}
                        values[b]=z;return z;
                    };
                    if(eval(root))t.mask|=1<<v;
                }
                tables.push_back(t);
            }
            if(!valid)continue;
            dict<std::pair<std::vector<SigBit>,int>,SigBit> cache;
            for(auto &t:tables) {
                SigBit y;
                if(t.mask==0||t.mask==65535)y=t.mask?State::S1:State::S0;
                else {
                    int identity=-1;for(int k=0;k<GetSize(t.vars);k++) {int mask=0;for(int v=0;v<16;v++)if((v>>k)&1)mask|=1<<v;if(mask==t.mask)identity=k;}
                    auto key=std::make_pair(t.vars,t.mask);
                    if(identity>=0)y=t.vars[identity];
                    else if(cache.count(key))y=cache.at(key);
                    else {
                        SigSpec pins;for(auto b:t.vars)pins.append(b);while(GetSize(pins)<4)pins.append(State::S0);
                        y=m->addWire(NEW_ID);m->addLut(NEW_ID,pins,y,Const(t.mask,16));cache[key]=y;
                    }
                }
                m->connect(t.y,y);
            }
            for(auto c:gates)m->remove(c);mapped+=GetSize(gates);
        }
        log("pmux_small_cones: exactly mapped %d scalar gates; all accepted roots have at most four boundary inputs.\n",mapped);
    }
} PmuxSmallConesPass;

// Exact truth-table folding of native LUT cones. No state assumptions.
struct Lut4ConeFoldPass : public Pass {
    Lut4ConeFoldPass():Pass("lut4_cone_fold","merge single-use LUT4 cones with at most four boundary inputs"){}
    void help()override {log("\n    lut4_cone_fold [selection]\n\nCanonicalize Cyclone IV datac LUTs and fold a single-use predecessor\nwhen the exact composed function has at most four boundary variables.\nReject used carry, keep/dont_touch cells, unsupported or unknown pins.\n\n");}
    struct Node {Cell *cell;SigBit y;std::vector<SigBit> vars;int mask;};
    static bool bit(int mask,int v){return (mask>>v)&1;}
    static void trim(Node &n) {
        std::vector<int> keep;
        for(int k=0;k<GetSize(n.vars);k++) {
            bool dependent=false;
            for(int v=0;v<(1<<GetSize(n.vars));v++)if(bit(n.mask,v)!=bit(n.mask,v^(1<<k)))dependent=true;
            if(dependent)keep.push_back(k);
        }
        int mask=0;std::vector<SigBit> vars;
        for(auto k:keep)vars.push_back(n.vars[k]);
        for(int v=0;v<16;v++) {
            int old=0;for(int k=0;k<GetSize(keep);k++)old|=((v>>k)&1)<<keep[k];
            if(bit(n.mask,old))mask|=1<<v;
        }
        n.vars=vars;n.mask=mask;
    }
    static void write(const Node &n) {
        const char *pins[]={"dataa","datab","datac","datad"};
        for(int k=0;k<4;k++)n.cell->setPort(RTLIL::escape_id(pins[k]),k<GetSize(n.vars)?n.vars[k]:SigBit(State::S0));
        n.cell->setParam(RTLIL::escape_id("lut_mask"),Const(n.mask,16));
    }
    static bool read(Cell *c,const SigMap &map,const dict<SigBit,int> &uses,Node &n) {
        if(c->type!=RTLIL::escape_id("cycloneiv_lcell_comb") || c->get_bool_attribute(ID::keep) || c->get_bool_attribute(RTLIL::escape_id("dont_touch")))return false;
        auto touch=RTLIL::escape_id("dont_touch");
        if(c->hasParam(touch)&&c->getParam(touch).decode_string()=="on")return false;
        auto mode=RTLIL::escape_id("sum_lutc_input"),mask_id=RTLIL::escape_id("lut_mask");
        if(c->hasParam(mode)&&c->getParam(mode).decode_string()!="datac")return false;
        if(!c->hasParam(mask_id)||GetSize(c->getParam(mask_id))!=16||!c->getParam(mask_id).is_fully_def())return false;
        auto cout=RTLIL::escape_id("cout");
        if(c->hasPort(cout))for(auto b:map(c->getPort(cout)))if(uses.count(b)&&uses.at(b))return false;
        auto y=map(c->getPort(RTLIL::escape_id("combout")));if(GetSize(y)!=1||!y[0].wire)return false;
        n.cell=c;n.y=y[0];n.vars.clear();n.mask=0;
        std::vector<SigBit> pins;
        const char *names[]={"dataa","datab","datac","datad"};
        for(auto name:names) {
            auto p=map(c->getPort(RTLIL::escape_id(name)));if(GetSize(p)!=1)return false;
            if(!p[0].wire&&p[0]!=State::S0&&p[0]!=State::S1)return false;
            pins.push_back(p[0]);if(p[0].wire)n.vars.push_back(p[0]);
        }
        std::sort(n.vars.begin(),n.vars.end());n.vars.erase(std::unique(n.vars.begin(),n.vars.end()),n.vars.end());
        int raw=c->getParam(mask_id).as_int(false);
        for(int v=0;v<16;v++) {
            int index=0;
            for(int k=0;k<4;k++) {
                bool val=pins[k]==State::S1;
                if(pins[k].wire)val=(v>>int(std::find(n.vars.begin(),n.vars.end(),pins[k])-n.vars.begin()))&1;
                index|=int(val)<<k;
            }
            if(bit(raw,index))n.mask|=1<<v;
        }
        trim(n);return true;
    }
    static bool compose(const Node &root,const Node &source,Node &merged) {
        auto vars=source.vars;
        for(auto b:root.vars)if(b!=source.y)vars.push_back(b);
        std::sort(vars.begin(),vars.end());vars.erase(std::unique(vars.begin(),vars.end()),vars.end());
        if(GetSize(vars)>4 || std::find(vars.begin(),vars.end(),root.y)!=vars.end() || std::find(vars.begin(),vars.end(),source.y)!=vars.end())return false;
        merged=root;merged.vars=vars;merged.mask=0;
        for(int v=0;v<16;v++) {
            auto value=[&](SigBit b){return bool((v>>int(std::find(vars.begin(),vars.end(),b)-vars.begin()))&1);};
            int inner=0;for(int k=0;k<GetSize(source.vars);k++)inner|=int(value(source.vars[k]))<<k;
            bool sy=bit(source.mask,inner);
            int outer=0;for(int k=0;k<GetSize(root.vars);k++)outer|=int(root.vars[k]==source.y?sy:value(root.vars[k]))<<k;
            if(bit(root.mask,outer))merged.mask|=1<<v;
        }
        trim(merged);return true;
    }
    void execute(std::vector<std::string> args,Design *design)override {
        extra_args(args,1,design);int folded=0,shared=0;bool canonicalized=false;
        for(auto m:design->selected_modules()) {
            // Canonicalization and each deletion strictly bound the work.
            for(int iteration=0;iteration<4096;iteration++) {
                SigMap map(m);dict<SigBit,int> uses;
                for(auto c:m->cells())for(auto &p:c->connections())if(c->input(p.first))for(auto b:map(p.second))uses[b]++;
                for(auto w:m->wires())if(w->port_output)for(auto b:map(w))uses[b]++;
                std::vector<Node> nodes;
                dict<SigBit,int> drivers;pool<SigBit> ambiguous;
                for(auto c:m->selected_cells()) {Node n;if(read(c,map,uses,n)){
                    const char *pins[]={"dataa","datab","datac","datad"};
                    bool differs=c->getParam(RTLIL::escape_id("lut_mask")).as_int(false)!=n.mask;
                    for(int k=0;k<4;k++)differs|=c->getPort(RTLIL::escape_id(pins[k]))!=SigSpec(k<GetSize(n.vars)?n.vars[k]:SigBit(State::S0));
                    canonicalized|=differs;write(n);nodes.push_back(n);
                }}
                for(int i=0;i<GetSize(nodes);i++) {
                    if(drivers.count(nodes[i].y))ambiguous.insert(nodes[i].y);
                    else drivers[nodes[i].y]=i;
                }
                bool changed=false;
                // Equal truth functions with canonical input order share one LUT.
                dict<std::pair<std::vector<SigBit>,int>,SigBit> unique;
                for(auto &n:nodes) {
                    auto key=std::make_pair(n.vars,n.mask);
                    if(ambiguous.count(n.y))continue;
                    if(unique.count(key)) {
                        m->connect(n.cell->getPort(RTLIL::escape_id("combout")),unique.at(key));m->remove(n.cell);shared++;changed=true;break;
                    }
                    unique[key]=n.y;
                }
                if(changed)continue;
                for(auto &root:nodes) {
                    if(ambiguous.count(root.y))continue;
                    for(auto b:root.vars) {
                        if(!drivers.count(b)||ambiguous.count(b)||uses[b]!=1)continue;
                        auto &source=nodes[drivers.at(b)];if(source.cell==root.cell)continue;
                        Node merged;if(!compose(root,source,merged))continue;
                        write(merged);m->remove(source.cell);folded++;changed=true;break;
                    }
                    if(changed)break;
                }
                if(!changed)break;
            }
        }
        if(folded||shared||canonicalized)design->scratchpad_set_bool("pmux_v31.changed",true);
        log("lut4_cone_fold: folded %d single-use LUT cones; shared %d equivalent LUTs.\n",folded,shared);
    }
} Lut4ConeFoldPass;
PRIVATE_NAMESPACE_END
