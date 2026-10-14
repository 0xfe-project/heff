//! heff's runtime for the C backend, its part in Zig: what the C half
//! calls out of line, exported by the C ABI, built by the zig that
//! builds the program's C into an object of its own and linked with it.
//! The compiler writes it beside the program's C, as it bundles the C
//! half into the header every part includes, so that a program's C and
//! its runtime come together wherever the C goes.
//!
//! What is done often and is short stays in the C half, inlined into the
//! program's code: counting references, an integer's fast paths, making
//! and reading objects.  What is here is called, and does not inline
//! into the program; it is given C's plain types (pointers, lengths,
//! words), and leaves allocating and counting to the C half.

const std = @import("std");

/// A fault of the runtime's own -- an index out of bounds, an overflow,
/// in a build that checks for them -- said, and the program ended: it is
/// no failure of the program's, which nothing catches.
pub const panic = std.debug.FullPanic(fault);

fn fault(message: []const u8, _: ?usize) noreturn {
    @branchHint(.cold);
    std.debug.print("heff: the runtime is at fault: {s}\n", .{message});
    std.process.abort();
}

// Floats
// ---------------------------------------------------------------------

/// The room a number written as JavaScript writes it takes, its NUL
/// too: at most a sign, "0.000000" and 17 digits.
const numberRoom = 32;

/// A `Float64` as JavaScript writes a number (ECMAScript's
/// Number::toString), at `out`, which has room for 32 bytes, a NUL
/// after it: the fewest digits that read back as the same float -- of
/// those, the nearest -- plain from 1e-7 up to 1e21, else as a digit,
/// the others after a point, and an exponent; "NaN", "Infinity",
/// "-Infinity", and "0", of either zero.
export fn heff_js_number(d: f64, out: [*]u8) void {
    jsNumber(f64, d, out[0..numberRoom]);
}

/// A `Float32` so: the fewest digits that read back as the same
/// `Float32`, written as JavaScript writes a number.
export fn heff_js_number32(f: f32, out: [*]u8) void {
    jsNumber(f32, f, out[0..numberRoom]);
}

fn jsNumber(comptime T: type, x: T, out: *[numberRoom]u8) void {
    var w: std.Io.Writer = .fixed(out[0 .. numberRoom - 1]);
    writeNumber(T, x, &w) catch unreachable;
    out[w.end] = 0;
}

fn writeNumber(comptime T: type, x: T, w: *std.Io.Writer) std.Io.Writer.Error!void {
    if (std.math.isNan(x)) return w.writeAll("NaN");
    if (std.math.isInf(x)) return w.writeAll(if (x < 0) "-Infinity" else "Infinity");
    if (x == 0) return w.writeAll("0");
    const shortest = decimalOf(T, x);
    if (shortest.sign) try w.writeByte('-');
    // (its digits, `k` of them, none of them a last 0, and where the
    // point is among them: `x` is 0.digits times 10 to the `n`)
    var mantissa = shortest.mantissa;
    var exponent = shortest.exponent;
    while (mantissa % 10 == 0) {
        mantissa /= 10;
        exponent += 1;
    }
    var room: [20]u8 = undefined;
    const digits = room[0..std.fmt.printInt(&room, mantissa, 10, .lower, .{})];
    const k: i32 = @intCast(digits.len);
    const n = exponent + k;
    if (k <= n and n <= 21) {
        try w.writeAll(digits);
        try w.splatByteAll('0', @intCast(n - k));
    } else if (0 < n and n <= 21) {
        try w.writeAll(digits[0..@intCast(n)]);
        try w.writeByte('.');
        try w.writeAll(digits[@intCast(n)..]);
    } else if (-6 < n and n <= 0) {
        try w.writeAll("0.");
        try w.splatByteAll('0', @intCast(-n));
        try w.writeAll(digits);
    } else {
        try w.writeByte(digits[0]);
        if (k > 1) {
            try w.writeByte('.');
            try w.writeAll(digits[1..]);
        }
        try w.print("e{c}{d}", .{ @as(u8, if (n - 1 < 0) '-' else '+'), @abs(n - 1) });
    }
}

/// A finite float's shortest decimal, by Ryū (zig's own): the fewest
/// digits that read back as it, of those the nearest.
fn decimalOf(comptime T: type, x: T) std.fmt.float.FloatDecimal(u64) {
    const Bits = @Int(.unsigned, @bitSizeOf(T));
    return std.fmt.float.binaryToDecimal(
        u64,
        @as(Bits, @bitCast(x)),
        std.math.floatMantissaBits(T),
        std.math.floatExponentBits(T),
        false,
        &std.fmt.float.Backend64_TablesFull,
    );
}
