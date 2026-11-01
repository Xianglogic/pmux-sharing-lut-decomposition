#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <vector>
USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct EqPrefixSharePass : public Pass {
    static bool &soft_mode() { static bool value=false; return value; }
    EqPrefixSharePass():Pass("eq_prefix_share","share a LUT4 prefix between adjacent constant equality predicates"){}
    void help()override {log("\n    eq_prefix_share [selection]\n\nMatch equal-width 5..12 bit equality predicates over the same variable\nkey whose fully defined constants differ in exactly one bit. Implement\nthe common equality prefix once and leave the residual bit comparisons\nto the normal mapper. No input constraints or sequential assumptions.\n\n");}
    struct Match {Cell *cell;SigSpec key;Const value;bool used=false;};
    static SigBit lut(Module *m,SigSpec pins,int mask) {
        if(soft_mode()) {
            // These prefixes are conjunctions of literals. Keep their cut
            // boundary open so ABC9 may absorb them into downstream predicates.
            SigSpec literals;
            for(int k=0;k<GetSize(pins);k++) {
                bool positive=false;
                for(int v=0;v<16;v++)if((mask>>v)&1)positive=(v>>k)&1;
                literals.append(positive?pins[k]:SigBit(m->Not(NEW_ID,SigSpec(pins[k]))));
            }
            return SigBit(m->ReduceAnd(NEW_ID,literals));
        }
        while(GetSize(pins)<4)pins.append(State::S0);
        auto c=m->addCell(NEW_ID,RTLIL::escape_id("cycloneiv_lcell_comb"));auto y=m->addWire(NEW_ID);
        c->setParam(RTLIL::escape_id("lut_mask"),Const(mask,16));
        c->setParam(RTLIL::escape_id("sum_lutc_input"),Const("datac"));
        const char *names[]={"dataa","datab","datac","datad"};
        for(int i=0;i<4;i++)c->setPort(RTLIL::escape_id(names[i]),pins.extract(i,1));
        c->setPort(RTLIL::escape_id("cin"),State::S0);c->setPort(RTLIL::escape_id("combout"),y);c->setPort(RTLIL::escape_id("cout"),m->addWire(NEW_ID));
        return SigBit(y);
    }
    struct Factor { SigBit output; std::vector<int> positions; };
    static SigBit prefix(Module *m,const Match &a,int skip,int &created,const Factor *factor=nullptr) {
        SigSpec pins;std::vector<int> values;
        for(int i=0;i<GetSize(a.key);i++)if(i!=skip && (!factor || std::find(factor->positions.begin(),factor->positions.end(),i)==factor->positions.end())){pins.append(a.key[i]);values.push_back(a.value[i]==State::S1);}
        SigBit previous=factor ? factor->output : SigBit(State::S1);int offset=0;bool have_previous=factor!=nullptr;
        while(offset<GetSize(pins)) {
            SigSpec input;std::vector<int> expected;
            if(have_previous){input.append(previous);expected.push_back(1);}
            while(GetSize(input)<4&&offset<GetSize(pins)){input.append(pins[offset]);expected.push_back(values[offset]);offset++;}
            int mask=0;for(int v=0;v<16;v++) {
                bool yes=true;for(int k=0;k<GetSize(expected);k++)if(((v>>k)&1)!=expected[k])yes=false;
                if(yes)mask|=1<<v;
            }
            previous=lut(m,input,mask);have_previous=true;created++;
        }
        return previous;
    }
    void execute(std::vector<std::string> args,Design *d)override {
        size_t argidx=1;soft_mode()=false;bool hybrid=false;
        if(argidx<args.size()&&args[argidx]=="-soft"){soft_mode()=true;argidx++;}
        if(argidx<args.size()&&args[argidx]=="-hybrid"){hybrid=true;argidx++;}
        extra_args(args,argidx,d);int pairs=0,created=0;
        for(auto m:d->selected_modules()) {
            bool has_eq=false;std::vector<Cell*> fact_ors;
            for(auto c:m->cells()) {
                if(c->type==ID($eq))has_eq=true;
                if(c->type==ID($reduce_or)||c->type==ID($reduce_bool))fact_ors.push_back(c);
            }
            if(!has_eq)continue;
            SigMap map(m);std::vector<Match> matches;
            // True constant comparisons imply every positive literal in the key.
            // An OR implies only literals common to every alternative.
            dict<SigBit, pool<SigBit>> facts;
            for(auto c:m->cells()) if(c->type==ID($eq)) {
                auto a=map(c->getPort(ID::A)),b=map(c->getPort(ID::B));
                if(a.is_fully_const())std::swap(a,b);
                if(GetSize(a)!=GetSize(b)||!b.is_fully_const()||!b.as_const().is_fully_def()||GetSize(c->getPort(ID::Y))!=1)continue;
                auto y=map(c->getPort(ID::Y))[0];
                for(int i=0;i<GetSize(a);i++)if(b[i]==State::S1&&a[i].wire)facts[y].insert(a[i]);
            }
            for(int iteration=0;iteration<8;iteration++)for(auto c:fact_ors) {
                if(c->type!=ID($reduce_or)&&c->type!=ID($reduce_bool))continue;
                auto a=map(c->getPort(ID::A));if(a.empty())continue;
                pool<SigBit> common=facts[a[0]];
                for(auto bit:a){pool<SigBit> next;for(auto x:common)if(facts[bit].count(x))next.insert(x);common=next;}
                facts[map(c->getPort(ID::Y))[0]]=common;
            }
            for(auto c:m->selected_cells())if(c->type==ID($reduce_and)&&!c->get_bool_attribute(ID::keep)) {
                auto a=map(c->getPort(ID::A));SigSpec reduced;
                for(int i=0;i<GetSize(a);i++) {
                    bool redundant=false;
                    for(int j=0;j<GetSize(a);j++)if(i!=j&&facts[a[j]].count(a[i]))redundant=true;
                    if(!redundant)reduced.append(a[i]);
                }
                if(!reduced.empty()&&GetSize(reduced)<GetSize(a)){c->setPort(ID::A,reduced);c->setParam(ID::A_WIDTH,GetSize(reduced));}
            }
            for(auto c:m->selected_cells()) {
                if(c->type!=ID($eq)||c->get_bool_attribute(ID::keep))continue;
                auto a=map(c->getPort(ID::A)),b=map(c->getPort(ID::B));
                if(a.is_fully_const())std::swap(a,b);
                int width=GetSize(a);
                if(width<5||width>12||GetSize(b)!=width||a.has_const()||!b.is_fully_const()||!b.as_const().is_fully_def()||GetSize(c->getPort(ID::Y))!=1)continue;
                pool<SigBit> unique;for(auto bit:a)unique.insert(bit);if(GetSize(unique)!=width)continue;
                matches.push_back({c,a,b.as_const(),false});
            }
            std::vector<Cell*> removed; dict<SigSpec,int> residual_bit;
            dict<SigSpec,Factor> factors;
            auto choose_boundary=[&](const SigSpec &key) {
                if(!hybrid)return;
                int consumers=0;for(auto &match:matches)if(match.key==key)consumers++;
                // Small isolated groups can retain a fixed prefix cut; larger
                // decoder groups benefit from cross-predicate ABC9 mapping.
                soft_mode()=consumers>=4;
            };
            // Share up to four identical literals across a comparator group.
            // A paired-prefix candidate must exist, and at least three full
            // comparisons must consume the same key. No content-specific match.
            for(auto &a:matches)if(!factors.count(a.key)) {
                choose_boundary(a.key);
                std::vector<const Match*> group;
                for(auto &b:matches)if(a.key==b.key)group.push_back(&b);
                if(GetSize(group)<3)continue;
                bool paired=false;
                for(auto b:group)for(auto c:group){int diff=0;for(int k=0;k<GetSize(a.key);k++)diff+=b->value[k]!=c->value[k];if(diff==1)paired=true;}
                if(!paired)continue;
                Factor factor;SigSpec pins;std::vector<int> expected;
                for(int k=0;k<GetSize(a.key)&&GetSize(pins)<4;k++) {
                    bool same=true;for(auto b:group)if(b->value[k]!=a.value[k])same=false;
                    if(same){factor.positions.push_back(k);pins.append(a.key[k]);expected.push_back(a.value[k]==State::S1);}
                }
                if(GetSize(pins)<3)continue;
                int mask=0;for(int v=0;v<16;v++){bool yes=true;for(int k=0;k<GetSize(expected);k++)if(((v>>k)&1)!=expected[k])yes=false;if(yes)mask|=1<<v;}
                factor.output=lut(m,pins,mask);created++;factors[a.key]=factor;
            }
            for(int i=0;i<GetSize(matches);i++) {
                auto &a=matches[i];if(a.used)continue;
                choose_boundary(a.key);
                for(int j=i+1;j<GetSize(matches);j++) {
                    auto &b=matches[j];if(b.used||a.key!=b.key)continue;
                    int differing=-1,count=0;
                    for(int k=0;k<GetSize(a.key);k++)if(a.value[k]!=b.value[k]){differing=k;count++;}
                    if(count!=1)continue;
                    residual_bit[a.key]=differing;
                    auto common=prefix(m,a,differing,created,factors.count(a.key)?&factors.at(a.key):nullptr);
                    for(auto match:{&a,&b}) {
                        SigSpec literal=match->key.extract(differing,1);
                        if(match->value[differing]==State::S0)literal=m->Not(NEW_ID,literal);
                        auto output=match->cell->getPort(ID::Y);
                        m->addAnd(NEW_ID,common,literal,output);match->used=true;removed.push_back(match->cell);
                    }
                    pairs++;break;
                }
            }
            // Preserve a shared complete predicate on the same key, preventing
            // duplicated partial comparisons at consumers of the paired flags.
            for(auto &a:matches)if(!a.used) {
                choose_boundary(a.key);
                bool related=false;for(auto &b:matches)if(b.used&&a.key==b.key)related=true;
                if(related){
                    // Keep the same residual literal outside the remaining
                    // comparator too. Its consumers can absorb this literal,
                    // avoiding a LUT needed only to exceed the four-pin cut.
                    int split=residual_bit.at(a.key);
                    auto common=prefix(m,a,split,created,factors.count(a.key)?&factors.at(a.key):nullptr);
                    SigSpec literal=a.key.extract(split,1);
                    if(a.value[split]==State::S0)literal=m->Not(NEW_ID,literal);
                    m->addAnd(NEW_ID,common,literal,a.cell->getPort(ID::Y));removed.push_back(a.cell);
                }
            }
            for(auto c:removed)m->remove(c);
        }
        log("eq_prefix_share: rewrote %d equality pairs; built %d shared prefix LUTs.\n",pairs,created);
    }
} EqPrefixSharePass;
PRIVATE_NAMESPACE_END
