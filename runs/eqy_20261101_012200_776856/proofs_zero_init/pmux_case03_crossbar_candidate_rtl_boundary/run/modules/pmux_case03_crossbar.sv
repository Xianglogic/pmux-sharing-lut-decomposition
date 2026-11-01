module miter (
  input  [  0:0] \__pi_clk ,
  input  [ 31:0] \__pi_ctrl_in ,
  input  [ 39:0] \__pi_data_in ,
  input  [  0:0] \__pi_en ,
  input  [  0:0] \__pi_rst ,
`ifdef DIRECT_CROSS_POINTS
`else
`endif
  output [  9:0] \__po_data_out__gold ,
  output [  9:0] \__po_data_out__gate
);
  \gold.pmux_case03_crossbar gold (
    .\__pi_clk (\__pi_clk ),
    .\__pi_ctrl_in (\__pi_ctrl_in ),
    .\__pi_data_in (\__pi_data_in ),
    .\__pi_en (\__pi_en ),
    .\__pi_rst (\__pi_rst ),
`ifdef DIRECT_CROSS_POINTS
`else
`endif
    .\__po_data_out (\__po_data_out__gold )
  );
  \gate.pmux_case03_crossbar gate (
    .\__pi_clk (\__pi_clk ),
    .\__pi_ctrl_in (\__pi_ctrl_in ),
    .\__pi_data_in (\__pi_data_in ),
    .\__pi_en (\__pi_en ),
    .\__pi_rst (\__pi_rst ),
`ifdef DIRECT_CROSS_POINTS
`else
`endif
    .\__po_data_out (\__po_data_out__gate )
  );
`ifdef ASSUME_DEFINED_INPUTS
  miter_def_prop #(1, "assume") \__pi_clk__assume (\__pi_clk );
  miter_def_prop #(32, "assume") \__pi_ctrl_in__assume (\__pi_ctrl_in );
  miter_def_prop #(40, "assume") \__pi_data_in__assume (\__pi_data_in );
  miter_def_prop #(1, "assume") \__pi_en__assume (\__pi_en );
  miter_def_prop #(1, "assume") \__pi_rst__assume (\__pi_rst );
`endif
`ifndef DIRECT_CROSS_POINTS
`endif
`ifdef CHECK_MATCH_POINTS
`endif
`ifdef CHECK_OUTPUTS
  miter_cmp_prop #(10, "assert") \__po_data_out__assert (\__po_data_out__gold , \__po_data_out__gate );
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
  miter_def_prop #(10, "cover") \__po_data_out__gold_cover (\__po_data_out__gold );
`endif
`ifdef COVER_DEF_GATE_OUTPUTS
  miter_def_prop #(10, "cover") \__po_data_out__gate_cover (\__po_data_out__gate );
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
module \gold.pmux_case03_crossbar (
  input  [  0:0] \__pi_clk ,
  input  [ 31:0] \__pi_ctrl_in ,
  input  [ 39:0] \__pi_data_in ,
  input  [  0:0] \__pi_en ,
  input  [  0:0] \__pi_rst ,
  output [  9:0] \__po_data_out
);
endmodule
module \gate.pmux_case03_crossbar (
  input  [  0:0] \__pi_clk ,
  input  [ 31:0] \__pi_ctrl_in ,
  input  [ 39:0] \__pi_data_in ,
  input  [  0:0] \__pi_en ,
  input  [  0:0] \__pi_rst ,
  output [  9:0] \__po_data_out
);
endmodule
