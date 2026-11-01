#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <algorithm>
#include <vector>
#include <functional>
#include <cstdint>
#include <array>
USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Exact truth-table folding of native LUT cones. No state assumptions.
struct Lut4ResubPass : public Pass {
    Lut4ResubPass():Pass("lut4_resub","merge single-use LUT4 cones with at most four boundary inputs"){}
    void help()override {log("\n    lut4_resub [selection]\n\nExact bounded truth-table resubstitution of native LUT4 networks.\nAt most 64 LUTs per module, six independent boundary variables,\nand 32 candidate divisor signals per root. Accept only fewer LUTs.\n\n");}
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
    void execute(std::vector<std::string> args,Design *design)override {
        extra_args(args,1,design);int rewrites=0,removed_count=0;
        for(auto m:design->selected_modules())for(int round=0;round<64;round++) {
            int native=0;for(auto c:m->cells())if(c->type==RTLIL::escape_id("cycloneiv_lcell_comb"))native++;
            if(native>64||native<3)break;
            SigMap map(m);dict<SigBit,int> uses;
            for(auto c:m->cells())for(auto &p:c->connections())if(c->input(p.first))for(auto b:map(p.second))uses[b]++;
            for(auto w:m->wires())if(w->port_output)for(auto b:map(w))uses[b]++;
            std::vector<Node> nodes;dict<SigBit,int> drivers;bool ambiguous=false;
            for(auto c:m->selected_cells()){Node n;if(read(c,map,uses,n)){if(drivers.count(n.y))ambiguous=true;drivers[n.y]=GetSize(nodes);nodes.push_back(n);}}
            if(ambiguous||nodes.empty())break;
            int count=GetSize(nodes);
            // Only LUT-output reference counts affect the deletion decision.
            std::array<int,64> base_refs{};
            for(int i=0;i<count;i++)base_refs[i]=uses[nodes[i].y];
            std::vector<int> state(count,0);std::vector<pool<SigBit>> leaves(count);std::vector<uint64_t> ancestors(count,0);bool cyclic=false;
            std::function<void(int)> visit=[&](int i){
                if(state[i]==2)return;if(state[i]==1){cyclic=true;return;}state[i]=1;
                for(auto b:nodes[i].vars)if(drivers.count(b)) {
                    int j=drivers.at(b);visit(j);ancestors[i]|=ancestors[j]|(uint64_t(1)<<j);
                    for(auto x:leaves[j])leaves[i].insert(x);
                }else leaves[i].insert(b);
                state[i]=2;
            };
            for(int i=0;i<count;i++)visit(i);if(cyclic)break;
            bool changed=false;
            for(int root=0;root<count&&!changed;root++) {
                if(GetSize(leaves[root])>6||leaves[root].empty())continue;
                // A root with no exclusively used predecessor cannot release a LUT.
                bool possible=false;for(auto b:nodes[root].vars)if(drivers.count(b)&&uses[b]==1)possible=true;
                if(!possible)continue;
                std::vector<SigBit> inputs;for(auto b:leaves[root])inputs.push_back(b);std::sort(inputs.begin(),inputs.end());
                int n=GetSize(inputs),rows=1<<n;uint64_t all=rows==64?~uint64_t(0):(uint64_t(1)<<rows)-1;
                dict<SigBit,uint64_t> truth;
                for(int k=0;k<n;k++){uint64_t t=0;for(int v=0;v<rows;v++)if((v>>k)&1)t|=uint64_t(1)<<v;truth[inputs[k]]=t;}
                std::function<uint64_t(SigBit)> evaluate=[&](SigBit b)->uint64_t {
                    if(truth.count(b))return truth.at(b);
                    auto &node=nodes[drivers.at(b)];std::array<uint64_t,4> ts{};
                    int size=GetSize(node.vars);
                    for(int k=0;k<size;k++)ts[k]=evaluate(node.vars[k]);
                    // Evaluate up to 64 assignments together using the LUT's minterms.
                    uint64_t result=0;
                    for(int ix=0;ix<(1<<size);ix++)if(bit(node.mask,ix)) {
                        uint64_t term=all;
                        for(int k=0;k<size;k++)term&=(ix&(1<<k))?ts[k]:~ts[k];
                        result|=term;
                    }
                    truth[b]=result;return result;
                };
                auto target=evaluate(nodes[root].y);std::vector<SigBit> candidates=inputs;
                for(int i=0;i<count;i++)if(i!=root&&!(ancestors[i]&(uint64_t(1)<<root))) {
                    bool subset=true;for(auto b:leaves[i])if(!leaves[root].count(b))subset=false;
                    if(subset)candidates.push_back(nodes[i].y);
                }
                if(GetSize(candidates)>32)continue;
                std::vector<uint64_t> ts;for(auto b:candidates)ts.push_back(evaluate(b));
                std::vector<int> choice;
                // At most four chosen inputs produce at most 16 partitions.
                // Fixed stack storage avoids a heap allocation and a vector copy
                // for every branch while preserving the exact search order.
                using Partitions = std::array<uint64_t,16>;
                std::function<bool(int,int,const Partitions&,int)> search;
                search=[&](int start,int remain,const Partitions& partitions,int partition_count)->bool {
                    if(remain) {
                        for(int i=start;i<=GetSize(candidates)-remain;i++) {
                            Partitions next;
                            for(int k=0;k<partition_count;k++){next[k]=partitions[k]&~ts[i];next[k+partition_count]=partitions[k]&ts[i];}
                            choice.push_back(i);if(search(i+1,remain-1,next,partition_count*2))return true;choice.pop_back();
                        }
                        return false;
                    }
                    int mask=0;
                    for(int k=0;k<partition_count;k++){auto p=partitions[k];if((p&target)&&(p&~target))return false;if(p&target)mask|=1<<k;}
                    auto refs=base_refs;std::vector<int> pending,erase;uint64_t marked=0;
                    for(auto b:nodes[root].vars)if(drivers.count(b))refs[drivers.at(b)]--;
                    for(auto i:choice)if(drivers.count(candidates[i]))refs[drivers.at(candidates[i])]++;
                    for(auto b:nodes[root].vars)if(drivers.count(b)&&refs[drivers.at(b)]==0)pending.push_back(drivers.at(b));
                    while(!pending.empty()) {
                        int j=pending.back();pending.pop_back();if(j==root||(marked&(uint64_t(1)<<j))||refs[j]!=0)continue;
                        marked|=uint64_t(1)<<j;erase.push_back(j);
                        for(auto b:nodes[j].vars)if(drivers.count(b)){int k=drivers.at(b);refs[k]--;if(refs[k]==0)pending.push_back(k);}
                    }
                    if(erase.empty())return false;
                    Node updated=nodes[root];updated.vars.clear();for(auto i:choice)updated.vars.push_back(candidates[i]);updated.mask=0;
                    for(int v=0;v<16;v++)if((mask>>(v&((1<<GetSize(choice))-1)))&1)updated.mask|=1<<v;
                    trim(updated);write(updated);
                    for(auto j:erase)m->remove(nodes[j].cell);
                    rewrites++;removed_count+=GetSize(erase);return true;
                };
                Partitions initial;initial[0]=all;
                for(int k=1;k<=4&&!changed;k++)changed=search(0,k,initial,1);
            }
            if(!changed)break;
        }
        if(rewrites)design->scratchpad_set_bool("pmux_v31.changed",true);
        log("lut4_resub: rewrote %d roots; removed %d LUTs by exact resubstitution.\n",rewrites,removed_count);
    }
} Lut4ResubPass;

