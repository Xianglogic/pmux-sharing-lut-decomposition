/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *  Copyright (C) 2024  Richard Herveille <richard.herveille@roalogic.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/celltypes.h"
#include "kernel/log.h"
#include "kernel/register.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct SynthIntelPass : public ScriptPass {
	SynthIntelPass() : ScriptPass("synth_intel", "synthesis for Intel (Altera) FPGAs.") { experimental(); }

	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    synth_intel [options]\n");
		log("\n");
		log("This command runs synthesis for Intel FPGAs.\n");
		log("\n");
		log("    -family <max10 | cyclone10lp | cycloneiv | cycloneive>\n");
		log("        generate the synthesis netlist for the specified family.\n");
		log("        MAX10 is the default target if no family argument specified.\n");
		log("        For Cyclone IV GX devices, use cycloneiv argument; for Cyclone IV E, use\n");
		log("        cycloneive. For Cyclone V and Cyclone 10 GX, use the synth_intel_alm\n");
		log("        backend instead.\n");
		log("\n");
		log("    -top <module>\n");
		log("        use the specified module as top module (default='top')\n");
		log("\n");
		log("    -vqm <file>\n");
		log("        write the design to the specified Verilog Quartus Mapping File. Writing\n");
		log("        of an output file is omitted if this parameter is not specified.\n");
		log("        Note that this backend has not been tested and is likely incompatible\n");
		log("        with recent versions of Quartus.\n");
		log("\n");
		log("    -vpr <file>\n");
		log("        write BLIF files for VPR flow experiments. The synthesized BLIF output\n");
		log("        file is not compatible with the Quartus flow. Writing of an\n");
		log("        output file is omitted if this parameter is not specified.\n");
		log("\n");
		log("    -run <from_label>:<to_label>\n");
		log("        only run the commands between the labels (see below). an empty\n");
		log("        from label is synonymous to 'begin', and empty to label is\n");
		log("        synonymous to the end of the command list.\n");
		log("\n");
		log("    -dff\n");
		log("        pass DFFs to ABC to perform sequential logic optimisations\n");
		log("        (EXPERIMENTAL)\n");
		log("\n");
		log("    -iopads\n");
		log("        use IO pad cells in output netlist\n");
		log("\n");
		log("    -nobram\n");
		log("        do not use block RAM cells in output netlist\n");
		log("\n");
		log("    -nodsp\n");
		log("        do not map multipliers to MUL18/MUL9 cells\n");
		log("\n");
		log("    -noflatten\n");
		log("        do not flatten design before synthesis\n");
		log("\n");
		log("The following commands are executed by this synthesis command:\n");
		help_script();
		log("\n");
	}

	string top_opt, family_opt, vout_file, blif_file;
	bool flatten, nobram, dff, nodsp, iopads;

	void clear_flags() override
	{
		top_opt = "-auto-top";
		family_opt = "max10";
		vout_file = "";
		blif_file = "";
		flatten = true;
		nobram = false;
		dff = false;
		nodsp = false;
		iopads = false;
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		string run_from, run_to;
		clear_flags();
        // V49 compliance retest: original backend restored; area rules retained. Built-in defaults; identical external synth_intel scripts.
        for(auto key:{"v49.recode","v49.windows","v49.sync_clear","v49.ff_mux"})design->scratchpad_set_bool(key,true);

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-family" && argidx + 1 < args.size()) {
				family_opt = args[++argidx];
				continue;
			}
			if (args[argidx] == "-top" && argidx + 1 < args.size()) {
				top_opt = "-top " + args[++argidx];
				continue;
			}
			if (args[argidx] == "-vqm" && argidx + 1 < args.size()) {
				vout_file = args[++argidx];
				log_warning("The Quartus backend has not been tested recently and is likely incompatible with modern versions of Quartus.\n");
				continue;
			}
			if (args[argidx] == "-vpr" && argidx + 1 < args.size()) {
				blif_file = args[++argidx];
				continue;
			}
			if (args[argidx] == "-run" && argidx + 1 < args.size()) {
				size_t pos = args[argidx + 1].find(':');
				if (pos == std::string::npos)
					break;
				run_from = args[++argidx].substr(0, pos);
				run_to = args[argidx].substr(pos + 1);
				continue;
			}
			if (args[argidx] == "-iopads") {
				iopads = true;
				continue;
			}
			if (args[argidx] == "-dff") {
				dff = true;
				continue;
			}
			if (args[argidx] == "-nodsp") {
				nodsp = true;
				continue;
			}
			if (args[argidx] == "-nobram") {
				nobram = true;
				continue;
			}
			if (args[argidx] == "-noflatten") {
				flatten = false;
				continue;
			}
			if (args[argidx] == "-retime") {
				// Removed: ABC9 does not support retiming.
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (!design->full_selection())
			log_cmd_error("This command only operates on fully selected designs!\n");

		if (family_opt == "cyclonev")
			log_cmd_error("Cyclone V synthesis has been moved to synth_intel_alm.\n");

		if (family_opt != "max10" &&
		    family_opt != "cycloneiv" &&
		    family_opt != "cycloneive" &&
		    family_opt != "cyclone10lp")
			log_cmd_error("Invalid or no family specified: '%s'\n", family_opt);

		if (family_opt != "max10")
			nodsp = true;

		log_header(design, "Executing SYNTH_INTEL pass.\n");
		log_push();

		run_script(design, run_from, run_to);

		log_pop();
	}

	// Cheap necessary conditions, recomputed after every potentially modifying pass.
	// No mapper changes: only inapplicable custom passes are omitted.
	bool eligible_type(RTLIL::IdString type, int lo=0, int hi=0, int minimum=1, int selects=0)
	{
		if(help_mode)return true;
        if(active_design->scratchpad_get_bool("v52.observable_selector"))return false;
		for(auto m:active_design->selected_modules()) {
			int count=0;
			for(auto c:m->selected_cells())if(c->type==type) {
				if(lo) {
					int width=type==ID($adff)?GetSize(c->getPort(ID::Q)):c->getParam(ID::WIDTH).as_int();
					if(width<lo||width>hi)continue;
				}
				if(selects&&c->getParam(ID::S_WIDTH).as_int()!=selects)continue;
				if(++count>=minimum)return true;
			}
		}
		return false;
	}
    bool referenced_type(RTLIL::IdString type) {
        if(help_mode)return true;
        for(auto m:active_design->modules())for(auto c:m->cells())if(c->type==type)return true;
        return false;
    }
    bool eligible_order_comparison() {
        return eligible_type(ID($lt))||eligible_type(ID($le))||eligible_type(ID($gt))||eligible_type(ID($ge));
    }
	bool eligible_resub()
	{
		if(help_mode)return true;
		for(auto m:active_design->selected_modules()) {
			int count=0;for(auto c:m->cells())if(c->type==RTLIL::escape_id("cycloneiv_lcell_comb"))count++;
			if(count>=3&&count<=64)return true;
		}
		return false;
	}

    bool eligible_pair_rewrite() {
        if(help_mode)return true;
        if(active_design->scratchpad_get_bool("v52.observable_selector"))return false;
        for(auto m:active_design->selected_modules()) {
            bool mux=false,key=false;
            for(auto c:m->selected_cells()) {
                if(c->type==ID($pmux)) {
                    int width=c->getParam(ID::WIDTH).as_int(),n=c->getParam(ID::S_WIDTH).as_int();
                    if(width>=2&&width<=256&&n>=3&&n<=4)mux=true;
                }
                if(c->type==ID($eq)&&GetSize(c->getPort(ID::A))==2&&GetSize(c->getPort(ID::B))==2)key=true;
                if(mux&&key)return true;
            }
        }
        return false;
    }
	void reset_custom_changes()
	{
		if(!help_mode)active_design->scratchpad_set_bool("pmux_v31.changed",false);
	}
	void clean_custom_changes()
	{
		if(help_mode||active_design->scratchpad_get_bool("pmux_v31.changed"))run("opt_clean");
	}


	void script() override
	{
		if (check_label("begin")) {
			if (check_label("family"))
				run(stringf("read_verilog -sv -lib +/intel/%s/cells_sim.v", family_opt));

			// Misc and common cells
			run("read_verilog -sv -lib +/intel/common/m9k_bb.v");
			run("read_verilog -sv -lib +/intel/common/altpll_bb.v");
			run(stringf("hierarchy -check %s", help_mode ? "-top <top>" : top_opt));
		}

		if (check_label("coarse")) {
			run("proc");
			if (flatten || help_mode) {
				run("check");
				run("flatten", "(skip if -noflatten)");
			}
			run("tribuf -logic");
			run("deminout");
			run("opt_expr");
			run("opt_clean");
			run("check");

            // Preserve the established V47 path for writable memories. This
            // avoids changing FIFO data/selector structure and adds no state
            // assumptions. It is a structural policy independent of names.
            bool legacy_memory=family_opt!="cycloneiv"&&family_opt!="cycloneive";
            if(!help_mode)for(auto m:active_design->selected_modules())for(auto c:m->cells()) {
                if(c->type.str().rfind("$memwr",0)==0 ||
                   (c->type==ID($mem_v2)&&c->getParam(ID::WR_PORTS).as_int()>0))legacy_memory=true;
            }
            if(!help_mode)active_design->scratchpad_set_bool("pmux_v48.legacy_flow",legacy_memory);
            if(!help_mode&&legacy_memory)active_design->scratchpad_set_bool("v49.recode",false);
            if(legacy_memory)log("V48: writable memory or unsupported family; retaining the V47 optimization and mapping path.\n");
			run("opt -nodffe -nosdff");
			run("fsm");
			run("opt");
            // Conservative algorithm boundary: externally observed decode keys
            // can interact with state/data aliases and upstream ABC9 stitching.
            // Retain the entire standard flow on this structural class.
            bool observable=false;
            if(!help_mode)for(auto m:active_design->selected_modules()) {
                SigMap map(m);pool<RTLIL::SigBit> outputs;dict<RTLIL::SigBit,RTLIL::Cell*> drivers;
                for(auto w:m->wires())if(w->port_output)for(auto b:map(w))outputs.insert(b);
                for(auto c:m->cells())if(c->type.in(ID($eq),ID($logic_not))) {
                    auto y=map(c->getPort(ID::Y));if(GetSize(y)==1)drivers[y[0]]=c;
                }
                for(auto c:m->cells())if(c->type==ID($pmux))for(auto sel:map(c->getPort(ID::S)))if(drivers.count(sel)) {
                    auto pred=drivers.at(sel);
                    for(auto port:{ID::A,ID::B})if(pred->hasPort(port))for(auto b:map(pred->getPort(port)))if(b.wire&&outputs.count(b))observable=true;
                }
            }
            if(!help_mode)active_design->scratchpad_set_bool("v52.observable_selector",observable);
            if(observable) {
                active_design->scratchpad_set_bool("pmux_v48.legacy_flow",true);
                active_design->scratchpad_set_bool("v49.recode",false);
                log("V52: exported decode key; retaining stock synthesis and ABC9 script.\n");
            }

            if(!help_mode)active_design->scratchpad_set_bool("v53.changed",false);
			reset_custom_changes();
			if(eligible_type(ID($adff),2,6))run("guarded_reuse_stage");
			if ((family_opt == "cycloneiv" || family_opt == "cycloneive")&&eligible_type(ID($pmux),8,512,2,16))
				run("pmux_cofactor_share");
            if((family_opt=="cycloneiv"||family_opt=="cycloneive")&&eligible_pair_rewrite())run("pmux_nested_addsub; pmux_addsub_share; pmux_pair_cut");
			if(eligible_type(ID($pmux)))run(!help_mode&&active_design->scratchpad_get_bool("pmux_v48.legacy_flow")?"pmux_opt":"pmux_opt -native-mux4 -min-branches 2 -min-saving 100 -min-bit-saving 101");
			if(eligible_type(ID($mux),8,512,2))run("pmux_cross_share");
			clean_custom_changes();
			reset_custom_changes();
			// Three distinct selector levels need at least three mux cells.
			if(eligible_type(ID($mux),8,512,3))run("pmux_shared_read");
			// A three/four-level constant-default priority chain needs at least three muxes.
			if(eligible_type(ID($mux),1,256,3))run("priority_pmux_opt");
			if(eligible_type(ID($eq))&&(eligible_type(ID($mux))||eligible_type(ID($pmux))))
                run(string("pmux_compare_const") + ((family_opt=="cycloneiv"||family_opt=="cycloneive") ? " -preserve-powerup-low" : ""));
			clean_custom_changes();
			if ((family_opt == "cycloneiv" || family_opt == "cycloneive")&&eligible_type(ID($eq)))
				run(!help_mode&&active_design->scratchpad_get_bool("pmux_v48.legacy_flow")?"eq_prefix_share":"eq_prefix_share -hybrid");
			run("wreduce");
			run("peepopt");
			run("opt_clean");
			run("techmap -map +/cmp2lut.v -D LUT_WIDTH=4");
			run("opt_expr");
			run("opt_clean");

			if (help_mode) {
				run("techmap -map +mul2dsp.v [...]", "(unless -nodsp)");
			} else if (!nodsp) {
				run("techmap -map +/mul2dsp.v -D DSP_A_MAXWIDTH=18 -D DSP_B_MAXWIDTH=18  -D DSP_A_MINWIDTH=10 -D DSP_B_MINWIDTH=4 -D DSP_NAME=$__MUL18X18");
					run("chtype -set $mul t:$__soft_mul");
					run("techmap -map +/mul2dsp.v -D DSP_A_MAXWIDTH=18 -D DSP_B_MAXWIDTH=18  -D DSP_A_MINWIDTH=4 -D DSP_B_MINWIDTH=10 -D DSP_NAME=$__MUL18X18");
					run("chtype -set $mul t:$__soft_mul");
					run("techmap -map +/mul2dsp.v -D DSP_A_MAXWIDTH=9 -D DSP_B_MAXWIDTH=9  -D DSP_A_MINWIDTH=4 -D DSP_B_MINWIDTH=4 -D DSP_NAME=$__MUL9X9");
					run("chtype -set $mul t:$__soft_mul");
				run("alumacc");
				run(stringf("techmap -map +/intel/%s/dsp_map.v", family_opt));
			} else {
				run("alumacc");
			}
			run("opt");
			run("memory -nomap");
			run("opt_clean");
            bool large_rom=false;
            if(!help_mode)for(auto m:active_design->selected_modules())for(auto c:m->cells())
                if(c->type==ID($mem_v2)&&c->getParam(ID::WR_PORTS).as_int()==0&&
                   c->getParam(ID::SIZE).as_int()>=64)large_rom=true;
            if(!help_mode)active_design->scratchpad_set_bool("pmux_v48.large_rom",large_rom);
			reset_custom_changes();
			if(eligible_type(ID($mem_v2)))run("rom_cofactor_opt");
			clean_custom_changes();
		}

		if (!nobram && check_label("map_bram", "(skip if -nobram)")) {
				if (family_opt == "cycloneiv" ||
				    family_opt == "cycloneive" ||
				    family_opt == "max10" ||
				    help_mode) {
                run("memory_bram -rules +/intel/common/brams_m9k.txt", "(if applicable for family)");
				run("techmap -map +/intel/common/brams_map_m9k.v", "(if applicable for family)");
			 } else {
				log_warning("BRAM mapping is not currently supported for %s.\n", family_opt);
			}
		}

		if (check_label("map_ffram")) {
			run("opt -fast -mux_undef -undriven -fine -full");
			run("memory_map");
			run("opt -undriven -fine");
			run("techmap -map +/techmap.v");
			run("opt -full");
			run("clean -purge");
			run("setundef -undriven -zero");
		}

		if (check_label("map_ffs")) {
            if(!help_mode&&active_design->scratchpad_get_bool("v49.sync_clear")&&!active_design->scratchpad_get_bool("pmux_v48.legacy_flow")&&
                (family_opt=="cycloneiv"||family_opt=="cycloneive"))run("pmux_sync_clear");
			run("dfflegalize -cell $_DFFE_PN0P_ 01");
            run("techmap -map +/intel/common/ff_map.v");
            if(!help_mode&&active_design->scratchpad_get_bool("v49.ff_mux")&&!active_design->scratchpad_get_bool("pmux_v48.legacy_flow")&&
                (family_opt=="cycloneiv"||family_opt=="cycloneive"))run("pmux_ff_input_mux");
		}

		if (check_label("map_luts")) {
            run("pmux_pair_finish");
            run("pmux_priority_finish");
            run("pmux_rom_finish");
            if(!help_mode&&!dff&&!active_design->scratchpad_get_bool("pmux_v48.legacy_flow")&&
               (family_opt=="cycloneiv"||family_opt=="cycloneive"))run("pmux_small_cones");
            run(string(help_mode ? "abc9" : "pmux_abc9_script") + " -lut 4 -W 300" + string(dff ? " -dff" : ""));
			run("clean");
			run("opt -fast");
			run("autoname");
			run("clean");
		}

		if (check_label("map_cells")) {
			if (iopads || help_mode)
				run("iopadmap -bits -outpad $__outpad I:O -inpad $__inpad O:I", "(if -iopads)");
			run(stringf("techmap -map +/intel/%s/cells_map.v", family_opt));
			run("clean -purge");
		}

		if (check_label("check")) {
            reset_custom_changes();
            // Conservative QA147 candidate: no custom post-ABC9 LUT rewriting.
            // Optional exhaustive resubstitution is excluded from this release.
            clean_custom_changes();
			run("hierarchy -check");
			run("stat");
			run("check -noinit");
			run("blackbox =A:whitebox");
		}

		if (check_label("vqm")) {
			if (!vout_file.empty() || help_mode)
				run(stringf("write_verilog -attr2comment -defparam -nohex -decimal -renameprefix syn_ %s",
					    help_mode ? "<file-name>" : vout_file.c_str()));
		}

		if (check_label("vpr")) {
			if (!blif_file.empty() || help_mode) {
				run(stringf("opt_clean -purge"));
				run(stringf("write_blif %s", help_mode ? "<file-name>" : blif_file));
			}
		}
	}
} SynthIntelPass;

PRIVATE_NAMESPACE_END

