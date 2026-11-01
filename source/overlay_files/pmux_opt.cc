#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#define PMUX_NEW_ID RTLIL::IdString::new_autoidx_with_prefix([](std::string_view func) -> const std::string * { static std::unique_ptr<const std::string> prefix(create_id_prefix(__FILE__, __LINE__, pmux_id_function(func))); return prefix.get(); }(__FUNCTION__))
#include <algorithm>
#include <climits>
#include <map>
#include <numeric>
#include <tuple>
#include <functional>

USING_YOSYS_NAMESPACE
YOSYS_NAMESPACE_BEGIN
void run_guarded_reg_reuse(Design *design);
YOSYS_NAMESPACE_END
PRIVATE_NAMESPACE_BEGIN
static std::string pmux_id_function(std::string_view f) { std::string s; for(char c:f) if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n'&&c!='\f'&&c!='\v') s+=c; return s; }
struct DecodeTerm
{
	RTLIL::SigSpec key;
	int value = -1;
};

struct RewriteCandidate
{
	RTLIL::Cell *cell = nullptr;
	RTLIL::SigSpec key;
	RTLIL::SigSpec output;
	std::vector<RTLIL::SigSpec> table;
	int width = 0;
	int key_width = 0;
	int select_width = 0;
};

// Factor equal data bits across arbitrary PMUX select terms.  Unlike the
// decoded-key DAG, this keeps the exact OR semantics for multi-hot selects.
// A bit plane is expressed as (|S) ? OR_g((|S_g) & data_g) : A_bit.
struct CoalesceGroup
{
	RTLIL::SigBit data;
	RTLIL::SigSpec selects;
};

static std::vector<CoalesceGroup> coalesce_groups(const RTLIL::SigSpec &data,
		const RTLIL::SigSpec &selects, int width, int bit)
{
	std::vector<CoalesceGroup> groups;
	for (int branch = 0; branch < GetSize(selects); branch++) {
		RTLIL::SigBit value = data[branch * width + bit];
		bool found = false;
		for (auto &group : groups)
			if (group.data == value) {
				group.selects.append(selects[branch]);
				found = true;
				break;
			}
		if (!found) {
			CoalesceGroup group;
			group.data = value;
			group.selects.append(selects[branch]);
			groups.push_back(group);
		}
	}
	return groups;
}

static bool coalesce_pmux(RTLIL::Module *module, RTLIL::Cell *cell,
		const SigMap &sigmap, int min_saving, int &created)
{
	int width = cell->getParam(ID::WIDTH).as_int();
	int choices = cell->getParam(ID::S_WIDTH).as_int();
	if (width < 2 || choices < 4 || width > 512 || choices > 128)
		return false;
	RTLIL::SigSpec a = sigmap(cell->getPort(ID::A));
	RTLIL::SigSpec b = sigmap(cell->getPort(ID::B));
	RTLIL::SigSpec s = sigmap(cell->getPort(ID::S));
	if (GetSize(a) != width || GetSize(b) != width * choices || GetSize(s) != choices)
		return false;
	for (auto bit : b)
		if (bit == RTLIL::State::Sx || bit == RTLIL::State::Sz)
			return false;

	std::vector<std::vector<CoalesceGroup>> planes;
	int original_terms = 0, new_terms = 0;
	dict<RTLIL::SigSpec, int> selector_groups;
	for (int bit = 0; bit < width; bit++) {
		auto groups = coalesce_groups(b, s, width, bit);
		for (int branch = 0; branch < choices; branch++)
			if (b[branch * width + bit] != RTLIL::State::S0)
				original_terms++;
		for (auto &group : groups) {
			if (group.data == RTLIL::State::S0)
				continue;
			new_terms++;
			if (GetSize(group.selects) > 1)
				selector_groups[group.selects] = GetSize(group.selects) - 1;
		}
		planes.push_back(std::move(groups));
	}
	for (auto &item : selector_groups)
		new_terms += item.second;
	int saving = original_terms - new_terms;
	if (original_terms < 16 || saving < 8 ||
		static_cast<long long>(saving) * 100 < static_cast<long long>(original_terms) * min_saving)
		return false;

	dict<RTLIL::SigSpec, RTLIL::SigSpec> selector_cache;
	auto selector_or = [&](const RTLIL::SigSpec &inputs) -> RTLIL::SigSpec {
		if (GetSize(inputs) == 1)
			return inputs;
		if (selector_cache.count(inputs))
			return selector_cache.at(inputs);
		RTLIL::Wire *wire = module->addWire(PMUX_NEW_ID);
		module->addReduceOr(PMUX_NEW_ID, inputs, wire);
		created++;
		selector_cache[inputs] = RTLIL::SigSpec(wire);
		return wire;
	};
	RTLIL::SigSpec active = selector_or(s);
	RTLIL::SigSpec result;
	for (int bit = 0; bit < width; bit++) {
		RTLIL::SigSpec sum(RTLIL::State::S0);
		for (auto &group : planes[bit]) {
			if (group.data == RTLIL::State::S0)
				continue;
			RTLIL::SigSpec term = selector_or(group.selects);
			if (group.data != RTLIL::State::S1) {
				RTLIL::Wire *wire = module->addWire(PMUX_NEW_ID);
				module->addAnd(PMUX_NEW_ID, term, RTLIL::SigSpec(group.data), wire);
				term = wire;
				created++;
			}
			if (sum == RTLIL::SigSpec(RTLIL::State::S0))
				sum = term;
			else {
				RTLIL::Wire *wire = module->addWire(PMUX_NEW_ID);
				module->addOr(PMUX_NEW_ID, sum, term, wire);
				sum = wire;
				created++;
			}
		}
		RTLIL::Wire *outbit = module->addWire(PMUX_NEW_ID);
		module->addMux(PMUX_NEW_ID, a.extract(bit, 1), sum, active, outbit);
		result.append(outbit);
		created++;
	}
	log("  Coalescing %s.%s using selector-coalescing: original-terms=%d, factored-terms=%d, estimated-saving=%d%%.\n",
		log_id(module), log_id(cell), original_terms, new_terms,
		original_terms ? saving * 100 / original_terms : 0);
	module->connect(cell->getPort(ID::Y), result);
	module->remove(cell);
	return true;
}

struct DagCost
{
	dict<RTLIL::SigSpec, int> leaves;
	std::map<std::tuple<int, int, int>, int> nodes;
	int next_id = 1;

	int leaf_id(const RTLIL::SigSpec &sig)
	{
		if (!leaves.count(sig))
			leaves[sig] = next_id++;
		return leaves.at(sig);
	}

	int build(const std::vector<RTLIL::SigSpec> &table,
			const std::vector<int> &order, int level, int value)
	{
		if (level == GetSize(order))
			return leaf_id(table.at(value));

		int bit = order.at(level);
		int low = build(table, order, level + 1, value);
		int high = build(table, order, level + 1, value | (1 << bit));
		if (low == high)
			return low;

		auto key = std::make_tuple(bit, low, high);
		auto it = nodes.find(key);
		if (it != nodes.end())
			return it->second;
		int id = next_id++;
		nodes[key] = id;
		return id;
	}
};

static int dag_cost(const std::vector<RewriteCandidate *> &group,
		const std::vector<int> &order)
{
	DagCost cost;
	for (auto candidate : group)
		cost.build(candidate->table, order, 0, 0);
	return GetSize(cost.nodes);
}

// Count one-bit cofactor nodes across every output bit in the group.  The
// existing word DAG only shares complete WIDTH-bit subtrees and misses these.
static int bit_dag_cost(const std::vector<RewriteCandidate *> &group,
		const std::vector<int> &order)
{
	DagCost cost;
	for (auto candidate : group)
		for (int bit = 0; bit < candidate->width; bit++) {
			std::vector<RTLIL::SigSpec> plane;
			plane.reserve(candidate->table.size());
			for (auto &entry : candidate->table)
				plane.push_back(entry.extract(bit, 1));
			cost.build(plane, order, 0, 0);
		}
	return GetSize(cost.nodes);
}

static RTLIL::SigSpec build_mux_dag(RTLIL::Module *module,
		const RTLIL::SigSpec &key, const std::vector<RTLIL::SigSpec> &table,
		const std::vector<int> &order, int level, int value,
		dict<RTLIL::SigSpec, RTLIL::SigSpec> &cache, int &created)
{
	if (level == GetSize(order))
		return table.at(value);

	int bit = order.at(level);
	RTLIL::SigSpec low = build_mux_dag(module, key, table, order,
			level + 1, value, cache, created);
	RTLIL::SigSpec high = build_mux_dag(module, key, table, order,
			level + 1, value | (1 << bit), cache, created);
	if (low == high)
		return low;

	RTLIL::SigSpec cache_key = low;
	cache_key.append(high);
	cache_key.append(key.extract(bit, 1));
	if (cache.count(cache_key))
		return cache.at(cache_key);

	RTLIL::Wire *wire = module->addWire(PMUX_NEW_ID, GetSize(low));
	module->addMux(PMUX_NEW_ID, low, high, key.extract(bit, 1), wire);
	RTLIL::SigSpec result(wire);
	cache[cache_key] = result;
	created++;
	return result;
}

static bool decode_equality(RTLIL::Cell *cell, const SigMap &sigmap, DecodeTerm &term)
{
	if (cell->type == ID($logic_not)) {
		if (!cell->hasPort(ID::A) || !cell->hasPort(ID::Y))
			return false;

		RTLIL::SigSpec key = sigmap(cell->getPort(ID::A));
		RTLIL::SigSpec y = sigmap(cell->getPort(ID::Y));
		if (GetSize(y) != 1 || GetSize(key) == 0 || key.has_const())
			return false;

		term.key = key;
		term.value = 0;
		return true;
	}

	if (cell->type != ID($eq))
		return false;
	if (!cell->hasPort(ID::A) || !cell->hasPort(ID::B) || !cell->hasPort(ID::Y))
		return false;

	RTLIL::SigSpec a = sigmap(cell->getPort(ID::A));
	RTLIL::SigSpec b = sigmap(cell->getPort(ID::B));
	RTLIL::SigSpec y = sigmap(cell->getPort(ID::Y));
	if (GetSize(y) != 1 || GetSize(a) == 0 || GetSize(a) != GetSize(b))
		return false;

	RTLIL::SigSpec key;
	RTLIL::Const constant;
	if (a.is_fully_const() && !b.has_const()) {
		constant = a.as_const();
		key = b;
	} else if (b.is_fully_const() && !a.has_const()) {
		constant = b.as_const();
		key = a;
	} else {
		return false;
	}

	if (!constant.is_fully_def() || GetSize(constant) >= int(sizeof(int) * CHAR_BIT - 1))
		return false;

	term.key = key;
	term.value = constant.as_int(false);
	return true;
}

// Non-disjoint decomposition of a four-way data selector into two LUT4s.
// g = s0 ? (s1 ? d3 : d1) : s1
// y = s0 ? g : (g ? d2 : d0)
// The shared s0 carries either a selector or a selected data value across
// the cut. This avoids a third binary-mux LUT without any input constraints.
struct Mux4LutBuilder {
    Module *m;const SigMap &map;dict<SigBit,SigBit> inverse;
    dict<std::pair<SigSpec,int>,SigBit> cache;
    Mux4LutBuilder(Module *module,const SigMap &sm):m(module),map(sm) {
        for(auto c:m->cells())if(c->type==ID($not)&&!c->get_bool_attribute(ID::keep)) {
            auto a=map(c->getPort(ID::A)),y=map(c->getPort(ID::Y));
            if(GetSize(a)==GetSize(y))for(int k=0;k<GetSize(a);k++)if(y[k].wire)inverse[y[k]]=a[k];
        }
    }
    std::pair<SigBit,bool> literal(SigBit b) {
        bool neg=false;pool<SigBit> seen;
        while(inverse.count(b)&&!seen.count(b)){seen.insert(b);b=inverse.at(b);neg=!neg;}
        return {b,neg};
    }
    SigBit lut(SigSpec args,int raw) {
        std::vector<std::pair<SigBit,bool>> ls;std::vector<SigBit> ordered;
        for(auto b:args){auto v=literal(b);ls.push_back(v);if(v.first.wire)ordered.push_back(v.first);}
        std::sort(ordered.begin(),ordered.end());ordered.erase(std::unique(ordered.begin(),ordered.end()),ordered.end());log_assert(GetSize(ordered)<=4);
        int mask=0;for(int x=0;x<16;x++) {
            int old=0;for(int k=0;k<GetSize(ls);k++) {
                auto b=ls[k].first;bool v=b==State::S1;
                if(b.wire)v=(x>>int(std::find(ordered.begin(),ordered.end(),b)-ordered.begin()))&1;
                old|=int(v^ls[k].second)<<k;
            }
            if((raw>>old)&1)mask|=1<<x;
        }
        if(mask==0)return State::S0;if(mask==65535)return State::S1;
        if(GetSize(ordered)==1&&mask==0xaaaa)return ordered[0];
        SigSpec pins;for(auto b:ordered)pins.append(b);
        auto key=std::make_pair(pins,mask);if(cache.count(key))return cache.at(key);
        auto c=m->addCell(PMUX_NEW_ID,RTLIL::escape_id("cycloneiv_lcell_comb"));auto y=m->addWire(PMUX_NEW_ID);
        const char *names[]={"dataa","datab","datac","datad"};
        for(int k=0;k<4;k++)c->setPort(RTLIL::escape_id(names[k]),k<GetSize(pins)?pins[k]:SigBit(State::S0));
        c->setParam(RTLIL::escape_id("lut_mask"),Const(mask,16));c->setParam(RTLIL::escape_id("sum_lutc_input"),Const("datac"));
        c->setPort(RTLIL::escape_id("cin"),State::S0);c->setPort(RTLIL::escape_id("combout"),y);c->setPort(RTLIL::escape_id("cout"),m->addWire(PMUX_NEW_ID));
        cache[key]=SigBit(y);return SigBit(y);
    }
    SigBit build(const SigSpec &key,const std::vector<SigBit> &table) {
        if(table[0]==table[1]&&table[0]==table[2]&&table[0]==table[3])return table[0];
        // Constant/small-support planes fit directly in a single LUT.
        pool<SigBit> support;
        for(auto b:key){auto v=literal(b).first;if(v.wire)support.insert(v);}
        for(auto b:table){auto v=literal(b).first;if(v.wire)support.insert(v);}
        if(GetSize(support)<=4) {
            std::vector<SigBit> pins;for(auto b:support)pins.push_back(b);std::sort(pins.begin(),pins.end());
            int mask=0;for(int x=0;x<16;x++) {
                auto value=[&](SigBit b){auto v=literal(b);bool z=v.first==State::S1;if(v.first.wire)z=(x>>int(std::find(pins.begin(),pins.end(),v.first)-pins.begin()))&1;return z^v.second;};
                int address=int(value(key[0]))+2*int(value(key[1]));if(value(table[address]))mask|=1<<x;
            }
            SigSpec args;for(auto b:pins)args.append(b);return lut(args,mask);
        }
        SigSpec inner;inner.append(table[1]);inner.append(key[0]);inner.append(key[1]);inner.append(table[3]);
        auto g=lut(inner,0xf838);
        SigSpec outer;outer.append(table[0]);outer.append(table[2]);outer.append(key[0]);outer.append(g);
        return lut(outer,0xfc0a);
    }
};

// Recode each output bit's at most two variable data values plus 0/1 into
// a shared two-bit operation code. Fully enumerate the decoded binary key;
// no one-hot assumption and no undefined selector value is introduced.
static bool recode_classes(Module *m,const std::vector<RewriteCandidate*> &group) {
    if(group[0]->key_width>4)return false;
    struct Plane {SigBit y;std::vector<SigBit> vars;std::vector<int> codes;};
    std::vector<Plane> planes;pool<std::vector<int>> patterns;int original=0;
    for(auto c:group)for(int k=0;k<c->width;k++) {
        Plane p{c->output[k],{}, {}};
        for(auto &entry:c->table) {
            auto b=entry[k];int code;
            if(b==State::S0)code=0;else if(b==State::S1)code=1;
            else if(!b.wire)return false;
            else {auto it=std::find(p.vars.begin(),p.vars.end(),b);if(it==p.vars.end()){p.vars.push_back(b);it=p.vars.end()-1;}code=2+int(it-p.vars.begin());}
            p.codes.push_back(code);
        }
        if(p.vars.empty()||GetSize(p.vars)>2)return false;
        patterns.insert(p.codes);planes.push_back(p);original+=c->select_width;
    }
    // Shared coding must amortize its two LUTs over the bit planes. The
    // conservative proxy requires fewer than two LUTs per output bit.
    if(GetSize(planes)<4||GetSize(planes)+2*GetSize(patterns)>=2*GetSize(planes))return false;
    auto lut=[&](SigSpec pins,int mask)->SigBit {
        auto cell=m->addCell(PMUX_NEW_ID,RTLIL::escape_id("cycloneiv_lcell_comb"));auto y=m->addWire(PMUX_NEW_ID);
        cell->setParam(RTLIL::escape_id("lut_mask"),Const(mask,16));cell->setParam(RTLIL::escape_id("sum_lutc_input"),Const("datac"));
        const char *names[]={"dataa","datab","datac","datad"};for(int k=0;k<4;k++)cell->setPort(RTLIL::escape_id(names[k]),k<GetSize(pins)?pins[k]:SigBit(State::S0));
        cell->setPort(RTLIL::escape_id("combout"),y);return SigBit(y);
    };
    dict<std::vector<int>,SigSpec> cache;
    for(auto &p:planes) {
        if(!cache.count(p.codes)) {
            SigSpec key;
            for(int k=0;k<2;k++){int mask=0;for(int v=0;v<16;v++)if((p.codes[v%GetSize(p.codes)]>>k)&1)mask|=1<<v;key.append(lut(group[0]->key,mask));}
            cache[p.codes]=key;
        }
        SigSpec pins=cache.at(p.codes);for(auto b:p.vars)pins.append(b);
        int mask=0;for(int v=0;v<16;v++){int code=v&3;bool value=code==1||(code>=2&&((v>>(code))&1));if(value)mask|=1<<v;}
        m->connect(p.y,lut(pins,mask));
    }
    for(auto c:group)m->remove(c->cell);
    log("pmux_opt: shared output-class recoding replaced %d PMUX cells, %d planes, %d shared selector classes.\n",GetSize(group),GetSize(planes),GetSize(patterns));
    return true;
}

static bool native_mux4_group(Module *m,const SigMap &map,const std::vector<RewriteCandidate*> &group) {
    if(group[0]->key_width!=2||group[0]->width<4)return false;
    int variables=0,total=0;pool<SigBit> distinct;
    for(auto c:group)for(auto &row:c->table)for(auto b:row) {
        total++;if(b.wire){variables++;distinct.insert(b);}else if(b!=State::S0&&b!=State::S1)return false;
    }
    pool<SigBit> boundary;
    for(auto w:m->wires())if(w->port_input)for(auto b:map(w))boundary.insert(b);
    for(auto c:m->cells())if(c->type.in(ID($dff),ID($adff),ID($dffe),ID($adffe),ID($sdff),ID($sdffe)))
        for(auto b:map(c->getPort(ID::Q)))boundary.insert(b);
    int independent=0;for(auto b:distinct)if(boundary.count(b))independent++;
    if(variables*4<total*3||GetSize(distinct)<2*group[0]->width||independent<2*group[0]->width)return false;
    Mux4LutBuilder builder(m,map);
    for(auto c:group) {
        SigSpec out;for(int k=0;k<c->width;k++) {
            std::vector<SigBit> plane;for(auto &row:c->table)plane.push_back(row[k]);out.append(builder.build(c->key,plane));
        }
        m->connect(c->output,out);m->remove(c->cell);
    }
    log("pmux_opt: native non-disjoint MUX4 decomposition rewrote %d PMUX(es), created %d shared LUTs.\n",GetSize(group),GetSize(builder.cache));return true;
}

struct PmuxOptPass : public Pass
{
	PmuxOptPass() : Pass("pmux_opt", "factor decoded $pmux cells into a shared mux decision DAG") {}

	void help() override
	{
		log("\n");
		log("    -native-mux4\n");
		log("        map decoded two-bit data selectors into shared-variable LUT4s;\n");
		log("        require width >= 4, >=75%% variable data, and at least two\n");
		log("        boundary input/register bits per output bit; unknowns are skipped\n");
		log("    -order-limit <N>\n");
		log("        bound tested variable orders (0 = legacy exhaustive search)\n");
		log("    pmux_opt [options] [selection]\n");
		log("\n");
		log("Recognize a $pmux whose select bits are unique equality decoders\n");
		log("for one common binary key. Missing decoded values use the PMUX A\n");
		log("input, so ordinary case/default structures are handled directly.\n");
		log("Duplicate, mixed-key, unknown-constant, and ambiguous decoders are\n");
		log("left unchanged. Matching PMUXes are grouped by key and width, then\n");
		log("rebuilt as a reduced, structurally shared $mux decision DAG. A\n");
		log("rewrite is accepted only when its bit-mux proxy cost clears a\n");
		log("configurable safety margin relative to the original PMUX branches.\n");
		log("\n");
		log("    -max-key <N>\n");
		log("        maximum binary key width to rewrite (default: 8)\n");
		log("\n");
		log("    -min-branches <N>\n");
		log("        minimum number of effective data choices to rewrite (default: 4);\n");
		log("        the A/default input counts when decoded values are missing\n");
		log("\n");
		log("    -min-saving <N>\n");
		log("        require at least N percent reduction in the bit-mux proxy\n");
		log("        before rewriting (default: 25, range: 0..100)\n");
		log("\n");
		log("    -allow-rom\n");
		log("        also rewrite all-constant tables with key width >= 5. By\n");
		log("        default those tables are preserved for downstream ROM inference.\n");
		log("    -min-bit-saving <N>\n");
		log("        require N percent bit-node saving when the word DAG is rejected\n");
		log("        (default: 90; set to 101 to disable bit-plane factoring)\n");
		log("    -coalesce-bits\n");
		log("        additionally factor repeated branch data bits across arbitrary\n");
		log("        select signals, preserving PMUX multi-hot OR semantics\n");
		log("    -coalesce-min-saving <N>\n");
		log("        minimum branch-term proxy saving (default: 40, range: 0..100)\n");
		log("    -guard-wide-decode\n");
		log("        preserve wide, high-fanin PMUXes with mostly variable branch\n");
		log("        data; a deep mux tree can cost more LUTs than native PMUX\n");
		log("    -anchor-gate\n");
		log("        rewrite decoded PMUX groups only in modules containing a\n");
		log("        wide low-variable-data anchor, a large bus, or a sparse\n");
		log("        well-shared one-bit lookup group\n");
		log("    -guard-onebit-highfanin\n");
		log("        preserve one-bit groups with any eight-way-or-higher PMUX\n");
		log("        and disallow such groups from anchoring a module\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		int max_key = 8;
		int min_branches = 4;
		int min_saving = 25;
		int min_bit_saving = 90;
        int order_limit=0;
        bool native_mux4=false;
		int coalesce_min_saving = 40;
		bool allow_rom = false;
		bool coalesce_bits = false;
		bool guard_wide_decode = false;
		bool anchor_gate = false;
		bool guard_onebit_highfanin = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
            if(args[argidx]=="-native-mux4"){native_mux4=true;continue;}
            if(args[argidx]=="-order-limit"&&argidx+1<args.size()){order_limit=atoi(args[++argidx].c_str());continue;}
			if (args[argidx] == "-max-key" && argidx + 1 < args.size()) {
				max_key = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-min-branches" && argidx + 1 < args.size()) {
				min_branches = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-min-saving" && argidx + 1 < args.size()) {
				min_saving = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-min-bit-saving" && argidx + 1 < args.size()) {
				min_bit_saving = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-allow-rom") {
				allow_rom = true;
				continue;
			}
			if (args[argidx] == "-coalesce-bits") {
				coalesce_bits = true;
				continue;
			}
			if (args[argidx] == "-guard-wide-decode") {
				guard_wide_decode = true;
				continue;
			}
			if (args[argidx] == "-anchor-gate") {
				anchor_gate = true;
				continue;
			}
			if (args[argidx] == "-guard-onebit-highfanin") {
				guard_onebit_highfanin = true;
				continue;
			}
			if (args[argidx] == "-coalesce-min-saving" && argidx + 1 < args.size()) {
				coalesce_min_saving = atoi(args[++argidx].c_str());
				continue;
			}
			break;
		}

		if (max_key < 1 || max_key > 20)
			log_cmd_error("Option -max-key must be between 1 and 20.\n");
		if (min_branches < 2)
			log_cmd_error("Option -min-branches must be at least 2.\n");
		if (min_saving < 0 || min_saving > 100)
			log_cmd_error("Option -min-saving must be between 0 and 100.\n");
		if (min_bit_saving < 0 || min_bit_saving > 101)
			log_cmd_error("Option -min-bit-saving must be between 0 and 101.\n");
		if (coalesce_min_saving < 0 || coalesce_min_saving > 100)
			log_cmd_error("Option -coalesce-min-saving must be between 0 and 100.\n");

		extra_args(args, argidx, design);
		log_header(design, "Executing PMUX_OPT pass (factor decoded PMUXes).\n");
		// Guarded reuse runs once in guarded_reuse_stage, before sharing.

		int rewritten = 0;
		int created_muxes = 0;
		int examined = 0;
		int coalesced = 0;
		for (auto module : design->selected_modules()) {
			std::vector<RTLIL::Cell *> pmux_cells;
			for (auto cell : module->selected_cells())
				if (cell->type == ID($pmux))
					pmux_cells.push_back(cell);
			if (pmux_cells.empty()) continue;
			SigMap sigmap(module);
			dict<RTLIL::SigBit, RTLIL::Cell *> driver;
			pool<RTLIL::SigBit> ambiguous_driver;

			for (auto cell : module->cells()) {
                if (cell->type != ID($eq) && cell->type != ID($logic_not) &&
                    !(design->scratchpad_get_bool("v49.recode")&&cell->type.in(ID($reduce_or),ID($reduce_bool),ID($or),ID($logic_or))))
					continue;
				if (!cell->hasPort(ID::Y))
					continue;
				RTLIL::SigSpec y = sigmap(cell->getPort(ID::Y));
				if (GetSize(y) != 1 || y[0].wire == nullptr)
					continue;
				if (driver.count(y[0])) {
					ambiguous_driver.insert(y[0]);
					continue;
				}
				driver[y[0]] = cell;
			}


			std::vector<RewriteCandidate> candidates;
			for (auto cell : pmux_cells) {
				examined++;
				int width = cell->getParam(ID::WIDTH).as_int();
				int select_width = cell->getParam(ID::S_WIDTH).as_int();
				RTLIL::SigSpec sig_a = sigmap(cell->getPort(ID::A));
				RTLIL::SigSpec sig_b = sigmap(cell->getPort(ID::B));
				RTLIL::SigSpec sig_s = sigmap(cell->getPort(ID::S));
				RTLIL::SigSpec sig_y = cell->getPort(ID::Y);

				if (width <= 0 || select_width <= 0 ||
					GetSize(sig_a) != width ||
					GetSize(sig_s) != select_width ||
					GetSize(sig_b) != width * select_width ||
					GetSize(sig_y) != width)
					continue;

				RTLIL::SigSpec common_key;
				std::vector<std::vector<int>> values(select_width);
				bool valid = true;
				for (int i = 0; i < select_width; i++) {
					RTLIL::SigBit sel = sig_s[i];
					if (sel.wire == nullptr || ambiguous_driver.count(sel) || !driver.count(sel)) {
						valid = false;
						break;
					}

                    pool<SigBit> active;int terms=0;
                    std::function<bool(SigBit,int)> collect=[&](SigBit bit,int depth)->bool {
                        if(bit==State::S0)return true;
                        if(depth>16||++terms>64||active.count(bit)||ambiguous_driver.count(bit)||!driver.count(bit))return false;
                        auto c=driver.at(bit);DecodeTerm term;
                        if(decode_equality(c,sigmap,term)) {
                            if(common_key.empty())common_key=term.key;
                            if(common_key!=term.key)return false;
                            values[i].push_back(term.value);return true;
                        }
                        if(!design->scratchpad_get_bool("v49.recode")||!c->type.in(ID($reduce_or),ID($reduce_bool),ID($or),ID($logic_or)))return false;
                        active.insert(bit);bool ok=true;
                        for(auto &port:c->connections())if(port.first!=ID::Y)for(auto b:sigmap(port.second))if(!collect(b,depth+1))ok=false;
                        active.erase(bit);return ok;
                    };
                    if(!collect(sel,0)||values[i].empty()){valid=false;break;}
				}
				if (!valid)
					continue;

				// The bit-mux proxy misses the LUT cost of a deep 8+-way tree
				// when almost every branch is an independent variable bus.
				// Keep the native PMUX so techmapping can factor the decoder
				// and data cone together. This is a structural gate, independent
				// of module or cell identity.
				if (guard_wide_decode && width >= 16 && select_width >= 8) {
					int variable_bits = 0;
					for (auto bit : sig_b)
						if (bit.wire != nullptr)
							variable_bits++;
					if (variable_bits * 4 >= GetSize(sig_b) * 3) {
						log("  Preserving high-fanin variable PMUX %s.%s: WIDTH=%d, S_WIDTH=%d, variable-data-bits=%d/%d.\n",
							log_id(module), log_id(cell), width, select_width,
							variable_bits, GetSize(sig_b));
						continue;
					}
				}

				int key_width = GetSize(common_key);
				if (key_width < 1 || key_width > max_key || key_width >= 30)
					continue;
                if(native_mux4&&!design->scratchpad_get_bool("v49.recode")&&min_saving==100&&min_bit_saving==101&&!coalesce_bits&&
                   (key_width!=2||width<4))continue;
				int table_size = 1 << key_width;
				if (select_width > table_size)
					continue;
				int effective_choices = select_width + (select_width < table_size ? 1 : 0);
				if (effective_choices < min_branches)
					continue;

				std::vector<bool> seen(table_size, false);
				std::vector<RTLIL::SigSpec> table_entries(table_size, sig_a);
				for (int i = 0; i < select_width; i++) {
					for(int value:values[i]) {
					if (value < 0 || value >= table_size || seen[value]) {
						valid = false;
						break;
					}
					seen[value] = true;
					table_entries[value] = sig_b.extract(i * width, width);
					}
				}
				if (!valid)
					continue;

				// Large all-constant tables are normally better left intact so the
				// normal memory/ROM passes can choose their own representation.
				bool all_table_constant = true;
				for (auto &entry : table_entries)
					if (!entry.is_fully_const())
						all_table_constant = false;
				if (!allow_rom && key_width >= 5 && all_table_constant)
					continue;

				RewriteCandidate candidate;
				candidate.cell = cell;
				candidate.key = common_key;
				candidate.output = sig_y;
				candidate.table = std::move(table_entries);
				candidate.width = width;
				candidate.key_width = key_width;
				candidate.select_width = select_width;
				candidates.push_back(std::move(candidate));
			}

			std::vector<std::vector<RewriteCandidate *>> groups;
			for (auto &candidate : candidates) {
				bool placed = false;
				for (auto &group : groups) {
					if (group.front()->width == candidate.width &&
						group.front()->key == candidate.key) {
						group.push_back(&candidate);
						placed = true;
						break;
					}
				}
				if (!placed)
					groups.push_back({&candidate});
			}
			if (anchor_gate) {
				bool anchor = false;
				for (auto &group : groups) {
					int width = group.front()->width;
					long long variable_bits = 0, data_bits = 0;
					int original_branch_cost = 0;
					for (auto candidate : group) {
						RTLIL::SigSpec branch_data = sigmap(candidate->cell->getPort(ID::B));
						data_bits += GetSize(branch_data);
						original_branch_cost += candidate->select_width * width;
						for (auto bit : branch_data)
							if (bit.wire != nullptr)
								variable_bits++;
					}
					bool highfanin_onebit = width == 1 &&
						std::any_of(group.begin(), group.end(), [](RewriteCandidate *candidate) {
							return candidate->select_width >= 8;
						});
					bool low_risk_shape = width >= 64 ||
						(width >= 16 && variable_bits * 4 <= data_bits * 3) ||
						(width == 1 && (!guard_onebit_highfanin || !highfanin_onebit) &&
						 GetSize(group) >= 8 && variable_bits * 4 <= data_bits);
					if (!low_risk_shape)
						continue;
					std::vector<int> order(group.front()->key_width);
					std::iota(order.begin(), order.end(), 0);
					int best_nodes = dag_cost(group, order);
					if (GetSize(order) <= 6)
						while (std::next_permutation(order.begin(), order.end()))
							best_nodes = std::min(best_nodes, dag_cost(group, order));
					long long saved = original_branch_cost - best_nodes * width;
					if (saved >= 0 && saved * 100 >=
							static_cast<long long>(original_branch_cost) * min_saving) {
						anchor = true;
						break;
					}
				}
				if (!anchor) {
					log("  Preserving %d decoded PMUX group(s) in %s: no low-risk module anchor.\n",
						GetSize(groups), log_id(module));
					groups.clear();
				}
			}

			for (auto &group : groups) {
                if(design->scratchpad_get_bool("v49.recode")&&recode_classes(module,group)){rewritten+=GetSize(group);continue;}
                if(native_mux4&&native_mux4_group(module,sigmap,group)){rewritten+=GetSize(group);continue;}
                if(native_mux4&&min_saving==100&&min_bit_saving==101&&!coalesce_bits)continue;
				int key_width = group.front()->key_width;
				int width = group.front()->width;
				if (guard_onebit_highfanin && width == 1 &&
					std::any_of(group.begin(), group.end(), [](RewriteCandidate *candidate) {
						return candidate->select_width >= 8;
					}))
					continue;
				int original_branch_cost = 0;
				for (auto candidate : group)
					original_branch_cost += candidate->select_width * width;
				std::vector<int> order(key_width);
				std::iota(order.begin(), order.end(), 0);
				std::vector<int> best_order = order;
				int best_word_nodes = dag_cost(group, order);

				// Exhaustive ordering is inexpensive for the small decoded keys that
				// dominate control and routing logic.
				if (key_width <= 6 && min_saving<100) {
                    int evaluated=1;
					while ((!order_limit||evaluated++<order_limit)&&std::next_permutation(order.begin(), order.end())) {
						int word_nodes = dag_cost(group, order);
						if (word_nodes < best_word_nodes) {
							best_word_nodes = word_nodes;
							best_order = order;
						}
					}
				}

				int best_cost = best_word_nodes * width;
				long long saved_cost = original_branch_cost - best_cost;
				bool use_word = saved_cost >= 0 && saved_cost * 100 >=
						static_cast<long long>(original_branch_cost) * min_saving;
				bool use_bits = false;
				if (!use_word && min_bit_saving <= 100) {
					int best_bit_nodes = bit_dag_cost(group, best_order);
					std::vector<int> bit_order(key_width);
					std::iota(bit_order.begin(), bit_order.end(), 0);
                    int evaluated=1;
					if (key_width <= 6)
						while ((!order_limit||evaluated++<order_limit)&&std::next_permutation(bit_order.begin(), bit_order.end())) {
							int nodes = bit_dag_cost(group, bit_order);
							if (nodes < best_bit_nodes) {
								best_bit_nodes = nodes;
								best_order = bit_order;
							}
						}
					best_cost = best_bit_nodes;
					saved_cost = original_branch_cost - best_cost;
					use_bits = saved_cost >= 0 && saved_cost * 100 >=
							static_cast<long long>(original_branch_cost) * min_bit_saving;
				}
				if (!use_word && !use_bits)
					continue;

				log("  Factoring %d decoded PMUX(es) in %s using %s DAG: original-branch-bit-cost=%d, shared-bit-cost=%d, saving=%lld%%.\n",
					GetSize(group), log_id(module), use_bits ? "bit-plane" : "word",
					original_branch_cost, best_cost,
					original_branch_cost ? saved_cost * 100 / original_branch_cost : 0);
				dict<RTLIL::SigSpec, RTLIL::SigSpec> cache;
				int group_created = 0;
				for (auto candidate : group) {
					RTLIL::SigSpec result;
					if (use_bits) {
						for (int bit = 0; bit < candidate->width; bit++) {
							std::vector<RTLIL::SigSpec> plane;
							plane.reserve(candidate->table.size());
							for (auto &entry : candidate->table)
								plane.push_back(entry.extract(bit, 1));
							result.append(build_mux_dag(module, candidate->key,
								plane, best_order, 0, 0, cache, group_created));
						}
					} else {
						result = build_mux_dag(module, candidate->key,
							candidate->table, best_order, 0, 0, cache, group_created);
					}
					module->connect(candidate->output, result);
					log("    Replaced %s.%s: WIDTH=%d, S_WIDTH=%d.\n",
						log_id(module), log_id(candidate->cell), candidate->width,
						candidate->select_width);
				module->remove(candidate->cell);
				rewritten++;
				}
				created_muxes += group_created;
			}
			if (coalesce_bits) {
				std::vector<RTLIL::Cell *> remaining_pmux;
				for (auto cell : module->selected_cells())
					if (cell->type == ID($pmux))
						remaining_pmux.push_back(cell);
				for (auto cell : remaining_pmux) {
					int created = 0;
					if (coalesce_pmux(module, cell, sigmap, coalesce_min_saving, created)) {
						coalesced++;
						rewritten++;
					}
				}
			}
		}

        if(rewritten)design->scratchpad_set_bool("pmux_v31.changed",true);
		log("pmux_opt: examined %d selected $pmux cell(s), rewrote %d into %d shared $mux cell(s).\n",
			examined, rewritten, created_muxes);
		log("pmux_opt: selector-coalescing rewrote %d $pmux cell(s).\n", coalesced);
	}
} PmuxOptPass;

PRIVATE_NAMESPACE_END
