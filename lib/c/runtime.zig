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
