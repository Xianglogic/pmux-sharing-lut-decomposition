
module pmux_case01_balanced #(
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
        reg [WIDTH-1:0] x0, x1, x2, x3, next_q;
        always @* begin
            case (s[1:0])
                2'd0: x0 = a ^ q;
                2'd1: x0 = b + q;
                2'd2: x0 = c - q;
                default: x0 = d;
            endcase
        end
        always @* begin
            case (s[3:2])
                2'd0: x1 = b ^ q;
                2'd1: x1 = c + a;
                2'd2: x1 = d - b;
                default: x1 = a;
            endcase
        end
        always @* begin
            case (s[5:4])
                2'd0: x2 = c ^ q;
                2'd1: x2 = d + b;
                2'd2: x2 = a - c;
                default: x2 = b;
            endcase
        end
        always @* begin
            case (s[7:6])
                2'd0: x3 = d ^ q;
                2'd1: x3 = a + c;
                2'd2: x3 = b - d;
                default: x3 = c;
            endcase
        end
        always @* begin
            case (s[9:8])
                2'd0: next_q = x0;
                2'd1: next_q = x1;
                2'd2: next_q = x2;
                default: next_q = x3;
            endcase
        end
        always @(posedge clk) begin
            if (rst) q <= {WIDTH{1'b0}};
            else if (en) q <= next_q;
        end
    end endgenerate
endmodule

