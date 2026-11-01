module miter (
  input  [  0:0] \__pi_clk ,
  input  [  0:0] \__pi_cs ,
  input  [  0:0] \__pi_sr_in ,
`ifdef DIRECT_CROSS_POINTS
`else
`endif
  output [  7:0] \__po_gpioout__gold ,
  output [  0:0] \__po_sr_out__gold ,
  output [  7:0] \__po_gpioout__gate ,
  output [  0:0] \__po_sr_out__gate
);
  \gold.test3 gold (
    .\__pi_clk (\__pi_clk ),
    .\__pi_cs (\__pi_cs ),
    .\__pi_sr_in (\__pi_sr_in ),
`ifdef DIRECT_CROSS_POINTS
`else
`endif
    .\__po_gpioout (\__po_gpioout__gold ),
    .\__po_sr_out (\__po_sr_out__gold )
  );
  \gate.test3 gate (
    .\__pi_clk (\__pi_clk ),
    .\__pi_cs (\__pi_cs ),
    .\__pi_sr_in (\__pi_sr_in ),
`ifdef DIRECT_CROSS_POINTS
`else
`endif
    .\__po_gpioout (\__po_gpioout__gate ),
    .\__po_sr_out (\__po_sr_out__gate )
  );
`ifdef ASSUME_DEFINED_INPUTS
  miter_def_prop #(1, "assume") \__pi_clk__assume (\__pi_clk );
  miter_def_prop #(1, "assume") \__pi_cs__assume (\__pi_cs );
  miter_def_prop #(1, "assume") \__pi_sr_in__assume (\__pi_sr_in );
`endif
`ifndef DIRECT_CROSS_POINTS
`endif
`ifdef CHECK_MATCH_POINTS
`endif
`ifdef CHECK_OUTPUTS
  miter_cmp_prop #(8, "assert") \__po_gpioout__assert (\__po_gpioout__gold , \__po_gpioout__gate );
  miter_cmp_prop #(1, "assert") \__po_sr_out__assert (\__po_sr_out__gold , \__po_sr_out__gate );
`endif
`ifdef COVER_DEF_CROSS_POINTS
  `ifdef DIRECT_CROSS_POINTS
  `else
  `endif
`endif
`ifdef COVER_DEF_GOLD_MATCH_POINTS
`endif
`ifdef COVER_DEF_GATE_MATCH_POINTS
`endif
`ifdef COVER_DEF_GOLD_OUTPUTS
  miter_def_prop #(8, "cover") \__po_gpioout__gold_cover (\__po_gpioout__gold );
  miter_def_prop #(1, "cover") \__po_sr_out__gold_cover (\__po_sr_out__gold );
`endif
`ifdef COVER_DEF_GATE_OUTPUTS
  miter_def_prop #(8, "cover") \__po_gpioout__gate_cover (\__po_gpioout__gate );
  miter_def_prop #(1, "cover") \__po_sr_out__gate_cover (\__po_sr_out__gate );
`endif
endmodule
module miter_cmp_prop #(parameter WIDTH=1, parameter TYPE="assert") (input [WIDTH-1:0] in_gold, in_gate);
  reg okay;
  integer i;
  always @* begin
    okay = 1;
    for (i = 0; i < WIDTH; i = i+1)
      okay = okay && (in_gold[i] === 1'bx || in_gold[i] === in_gate[i]);
  end
  generate
    if (TYPE == "assert") always @* assert(okay);
    if (TYPE == "assume") always @* assume(okay);
    if (TYPE == "cover")  always @* cover(okay);
  endgenerate
endmodule
module miter_def_prop #(parameter WIDTH=1, parameter TYPE="assert") (input [WIDTH-1:0] in);
  wire okay = ^in !== 1'bx;
  generate
    if (TYPE == "assert") always @* assert(okay);
    if (TYPE == "assume") always @* assume(okay);
    if (TYPE == "cover")  always @* cover(okay);
  endgenerate
endmodule
module \gold.test3 (
  input  [  0:0] \__pi_clk ,
  input  [  0:0] \__pi_cs ,
  input  [  0:0] \__pi_sr_in ,
  output [  7:0] \__po_gpioout ,
  output [  0:0] \__po_sr_out
);
endmodule
module \gate.test3 (
  input  [  0:0] \__pi_clk ,
  input  [  0:0] \__pi_cs ,
  input  [  0:0] \__pi_sr_in ,
  output [  7:0] \__po_gpioout ,
  output [  0:0] \__po_sr_out
);
endmodule
