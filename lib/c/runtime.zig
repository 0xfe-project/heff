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
    said("heff: the runtime is at fault: ");
    said(message);
    said("\n");
    std.process.abort();
}

// (the C library's, linked with every program: what the C half has)
extern fn write(fd: c_int, bytes: [*]const u8, n: usize) isize;

// (written to the standard error, as it is -- by the C library, not
// std.debug.print, which brings Zig's formatting and its locked stderr
// into every program's build, a second of zig's at each)
fn said(text: []const u8) void {
    _ = write(2, text.ptr, text.len);
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

// Integers beyond a word: their magnitudes
// ---------------------------------------------------------------------
// A magnitude is its 32-bit digits, the least first, as `Big` holds
// them; the C half gives each result room, and makes an integer of it.

/// `a` against `b`, as magnitudes of `an` and `bn` digits, no top digit
/// 0: -1, 0 or 1.
export fn heff_mag_cmp(a: [*]const u32, an: c_int, b: [*]const u32, bn: c_int) c_int {
    if (an != bn) return if (an < bn) -1 else 1;
    var i: usize = @intCast(an);
    while (i > 0) {
        i -= 1;
        if (a[i] != b[i]) return if (a[i] < b[i]) -1 else 1;
    }
    return 0;
}

/// `a` plus `b`, into `o`, which has room for max(an, bn) + 1 digits:
/// how many digits it has.
export fn heff_mag_add(a: [*]const u32, an: c_int, b: [*]const u32, bn: c_int, o: [*]u32) c_int {
    if (an < bn) return heff_mag_add(b, bn, a, an, o);
    const x = a[0..@intCast(an)];
    const y = b[0..@intCast(bn)];
    var carry: u64 = 0;
    for (x, 0..) |d, i| {
        carry += @as(u64, d) + if (i < y.len) y[i] else 0;
        o[i] = @truncate(carry);
        carry >>= 32;
    }
    o[x.len] = @truncate(carry);
    return an + 1;
}

/// `a` minus `b`, `a` not below `b`, into `o`, which has room for `an`
/// digits: how many digits it has, `an`.
export fn heff_mag_sub(a: [*]const u32, an: c_int, b: [*]const u32, bn: c_int, o: [*]u32) c_int {
    const x = a[0..@intCast(an)];
    const y = b[0..@intCast(bn)];
    var borrow: i64 = 0;
    for (x, 0..) |d, i| {
        borrow += @as(i64, d) - if (i < y.len) y[i] else 0;
        o[i] = @truncate(@as(u64, @bitCast(borrow)));
        borrow = if (borrow < 0) -1 else 0;
    }
    return an;
}

/// `a` times `b`, into `o`, which has room for an + bn digits, all of
/// them written.
export fn heff_mag_mul(a: [*]const u32, an: c_int, b: [*]const u32, bn: c_int, o: [*]u32) void {
    const x = a[0..@intCast(an)];
    const y = b[0..@intCast(bn)];
    @memset(o[0 .. x.len + y.len], 0);
    for (x, 0..) |d, i| {
        var carry: u64 = 0;
        for (y, 0..) |e, j| {
            carry += @as(u64, d) * e + o[i + j];
            o[i + j] = @truncate(carry);
            carry >>= 32;
        }
        o[i + y.len] = @truncate(carry);
    }
}

/// `u`, `m` digits, divided by `v`, `n` digits whose top is not 0, as
/// Knuth's algorithm D: the quotient `q` gets max(m - n + 1, 1) digits,
/// the remainder `r` gets `n`; `vn` and `un` are room for `n` and `m +
/// 1` digits, the normalised divisor and dividend.
export fn heff_mag_divmod(u: [*]const u32, m: c_int, v: [*]const u32, n: c_int, q: [*]u32, r: [*]u32, vn: [*]u32, un: [*]u32) void {
    const mu: usize = @intCast(m);
    const nv: usize = @intCast(n);
    if (mu < nv) {
        @memcpy(r[0..mu], u[0..mu]);
        @memset(r[mu..nv], 0);
        return;
    }
    if (nv == 1) {
        var k: u64 = 0;
        var j = mu;
        while (j > 0) {
            j -= 1;
            const t = (k << 32) | u[j];
            q[j] = @truncate(t / v[0]);
            k = t - @as(u64, q[j]) * v[0];
        }
        r[0] = @truncate(k);
        return;
    }
    // (the divisor's top digit made to have its top bit set, and the
    // dividend shifted as much, so that each guess of a quotient digit
    // is at most 2 too big)
    const s: u5 = @intCast(@clz(v[nv - 1]));
    const back: u5 = @truncate(32 - @as(u6, s));
    var i = nv - 1;
    while (i > 0) : (i -= 1) vn[i] = (v[i] << s) | (if (s != 0) v[i - 1] >> back else 0);
    vn[0] = v[0] << s;
    un[mu] = if (s != 0) u[mu - 1] >> back else 0;
    i = mu - 1;
    while (i > 0) : (i -= 1) un[i] = (u[i] << s) | (if (s != 0) u[i - 1] >> back else 0);
    un[0] = u[0] << s;
    var j = mu - nv + 1;
    while (j > 0) {
        j -= 1;
        const num = (@as(u64, un[j + nv]) << 32) | un[j + nv - 1];
        var qhat = num / vn[nv - 1];
        var rhat = num - qhat * vn[nv - 1];
        while (qhat >> 32 != 0 or qhat * vn[nv - 2] > ((rhat << 32) | un[j + nv - 2])) {
            qhat -= 1;
            rhat += vn[nv - 1];
            if (rhat >> 32 != 0) break;
        }
        var k: i64 = 0;
        var t: i64 = 0;
        for (0..nv) |l| {
            const p = qhat * vn[l];
            t = @as(i64, un[l + j]) - k - @as(i64, @intCast(p & 0xFFFFFFFF));
            un[l + j] = @truncate(@as(u64, @bitCast(t)));
            k = @as(i64, @intCast(p >> 32)) - (t >> 32);
        }
        t = @as(i64, un[j + nv]) - k;
        un[j + nv] = @truncate(@as(u64, @bitCast(t)));
        q[j] = @truncate(qhat);
        if (t < 0) {
            // (the guess was one too big: a digit less, and the divisor
            // added back)
            q[j] -%= 1;
            k = 0;
            for (0..nv) |l| {
                t = @as(i64, un[l + j]) + vn[l] + k;
                un[l + j] = @truncate(@as(u64, @bitCast(t)));
                k = t >> 32;
            }
            un[j + nv] +%= @truncate(@as(u64, @bitCast(k)));
        }
    }
    for (0..nv) |l| r[l] = (un[l] >> s) | (if (s != 0) un[l + 1] << back else 0);
}

// Bits
// ---------------------------------------------------------------------
// An integer of `n` bits, 0 to 64, as Erlang's bit syntax has it: its
// two's complement, the most significant bit first -- or, little-endian,
// its bytes the least significant first and then the `n % 8` bits left,
// the most significant.  `heff_bits_read` and `heff_bits_write` have
// them in the order they are written; `heff_bits_little` turns the one
// into the other.

/// The `n` bits at bit `at` of `s`, the most significant first.
export fn heff_bits_read(s: [*]const u8, at: i64, n: i64) u64 {
    var u: u64 = 0;
    const from: usize = @intCast(at);
    const count: usize = @intCast(n);
    if (from & 7 == 0 and count & 7 == 0) {
        for (s[from >> 3 ..][0 .. count >> 3]) |byte| u = (u << 8) | byte;
    } else {
        for (from..from + count) |i| u = (u << 1) | ((s[i >> 3] >> @intCast(7 - (i & 7))) & 1);
    }
    return u;
}

/// The `n` bits of `w`, the most significant first, put at bit `at` of
/// `s`, whose bits there are 0.
export fn heff_bits_write(s: [*]u8, at: i64, w: u64, n: i64) void {
    const from: usize = @intCast(at);
    const count: usize = @intCast(n);
    if (from & 7 == 0 and count & 7 == 0) {
        for (s[from >> 3 ..][0 .. count >> 3], 0..) |*byte, i| byte.* = @truncate(w >> @intCast(count - 8 * (i + 1)));
    } else {
        for (0..count) |i| {
            if ((w >> @intCast(count - 1 - i)) & 1 != 0) s[(from + i) >> 3] |= @as(u8, 0x80) >> @intCast((from + i) & 7);
        }
    }
}

/// An integer of `n` bits, little-endian: its bits as they are written,
/// from the integer (`out` not 0), or the integer, from them.
export fn heff_bits_little(u: u64, n: i64, out: c_int) u64 {
    const count: usize = @intCast(n);
    const k = count >> 3;
    const r = count & 7;
    var w: u64 = 0;
    if (out != 0) {
        for (0..k) |i| w = (w << 8) | ((u >> @intCast(8 * i)) & 255);
        if (r != 0) w = (w << @intCast(r)) | ((u >> @intCast(8 * k)) & ((@as(u64, 1) << @intCast(r)) - 1));
    } else {
        if (r != 0) w = (u & ((@as(u64, 1) << @intCast(r)) - 1)) << @intCast(8 * k);
        for (0..k) |i| w |= ((u >> @intCast(count - 8 * (i + 1))) & 255) << @intCast(8 * i);
    }
    return w;
}

// A path under a place's working directory (#97)
// ---------------------------------------------------------------------
//
// The system has one working directory a process, and a place is one
// thread of its process; so the C half keeps a directory for each
// place, and gives the system each relative path under it, joined by
// `heff_path_under`: the join `std.path` does, less its tidying of `.`
// and `..`, which the system does against the directories as they are,
// links and all.

fn isSep(c: u8, windows: bool) bool {
    return c == '/' or (windows and c == '\\');
}

/// How much of `p` is its drive, in the Windows style: `C:`, or
/// `//server/share` with either slash; 0 where it has none.
fn driveLen(p: []const u8) usize {
    if (p.len >= 2 and p[1] == ':' and std.ascii.isAlphabetic(p[0])) return 2;
    if (p.len >= 3 and isSep(p[0], true) and isSep(p[1], true) and !isSep(p[2], true)) {
        var i: usize = 2;
        while (i < p.len and !isSep(p[i], true)) : (i += 1) {}
        if (i == p.len) return 0;
        i += 1;
        while (i < p.len and !isSep(p[i], true)) : (i += 1) {}
        return i;
    }
    return 0;
}

/// `path` as the system is to be given it from the directory `dir`,
/// written to `out` (room for both and a separator): how much of it.
/// `path` as it is where it is absolute, where it is empty (an error
/// the system's) or, in the Windows style, where it names a drive --
/// `C:x` too, which the system takes from that drive's own directory;
/// `dir`'s drive before it where it starts at a root; else `dir`, a
/// separator and `path`.
export fn heff_path_under(dir: [*]const u8, dn: usize, path: [*]const u8, pn: usize, windows: c_int, out: [*]u8) usize {
    const d = dir[0..dn];
    const p = path[0..pn];
    const win = windows != 0;
    if (p.len == 0 or d.len == 0 or (win and driveLen(p) > 0) or (!win and p[0] == '/')) {
        @memcpy(out[0..p.len], p);
        return p.len;
    }
    if (win and isSep(p[0], true)) {
        const k = driveLen(d);
        @memcpy(out[0..k], d[0..k]);
        @memcpy(out[k..][0..p.len], p);
        return k + p.len;
    }
    @memcpy(out[0..d.len], d);
    var n = d.len;
    if (!isSep(d[d.len - 1], win)) {
        out[n] = if (win) '\\' else '/';
        n += 1;
    }
    @memcpy(out[n..][0..p.len], p);
    return n + p.len;
}
