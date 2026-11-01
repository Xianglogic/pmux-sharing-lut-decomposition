
module test2 (clk, a,b,c,d,dataa,datab,datac,datad,out) ;

input a;
input b;
input c;
input d;
input clk;
input [4 : 0]dataa;
input [4 : 0]datab;
input [4 : 0]datac;
input [4 : 0]datad;
output[4:0] out;
reg[4:0]temp;

always@(a or dataa or b or datab or c or datac)

  begin
    case (1'b1) 
      a:
	  temp = dataa;
      b:
	  temp = datab;
      c:
	  temp = datac;
	  d:
	  temp = datad;
      default: temp =0 ;
    endcase
  end
  assign out = temp;
 
endmodule





				// a <= {1'b1};
				// b <= {1'b1};
				// c <= {1'b1};
				// dataa <= {5'b00001};
				// datab <= {5'b00011};
				// datac <= {5'b00111};