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
