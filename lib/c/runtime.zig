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
