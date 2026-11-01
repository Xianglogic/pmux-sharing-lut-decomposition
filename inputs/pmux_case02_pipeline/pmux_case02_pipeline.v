
module pmux_case02_pipeline #(
    parameter integer LANES = 4,
    parameter integer WIDTH = 3
)(
    input wire clk,
    input wire rst,
    input wire en,
    input wire [LANES*4*WIDTH-1:0] data_in,
    input wire [LANES*16-1:0] ctrl_in,
    output wire [LANES*WIDTH-1:0] data_out
);
    wire [LANES*WIDTH-1:0] mid_bus;
    genvar i;
    generate for (i = 0; i < LANES; i = i + 1) begin: lane
        wire [WIDTH-1:0] a = data_in[i*4*WIDTH +: WIDTH];
        wire [WIDTH-1:0] b = data_in[i*4*WIDTH+WIDTH +: WIDTH];
        wire [WIDTH-1:0] c = data_in[i*4*WIDTH+2*WIDTH +: WIDTH];
        wire [WIDTH-1:0] d = data_in[i*4*WIDTH+3*WIDTH +: WIDTH];
        wire [15:0] s = ctrl_in[i*16 +: 16];
        reg [WIDTH-1:0] q;
        assign data_out[i*WIDTH +: WIDTH] = q;
        reg [WIDTH-1:0] mid;
        reg [WIDTH-1:0] x0, x1, first, y0, y1, next_q;
        wire [WIDTH-1:0] left_mid = mid_bus[((i+LANES-1)%LANES)*WIDTH +: WIDTH];
        wire [WIDTH-1:0] right_mid = mid_bus[((i+1)%LANES)*WIDTH +: WIDTH];
        assign mid_bus[i*WIDTH +: WIDTH] = mid;
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
                2'd0: x1 = b ^ c;
                2'd1: x1 = c + d;
                2'd2: x1 = d - a;
                default: x1 = a;
            endcase
        end
        always @* begin
            case (s[5:4])
                2'd0: first = x0;
                2'd1: first = x1;
                2'd2: first = a;
                default: first = q;
            endcase
        end
        always @* begin
            case (s[7:6])
                2'd0: y0 = mid ^ q;
                2'd1: y0 = left_mid + a;
                2'd2: y0 = right_mid - b;
                default: y0 = c;
            endcase
        end
        always @* begin
            case (s[9:8])
                2'd0: y1 = left_mid ^ d;
                2'd1: y1 = right_mid + q;
                2'd2: y1 = mid - c;
                default: y1 = b;
            endcase
        end
        always @* begin
            case (s[11:10])
                2'd0: next_q = y0;
                2'd1: next_q = y1;
                2'd2: next_q = left_mid;
                default: next_q = right_mid;
            endcase
        end
        always @(posedge clk) begin
            if (rst) begin
                mid <= {WIDTH{1'b0}};
                q <= {WIDTH{1'b0}};
            end else if (en) begin
                mid <= first;
                q <= next_q;
            end
        end
    end endgenerate
endmodule

