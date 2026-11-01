module test4
(
	clk,
	reset_n,
	ena,
	datain,
	dataout,
	only_5b,
	datain_5bis7,
	data_dp,
	data_dn
)/*synthesis noprune*/;

input		clk;
input		reset_n;
input		ena;
input[4:0]	datain;
output[5:0]	dataout;
output		only_5b;		//when the 6b data is the only one , only_5b = 1
output		datain_5bis7;	//when the 5b data is 7 , datain_5bis7 = 1
output		data_dp;		//when the 5b input is 17 , 18 , 20 , data_dp = 1
output		data_dn;		//when the 5b input is 11 , 13 , 14 , data_dn = 1

reg[5:0]	dataout;
reg			only_5b;
reg			datain_5bis7;
reg			data_dp;
reg			data_dn;

always		@(posedge clk or negedge reset_n)
begin
	if(!reset_n)
	begin
		dataout		<= 6'b000000;
		only_5b		<= 1'b1;
		data_dp		<= 1'b0;
		data_dn		<= 1'b0;
	end
	else
	begin
		if(!ena)
		begin
			{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b000000,1'b0,1'b0,1'b0,1'b0};
		end
		else
		begin
			case(datain)
			5'd0:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b100111,1'b0,1'b0,1'b0,1'b0};
			5'd1:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b011101,1'b0,1'b0,1'b0,1'b0};
			5'd2:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b101101,1'b0,1'b0,1'b0,1'b0};
			5'd3:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b110001,1'b1,1'b0,1'b0,1'b0};
			5'd4:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b110101,1'b0,1'b0,1'b0,1'b0};
			5'd5:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b101001,1'b1,1'b0,1'b0,1'b0};
			5'd6:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b011001,1'b1,1'b0,1'b0,1'b0};
			5'd7:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b111000,1'b0,1'b1,1'b0,1'b0};
			5'd8:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b111001,1'b0,1'b0,1'b0,1'b0};
			5'd9:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b100101,1'b1,1'b0,1'b0,1'b0};
			5'd10:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b010101,1'b1,1'b0,1'b0,1'b0};
			5'd11:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b110100,1'b1,1'b0,1'b0,1'b1};
			5'd12:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b001101,1'b1,1'b0,1'b0,1'b0};
			5'd13:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b101100,1'b1,1'b0,1'b0,1'b1};
			5'd14:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b011100,1'b1,1'b0,1'b0,1'b1};
			5'd15:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b010111,1'b0,1'b0,1'b0,1'b0};
			5'd16:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b011011,1'b0,1'b0,1'b0,1'b0};
			5'd17:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b100011,1'b1,1'b0,1'b1,1'b0};
			5'd18:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b010011,1'b1,1'b0,1'b1,1'b0};
			5'd19:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b110010,1'b1,1'b0,1'b0,1'b0};
			5'd20:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b001011,1'b1,1'b0,1'b1,1'b0};
			5'd21:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b101010,1'b1,1'b0,1'b0,1'b0};
			5'd22:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b011010,1'b1,1'b0,1'b0,1'b0};
			5'd23:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b111010,1'b0,1'b0,1'b0,1'b0};
			5'd24:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b110011,1'b0,1'b0,1'b0,1'b0};
			5'd25:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b100110,1'b1,1'b0,1'b0,1'b0};
			5'd26:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b010110,1'b1,1'b0,1'b0,1'b0};
			5'd27:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b110110,1'b0,1'b0,1'b0,1'b0};
			5'd28:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b001110,1'b1,1'b0,1'b0,1'b0};
			5'd29:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b101110,1'b0,1'b0,1'b0,1'b0};
			5'd30:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b011110,1'b0,1'b0,1'b0,1'b0};
			5'd31:	{dataout,only_5b,datain_5bis7,data_dp,data_dn} <= {6'b101011,1'b0,1'b0,1'b0,1'b0};
			endcase		
		end
	end
end

endmodule