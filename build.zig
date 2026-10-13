//! heff's build, by zig alone: scripts/bootstrap.zig, built for this
//! machine, runs each stage -- from the heff built before, or from the
//! seed, boot/heff.tar.xz -- and zig builds the C of each, a part an
//! object, kept by what it is made of in .heff/out/heff.o/.  (zig's own
//! cache, .zig-cache/, keeps bootstrap.)  The Makefile's build, boot,
//! native and release are these steps.
//!
//!   zig build                      .heff/out/heff
//!   zig build seed                 .heff/out/heff, from the seed, always
//!   zig build fixpoint             heff compiles itself to the same C
//!   zig build boot                 boot/heff.tar.xz renewed, once it does
//!   zig build native -Dtargets="x86_64-linux-musl aarch64-macos"
//!   zig build release              .heff/out/release/, an archive a platform

const std = @import("std");

pub fn build(b: *std.Build) void {
    const bootstrap = b.addExecutable(.{
        .name = "bootstrap",
        .root_module = b.createModule(.{
            .root_source_file = b.path("scripts/bootstrap.zig"),
            .target = b.graph.host,
            .optimize = .Debug,
        }),
    });
    const targets = b.option([]const u8, "targets", "native: zig targets, e.g. \"x86_64-linux-musl aarch64-macos\"") orelse "";

    const heff = command(b, bootstrap, &.{"build"});
    b.getInstallStep().dependOn(heff);
    b.step("heff", "build .heff/out/heff, from the heff built before (from the seed, the first time)").dependOn(heff);
    b.step("seed", "build .heff/out/heff from the seed, boot/heff.tar.xz, always").dependOn(command(b, bootstrap, &.{"seed"}));
    b.step("fixpoint", "check that heff compiles itself to the same C").dependOn(command(b, bootstrap, &.{"fixpoint"}));

    // (each once heff is built: the seed renewed from its C, heff for
    // other systems, the release)
    const boot = command(b, bootstrap, &.{"boot"});
    boot.dependOn(command(b, bootstrap, &.{"build"}));
    b.step("boot", "renew the seed, boot/heff.tar.xz, once heff compiles itself to itself").dependOn(boot);

    var each = std.mem.tokenizeScalar(u8, targets, ' ');
    var names: std.ArrayList([]const u8) = .empty;
    names.append(b.allocator, "native") catch @panic("OOM");
    while (each.next()) |target| names.append(b.allocator, target) catch @panic("OOM");
    const native = command(b, bootstrap, names.items);
    native.dependOn(command(b, bootstrap, &.{"build"}));
    b.step("native", "heff for other systems, .heff/out/native/TARGET/ (-Dtargets=...)").dependOn(native);

    const release = command(b, bootstrap, &.{"release"});
    release.dependOn(command(b, bootstrap, &.{"build"}));
    b.step("release", "the release archives, heff for each platform, .heff/out/release/").dependOn(release);
}

/// bootstrap run at the repository's root, with the zig that runs the
/// build: each time, as it says what it does, and knows itself what is
/// up to date.
fn command(b: *std.Build, bootstrap: *std.Build.Step.Compile, arguments: []const []const u8) *std.Build.Step {
    const run = b.addRunArtifact(bootstrap);
    run.addArg(b.graph.zig_exe);
    run.addArgs(arguments);
    run.setCwd(b.path("."));
    run.stdio = .inherit;
    run.has_side_effects = true;
    return &run.step;
}
