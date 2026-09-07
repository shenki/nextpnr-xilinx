// Run the RTL and the two netlists fasm2bels recovered from the bitstreams
// side by side on the same stimulus and count LED mismatches.
// sw[0]=1 enables pushes, sw[1]=0 keeps the LFSR running, no reset asserted.
module tb (
    input clk,
    output [3:0] led_rtl,
    output [3:0] led_old,
    output [3:0] led_fixed,
    output reg [15:0] old_bad = 0,
    output reg [15:0] fixed_bad = 0,
    output reg [15:0] cycles = 0
);
    top_rtl u_rtl (.clk100(clk), .cpu_reset_n(1'b1), .sw(4'b0001), .led(led_rtl));

    top_old u_old (
        .RIOB33_X57Y125_IOB_X1Y126_IPAD(clk),          // E3  clk100
        .RIOB33_X57Y117_IOB_X1Y118_IPAD(1'b1),         // C2  cpu_reset_n
        .LIOB33_X0Y175_IOB_X0Y175_IPAD(1'b1),          // A8  sw[0]
        .LIOB33_X0Y173_IOB_X0Y174_IPAD(1'b0),          // C11 sw[1]
        .LIOB33_X0Y173_IOB_X0Y173_IPAD(1'b0),          // C10 sw[2]
        .LIOB33_X0Y171_IOB_X0Y172_IPAD(1'b0),          // A10 sw[3]
        .RIOB33_X57Y101_IOB_X1Y101_OPAD(led_old[0]),   // H5  led[0]
        .RIOB33_SING_X57Y100_IOB_X1Y100_OPAD(led_old[1]), // J5 led[1]
        .LIOB33_X0Y51_IOB_X0Y52_OPAD(led_old[2]),      // T9  led[2]
        .LIOB33_X0Y51_IOB_X0Y51_OPAD(led_old[3])       // T10 led[3]
    );

    top_fixed u_fixed (
        .RIOB33_X57Y125_IOB_X1Y126_IPAD(clk),
        .RIOB33_X57Y117_IOB_X1Y118_IPAD(1'b1),
        .LIOB33_X0Y175_IOB_X0Y175_IPAD(1'b1),
        .LIOB33_X0Y173_IOB_X0Y174_IPAD(1'b0),
        .LIOB33_X0Y173_IOB_X0Y173_IPAD(1'b0),
        .LIOB33_X0Y171_IOB_X0Y172_IPAD(1'b0),
        .RIOB33_X57Y101_IOB_X1Y101_OPAD(led_fixed[0]),
        .RIOB33_SING_X57Y100_IOB_X1Y100_OPAD(led_fixed[1]),
        .LIOB33_X0Y51_IOB_X0Y52_OPAD(led_fixed[2]),
        .LIOB33_X0Y51_IOB_X0Y51_OPAD(led_fixed[3])
    );

    always @(posedge clk) begin
        cycles <= cycles + 1;
        if (led_rtl != led_old) old_bad <= old_bad + 1;
        if (led_rtl != led_fixed) fixed_bad <= fixed_bad + 1;
    end
endmodule
