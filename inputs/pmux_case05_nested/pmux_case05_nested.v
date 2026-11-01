
module pmux_case05_nested #(
    parameter integer LANES = 1,
    parameter integer WIDTH = 12
)(
    input wire clk,
    input wire rst,
    input wire en,
    input wire [LANES*4*WIDTH-1:0] data_in,
    input wire [LANES*16-1:0] ctrl_in,
    output wire [LANES*WIDTH-1:0] data_out
);

    genvar i;
    generate for (i = 0; i < LANES; i = i + 1) begin: lane
        wire [WIDTH-1:0] a = data_in[i*4*WIDTH +: WIDTH];
        wire [WIDTH-1:0] b = data_in[i*4*WIDTH+WIDTH +: WIDTH];
        wire [WIDTH-1:0] c = data_in[i*4*WIDTH+2*WIDTH +: WIDTH];
        wire [WIDTH-1:0] d = data_in[i*4*WIDTH+3*WIDTH +: WIDTH];
        wire [15:0] s = ctrl_in[i*16 +: 16];
        reg [WIDTH-1:0] q;
        assign data_out[i*WIDTH +: WIDTH] = q;
        reg [WIDTH-1:0] next_q;
        // Actual nested procedural case statements: up to three pmux levels.
        always @* begin
            case (s[1:0])
                2'd0: begin
                    case (s[3:2])
                        2'd0: begin
                            case (s[5:4])
                                2'd0: next_q = a ^ q;
                                2'd1: next_q = b + q;
                                2'd2: next_q = c - q;
                                default: next_q = d;
                            endcase
                        end
                        2'd1: next_q = a + b;
                        2'd2: next_q = b ^ c;
                        default: next_q = q;
                    endcase
                end
                2'd1: begin
                    case (s[7:6])
                        2'd0: next_q = a | c;
                        2'd1: next_q = b & d;
                        2'd2: next_q = c ^ q;
                        default: next_q = d + q;
                    endcase
                end
                2'd2: begin
                    case (s[9:8])
                        2'd0: next_q = a - q;
                        2'd1: next_q = b | q;
                        2'd2: next_q = c + d;
                        default: next_q = q ^ d;
                    endcase
                end
                default: next_q = q + a;
            endcase
        end
        always @(posedge clk) begin
            if (rst) q <= {WIDTH{1'b0}};
            else if (en) q <= next_q;
        end
    end endgenerate
endmodule