// A bounded local cut, rather than the entire transitive support, lets exact
// resubstitution cross native/ABC9 boundaries in larger control networks.
struct Lut4WindowRewritePass : public Pass {
    using N=Lut4ResubPass::Node;
    Lut4WindowRewritePass():Pass("lut4_window_rewrite","exact bounded local-cut LUT4 resubstitution"){}
    void execute(std::vector<std::string> args,Design *d)override {
        extra_args(args,1,d);int saved=0,checked=0,budget=4096;
        for(auto m:d->selected_modules())for(int round=0;round<32&&budget>0;round++) {
            SigMap map(m);dict<SigBit,int> uses;
            for(auto c:m->cells())for(auto &p:c->connections())if(c->input(p.first))for(auto b:map(p.second))uses[b]++;
            for(auto w:m->wires())if(w->port_output||w->get_bool_attribute(ID::keep))for(auto b:map(w))uses[b]++;
            std::vector<N> nodes;dict<SigBit,int> drivers;bool ambiguous=false;
            for(auto c:m->selected_cells()){N n;if(Lut4ResubPass::read(c,map,uses,n)){if(drivers.count(n.y))ambiguous=true;drivers[n.y]=GetSize(nodes);nodes.push_back(n);}}
            if(ambiguous||nodes.empty()||GetSize(nodes)>512)break;
            std::vector<int> states(GetSize(nodes),0);bool cycle=false;
            std::function<void(int)> acyclic=[&](int i){if(states[i]==2)return;if(states[i]==1){cycle=true;return;}states[i]=1;for(auto b:nodes[i].vars)if(drivers.count(b))acyclic(drivers.at(b));states[i]=2;};
            for(int i=0;i<GetSize(nodes);i++)acyclic(i);if(cycle)break;
            bool changed=false;
            for(int root=0;root<GetSize(nodes)&&!changed&&budget>0;root++) {
                bool possible=false;for(auto b:nodes[root].vars)if(drivers.count(b)&&uses[b]==1)possible=true;
                if(!possible)continue;
                pool<SigBit> frontier,expanded;frontier.insert(nodes[root].y);
                for(int step=0;step<9;step++) {
                    bool found=false;SigBit best;int cost=100;
                    for(auto b:frontier)if(drivers.count(b)&&!expanded.count(b)) {
                        auto next=frontier;next.erase(b);for(auto x:nodes[drivers.at(b)].vars)next.insert(x);
                        int score=2*GetSize(next)+(uses[b]>1?1:0);
                        if(GetSize(next)<=6&&score<cost){cost=score;best=b;found=true;}
                    }
                    if(!found)break;
                    frontier.erase(best);expanded.insert(best);for(auto b:nodes[drivers.at(best)].vars)frontier.insert(b);
                }
                if(GetSize(expanded)<2||frontier.empty()||GetSize(frontier)>6)continue;
                std::vector<SigBit> inputs;for(auto b:frontier)inputs.push_back(b);std::sort(inputs.begin(),inputs.end());
                int size=GetSize(inputs),rows=1<<size;uint64_t all=rows==64?~uint64_t(0):(uint64_t(1)<<rows)-1;
                dict<SigBit,uint64_t> truth;
                for(int k=0;k<size;k++){uint64_t v=0;for(int x=0;x<rows;x++)if((x>>k)&1)v|=uint64_t(1)<<x;truth[inputs[k]]=v;}
                std::function<uint64_t(SigBit)> eval=[&](SigBit b)->uint64_t {
                    if(truth.count(b))return truth.at(b);
                    auto &n=nodes[drivers.at(b)];uint64_t result=0;std::array<uint64_t,4> xs{};
                    for(int k=0;k<GetSize(n.vars);k++)xs[k]=eval(n.vars[k]);
                    for(int x=0;x<(1<<GetSize(n.vars));x++)if((n.mask>>x)&1){uint64_t t=all;for(int k=0;k<GetSize(n.vars);k++)t&=(x&(1<<k))?xs[k]:~xs[k];result|=t;}
                    truth[b]=result;return result;
                };
                uint64_t target=eval(nodes[root].y);std::vector<SigBit> candidates=inputs;
                for(auto b:expanded)if(b!=nodes[root].y)candidates.push_back(b);
                if(GetSize(candidates)>16)continue;
                std::vector<uint64_t> ts;for(auto b:candidates)ts.push_back(eval(b));checked++;
                using Parts=std::array<uint64_t,16>;std::vector<int> choice;int trials=0;
                std::function<bool(int,int,const Parts&,int)> search=[&](int start,int remain,const Parts&ps,int count)->bool {
                    if(--budget<0||++trials>256)return false;
                    if(remain){for(int k=start;k<=GetSize(candidates)-remain;k++){Parts next;for(int i=0;i<count;i++){next[i]=ps[i]&~ts[k];next[i+count]=ps[i]&ts[k];}choice.push_back(k);if(search(k+1,remain-1,next,count*2))return true;choice.pop_back();}return false;}
                    int mask=0;for(int i=0;i<count;i++){if((ps[i]&target)&&(ps[i]&~target))return false;if(ps[i]&target)mask|=1<<i;}
                    auto refs=uses;for(auto b:nodes[root].vars)refs[b]--;for(int k:choice)refs[candidates[k]]++;
                    std::vector<int> erase;std::vector<SigBit> todo=nodes[root].vars;pool<SigBit> visited;
                    while(!todo.empty()){auto b=todo.back();todo.pop_back();if(refs[b]!=0||visited.count(b)||!drivers.count(b))continue;visited.insert(b);int j=drivers.at(b);erase.push_back(j);for(auto x:nodes[j].vars){refs[x]--;if(refs[x]==0)todo.push_back(x);}}
                    if(erase.empty())return false;
                    N updated=nodes[root];updated.vars.clear();for(int k:choice)updated.vars.push_back(candidates[k]);updated.mask=0;
                    for(int v=0;v<16;v++)if((mask>>(v&((1<<GetSize(choice))-1)))&1)updated.mask|=1<<v;
                    Lut4ResubPass::trim(updated);Lut4ResubPass::write(updated);
                    for(int j:erase)m->remove(nodes[j].cell);saved+=GetSize(erase);return true;
                };
                Parts initial;initial[0]=all;for(int k=1;k<=4&&!changed;k++)changed=search(0,k,initial,1);
            }
            if(!changed)break;
        }
        if(saved)d->scratchpad_set_bool("pmux_v31.changed",true);
        log("lut4_window_rewrite: examined %d local cuts, removed %d LUTs, remaining search budget %d.\n",checked,saved,budget);
    }
} Lut4WindowRewritePass;
PRIVATE_NAMESPACE_END
