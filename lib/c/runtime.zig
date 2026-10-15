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

// Values, as the C half lays them out
// ---------------------------------------------------------------------

/// A value: a word.  Its low bit set, an integer, 62 bits and a sign;
/// its low bits 10, a tag with no fields; else it points to an object.
const V = usize;

fn isInt(v: V) bool {
    return v & 1 == 1;
}

fn intOf(v: V) i64 {
    return @as(i64, @bitCast(v)) >> 1;
}

/// An object's header: its count, its kind, its tag.
const Obj = extern struct { rc: u32, kind: u16, tag: u16 };

/// An integer beyond a word: its sign, and its `n` digits of 32 bits
/// after it, the lowest first.
const Big = extern struct {
    h: Obj,
    neg: i32,
    n: i32,

    fn digits(b: *const Big) []const u32 {
        const after: [*]const u32 = @ptrCast(@as([*]const Big, @ptrCast(b)) + 1);
        return after[0..@intCast(b.n)];
    }
};

/// The layout above, as the C half checks it: each offset and size.
export const heff_zig_layout = [_]u16{
    @sizeOf(Obj), @offsetOf(Obj, "kind"), @offsetOf(Obj, "tag"),
    @sizeOf(Big), @offsetOf(Big, "neg"),  @offsetOf(Big, "n"),
};

// Hashes
// ---------------------------------------------------------------------
// A string's is FxHash's step (Firefox's and rustc's, `rotl(h, 5) ^ w`
// times its constant) over its bytes 8 at a time, little-endian, the
// last word filled with zeros, from its length; then murmur3's finish
// (`fmix64`), which FxHash lacks, so that the low bits a trie looks at
// first are as mixed as the high.  An integer's is that finish of its
// low 64 bits (two's complement).  The C half shifts each right by 2, a
// word's integer, never negative; JavaScript's and the interpreter's are
// the same numbers, so that a map's order is one everywhere.

const fxK = 0x517cc1b727220a95;

fn fmix64(x: u64) u64 {
    var h = x;
    h ^= h >> 33;
    h *%= 0xff51afd7ed558ccd;
    h ^= h >> 33;
    h *%= 0xc4ceb9fe1a85ec53;
    return h ^ (h >> 33);
}

fn fxStep(h: u64, w: u64) u64 {
    return (std.math.rotl(u64, h, 5) ^ w) *% fxK;
}

/// The hash of `n` bytes at `p`.
export fn heff_hash_bytes(p: [*]const u8, n: i64) u64 {
    const bytes = p[0..@intCast(n)];
    var h: u64 = @bitCast(n);
    var i: usize = 0;
    while (i + 8 <= bytes.len) : (i += 8) h = fxStep(h, std.mem.readInt(u64, bytes[i..][0..8], .little));
    if (i < bytes.len) {
        var last: [8]u8 = @splat(0);
        @memcpy(last[0 .. bytes.len - i], bytes[i..]);
        h = fxStep(h, std.mem.readInt(u64, &last, .little));
    }
    return fmix64(h);
}

/// The hash of an integer, a word's or a big one's.
export fn heff_int_hash(a: V) u64 {
    if (isInt(a)) return fmix64(@bitCast(intOf(a)));
    const big: *const Big = @ptrFromInt(a);
    const d = big.digits();
    const low: u64 = @as(u64, d[0]) | if (d.len > 1) @as(u64, d[1]) << 32 else 0;
    return fmix64(if (big.neg != 0) 0 -% low else low);
}

// UTF-8
// ---------------------------------------------------------------------

const replacement = 0xFFFD;

/// The rune whose bytes start at `p`, `n` bytes there, at least one:
/// how many bytes it takes, the rune at `out`.  What is not UTF-8 is
/// U+FFFD, of as many bytes as start a rune's and go on as one's would,
/// at least one -- the "maximal subpart" WHATWG's decoder takes, as
/// TextDecoder does: after a first byte that starts a rune (C2 to F4),
/// the k-th that does not go on as its (a second byte of E0, ED, F0 and
/// F4 in their own ranges, Unicode's table 3-7) makes one U+FFFD of the
/// k before it; a byte that starts none is one.
export fn heff_u8_decode(p: [*]const u8, n: i64, out: *u32) c_int {
    const first = p[0];
    if (first < 0x80) {
        out.* = first;
        return 1;
    }
    const width: u8, var rune: u32 = switch (first) {
        0xC2...0xDF => .{ 2, first & 0x1F },
        0xE0...0xEF => .{ 3, first & 0x0F },
        0xF0...0xF4 => .{ 4, first & 0x07 },
        else => {
            out.* = replacement;
            return 1;
        },
    };
    const lo: u8 = switch (first) {
        0xE0 => 0xA0,
        0xF0 => 0x90,
        else => 0x80,
    };
    const hi: u8 = switch (first) {
        0xED => 0x9F,
        0xF4 => 0x8F,
        else => 0xBF,
    };
    var k: u8 = 1;
    while (k < width) : (k += 1) {
        if (k >= n) break;
        const next = p[k];
        if (next < (if (k == 1) lo else 0x80) or next > (if (k == 1) hi else 0xBF)) break;
        rune = (rune << 6) | (next & 0x3F);
    } else {
        out.* = rune;
        return width;
    }
    out.* = replacement;
    return k;
}

/// A code point's UTF-8 bytes, at `o`, room for 4: how many.  (A
/// surrogate's are WTF-8's, as C's arithmetic would make them; a rune
/// written is a scalar value, U+FFFD for what is not.)
export fn heff_u8_encode(o: [*]u8, u: u32) c_int {
    return std.unicode.wtf8Encode(@intCast(u), o[0..4]) catch unreachable;
}

/// How many bytes `n` bytes are, made text, what is not UTF-8 in them
/// U+FFFD; as many, where they are UTF-8 (`own.*` 1, else 0).
export fn heff_u8_valid_len(p: [*]const u8, n: i64, own: *c_int) i64 {
    var made: i64 = 0;
    own.* = 1;
    var i: usize = 0;
    while (i < n) {
        var u: u32 = undefined;
        const w = heff_u8_decode(p + i, n - @as(i64, @intCast(i)), &u);
        if (isOwn(p + i, u, w)) {
            made += w;
        } else {
            own.* = 0;
            made += 3;
        }
        i += @intCast(w);
    }
    return made;
}

/// Whether the `w` bytes from `p`, read as `u`, are a rune's: not
/// U+FFFD read for what is not UTF-8.
fn isOwn(p: [*]const u8, u: u32, w: c_int) bool {
    return u != replacement or (w == 3 and p[0] == 0xEF and p[1] == 0xBF and p[2] == 0xBD);
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
