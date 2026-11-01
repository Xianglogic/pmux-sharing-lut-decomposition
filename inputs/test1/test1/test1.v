
module test1 (
input clock,
input reset,
input valid,
input  [15:0]I0,
input  [15:0]I1,
input  [15:0]I2,
input  [15:0]I3,
input  [15:0]I4,
input  [15:0]I5,
input  [15:0]I6,
input  [15:0]I7,
input  [15:0]Q0,
input  [15:0]Q1,
input  [15:0]Q2,
input  [15:0]Q3,
input  [15:0]Q4,
input  [15:0]Q5,
input  [15:0]Q6,
input  [15:0]Q7,
output por6_out,
output inter0007_out,
output [11:0] inter0008_out,
output [11:0] inter0009_out
) ;

parameter START   = 4'b0000;
parameter CYCLE1  = 4'b0001;
parameter CYCLE2  = 4'b0010;
parameter CYCLE3  = 4'b0011;
parameter CYCLE4  = 4'b0100;
parameter CYCLE5  = 4'b0101;
parameter CYCLE6  = 4'b0110;
parameter CYCLE7  = 4'b0111;
parameter CYCLE8  = 4'b1000;
parameter CYCLE9  = 4'b1001;
parameter CYCLE10 = 4'b1010;
parameter CYCLE11 = 4'b1011;
parameter CYCLE12 = 4'b1100;
parameter CYCLE13 = 4'b1101;
parameter CYCLE14 = 4'b1110;
parameter CYCLE15 = 4'b1111;

reg    [3:0]  state;
reg por6;
reg inter0007;
reg [11:0] inter0008;
reg [11:0] inter0009;
reg [11:0] i0;
reg [11:0] i1;
reg [11:0] i2;
reg [11:0] i3;
reg [11:0] i4;
reg [11:0] i5;
reg [11:0] i6;
reg [11:0] i7;
reg [11:0] q0;
reg [11:0] q1;
reg [11:0] q2;
reg [11:0] q3;
reg [11:0] q4;
reg [11:0] q5;
reg [11:0] q6;
reg [11:0] q7;

initial  
	begin
	state =  START;
	inter0007 = 1'b1;
	por6 = 1'b0;
	end

assign por6_out = por6;
assign inter0007_out = inter0007;
assign inter0008_out = inter0008;
assign inter0009_out = inter0009;
	
always@(posedge clock, posedge reset)
begin
	if( reset == 1'b1) begin
		state <= START;
		inter0007 <= 1'b1;
	end else begin	
		case (state)
		  START:begin
		    por6 <= 1'b1; 
			inter0007 <= 1'b1;
			inter0008 <= I0[15:4];
			inter0009 <= Q0[15:4];
			i0 <= I0[15:4];
		    i1 <= I1[15:4];
		    i2 <= I2[15:4];
			i3 <= I3[15:4];
			i4 <= I4[15:4];
			i5 <= I5[15:4];
			i6 <= I6[15:4];
			i7 <= I7[15:4];
			q0 <= Q0[15:4];
			q1 <= Q1[15:4];
			q2 <= Q2[15:4];
			q3 <= Q3[15:4];
			q4 <= Q4[15:4];
			q5 <= Q5[15:4];
			q6 <= Q6[15:4];
			q7 <= Q7[15:4];
			if (valid == 1'b1)
				state <= CYCLE1;
			else
				state <= START;
			end
		  CYCLE1:begin
			inter0008 <= q0;
			inter0009 <= i0;
			state <= CYCLE2;
			por6 <= 1'b0;   
			inter0007 <= 1'b0; 
			end
		  CYCLE2:begin
			inter0008 <= i1;
			inter0009 <= q1;
			state <= CYCLE3;
			por6 <= 1'b0;   
			inter0007 <= 1'b1; 
			end
		  CYCLE3:begin
		  inter0008 <= q1;
          inter0009 <= i1;
          state <= CYCLE4;
          por6 <= 1'b0;   
          inter0007 <= 1'b0; 
		  end
		  CYCLE4:begin
		   inter0008 <= i2;
          inter0009 <= q2;
          state <= CYCLE5;
          por6 <= 1'b0;   
          inter0007 <= 1'b1;
		  end
		  CYCLE5:begin
		  inter0008 <= q2;
          inter0009 <= i2;
          state <= CYCLE6;
          por6 <= 1'b0;   
          inter0007 <= 1'b0; 
		  end
		  CYCLE6:begin
		   inter0008 <= i3;
          inter0009 <= q3;
          state <= CYCLE7;
          por6 <= 1'b0;   
          inter0007 <= 1'b1; 
		  end
		  CYCLE7:begin
		  inter0008 <= q3;
          inter0009 <= i3;
          state <= CYCLE8;
          por6 <= 1'b0;   
          inter0007 <= 1'b0; 
		  end
		  CYCLE8:begin
		  inter0008 <= i4;
          inter0009 <= q4;
          state <= CYCLE9;
          por6 <= 1'b0;   
          inter0007 <= 1'b1; 
			end
		  CYCLE9:begin
		   inter0008 <= q4;
          inter0009 <= i4;
          state <= CYCLE10;
          por6 <= 1'b0;   
          inter0007 <= 1'b0; 
		  end
		  CYCLE10:begin
		  inter0008 <= i5;
          inter0009 <= q5;
          state <= CYCLE11;
          por6 <= 1'b0;   
          inter0007 <= 1'b1; 
		  end
		  CYCLE11:begin
		  inter0008 <= q5;
          inter0009 <= i5;
          state <= CYCLE12;
          por6 <= 1'b0;   
          inter0007 <= 1'b0; 
		 end
		  CYCLE12:begin
		  inter0008 <= i6;
          inter0009 <= q6;
          state <= CYCLE13;
          por6 <= 1'b0;   
          inter0007 <= 1'b1; 
		  end
		  CYCLE13:begin
		   inter0008 <= q6;
          inter0009 <= i6;
          state <= CYCLE14;
          por6 <= 1'b0;   
          inter0007 <= 1'b0; 
		  end
		  CYCLE14:begin
		   inter0008 <= i7;
          inter0009 <= q7;
          state <= CYCLE15;
          por6 <= 1'b0;   
          inter0007 <= 1'b1; 
		 end
		  CYCLE15:begin
		   inter0008 <= q7;
          inter0009 <= i7;
          state <= START;
          por6 <= 1'b0;   
          inter0007 <= 1'b0;    
          inter0007 <= 1'b0;    
		  end
		  default: begin  end
		endcase
	end
end

 
endmodule





				// a <= {1'b1};
				// b <= {1'b1};
				// c <= {1'b1};
				// dataa <= {5'b00001};
				// datab <= {5'b00011};
				// datac <= {5'b00111};