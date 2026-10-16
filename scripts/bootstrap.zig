//! The compiler, bootstrapped, as build.zig runs it: a compiler there is
//! (stage 0) compiles app/heff/main.hf to a first stage, and the first
//! stage compiles the same source to C, .heff/out/heff.d/ (a file a
//! module, and the runtime's Zig) -- made by the backend of the source,
//! whatever stage 0's was -- which zig builds into .heff/out/heff, `heff`.  Stage 0 is the heff
//! built before (.heff/out/heff), as Go builds with a Go there is, if it
//! comes of the seed there is: built from it, or with a heff that came of
//! it (.heff/out/heff.seed, the seed's hash).  A seed is renewed when the
//! source wants what an older heff has not, so a heff of another seed, or
//! of one not known, is not built with.  Where there is no heff of the
//! seed, or it cannot make the first stage, the seed: boot/heff.tar.xz,
//! heff compiled by itself to C, unpacked to .heff/out/heff-seed.d/ and
//! built by zig for this machine.  zig builds each part apart, and the
//! objects are kept by what they are made of, in .heff/out/heff.o/.
//!
//!   bootstrap ZIG build            the compiler, from the heff there is
//!   bootstrap ZIG seed             the compiler, from the seed, always
//!   bootstrap ZIG fixpoint         heff compiles itself to the same C
//!   bootstrap ZIG boot             the seed renewed, once it does
//!   bootstrap ZIG native TARGET... heff for other systems
//!   bootstrap ZIG release          the release archives, .heff/out/release/
//!
//! (ZIG, the zig that runs the build, builds the C too.)

const std = @import("std");
const builtin = @import("builtin");
const Io = std.Io;
const Dir = Io.Dir;
const Allocator = std.mem.Allocator;

const exe = if (builtin.os.tag == .windows) ".exe" else "";
const out = ".heff/out";
const seed = "boot/heff.tar.xz";
const heff = out ++ "/heff" ++ exe;
const parts = out ++ "/heff.d";
const kept = out ++ "/heff.o";
const nativeKept = out ++ "/native.o";
const seedOf = out ++ "/heff.seed";
// (zig's own cache, of the parts it builds for this checkout: `zig cc`'s
// ZIG_LOCAL_CACHE_DIR, begun afresh each run)
const zigCache = ".heff/zig-cache";

// (for any machine of this kind, not only this one: zig's default is this
// machine's processor, whose instructions another may not have)
const flags = [_][]const u8{ "-mcpu=baseline", "-O2", "-w" } ++ noUnwind[0..unwound(builtin.os.tag == .windows)].*;

/// No unwind tables (#69): heff's C throws nothing through them -- a
/// failure is a longjmp, which needs none -- and they were 1.4 MB of
/// heff's 17.5; Windows's are kept, its longjmp unwinding by them.
const noUnwind = [_][]const u8{ "-fno-asynchronous-unwind-tables", "-fno-unwind-tables" };
fn unwound(windows: bool) usize {
    return if (windows) 0 else noUnwind.len;
}
fn unwind(windows: bool) []const []const u8 {
    return noUnwind[0..unwound(windows)];
}

/// The flags C is built with, as zig builds the runtime's Zig with them:
/// the same target and processor, the mode of the level (`-O2`
/// ReleaseFast), and no unwind tables where C has none.
fn zigFlags(arena: Allocator, compile: []const []const u8) ![]const []const u8 {
    var flagged: std.ArrayList([]const u8) = .empty;
    var unwinds = true;
    var k: usize = 0;
    while (k < compile.len) : (k += 1) {
        const flag = compile[k];
        if (std.mem.eql(u8, flag, "-target") and k + 1 < compile.len) {
            try flagged.appendSlice(arena, &.{ flag, compile[k + 1] });
            k += 1;
        } else if (std.mem.startsWith(u8, flag, "-mcpu=")) {
            try flagged.append(arena, flag);
        } else if (modeOf(flag)) |mode| {
            try flagged.appendSlice(arena, &.{ "-O", mode });
        } else if (std.mem.eql(u8, flag, "-fno-unwind-tables")) {
            unwinds = false;
        }
    }
    if (!unwinds) try flagged.append(arena, "-fno-unwind-tables");
    return flagged.items;
}

/// zig's mode for a C compiler's level, as heff builds the runtime's Zig.
fn modeOf(level: []const u8) ?[]const u8 {
    const modes = [_]struct { []const u8, []const u8 }{
        .{ "-O0", "Debug" },       .{ "-O1", "ReleaseSafe" },  .{ "-O2", "ReleaseFast" },
        .{ "-O3", "ReleaseFast" }, .{ "-Os", "ReleaseSmall" },
    };
    for (modes) |m| if (std.mem.eql(u8, level, m[0])) return m[1];
    return null;
}

// (what the sources of the compiler are: a heff newer than every one of
// them, and than the seed, is up to date)
const sourceDirectories = [_][]const u8{ "app", "src", "lib" };
const sourceFiles = [_][]const u8{ seed, "build.zig", "scripts/bootstrap.zig" };

const Bootstrap = struct {
    arena: Allocator,
    io: Io,
    zig: []const u8,
    /// zig's version and the machine it builds for, as `zig env` says
    /// them: what an object is made by.
    zigIs: []const u8,
    /// The objects, by name, the parts of this build are.
    used: *std.StringHashMapUnmanaged(void),
    environment: *std.process.Environ.Map,
    progress: std.Progress.Node,
    root: Dir,

    fn say(b: *const Bootstrap, comptime format: []const u8, arguments: anytype) void {
        const text = std.fmt.allocPrint(b.arena, format ++ "\n", arguments) catch return;
        Io.File.stdout().writeStreamingAll(b.io, text) catch {};
    }

    fn complain(b: *const Bootstrap, comptime format: []const u8, arguments: anytype) void {
        const text = std.fmt.allocPrint(b.arena, format ++ "\n", arguments) catch return;
        Io.File.stderr().writeStreamingAll(b.io, text) catch {};
    }

    fn fail(b: *const Bootstrap, comptime format: []const u8, arguments: anytype) noreturn {
        b.complain(format, arguments);
        std.process.exit(1);
    }

    fn seconds(b: *const Bootstrap, since: Io.Timestamp) f64 {
        const took = since.durationTo(Io.Timestamp.now(b.io, .awake));
        return @as(f64, @floatFromInt(took.toMilliseconds())) / 1000;
    }

    fn concat(b: *const Bootstrap, pieces: []const []const u8) []const u8 {
        return std.mem.concat(b.arena, u8, pieces) catch b.fail("heff build: out of memory", .{});
    }

    /// A program run, what it says on its standard output let go; whether
    /// it ended well.
    fn run(b: *const Bootstrap, argv: []const []const u8, node: std.Progress.Node) bool {
        var child = std.process.spawn(b.io, .{
            .argv = argv,
            .environ_map = b.environment,
            .stdin = .ignore,
            .stdout = .ignore,
            .progress_node = node,
        }) catch |err| {
            if (err == error.FileNotFound and argv[0].ptr == b.zig.ptr)
                b.fail("heff build: no zig at {s} (https://ziglang.org/download/)", .{b.zig});
            b.complain("heff build: {s} could not be run: {t}", .{ argv[0], err });
            return false;
        };
        const term = child.wait(b.io) catch return false;
        return term.success();
    }

    fn modified(b: *const Bootstrap, path: []const u8) ?i96 {
        const stat = b.root.statFile(b.io, path, .{}) catch return null;
        return stat.mtime.nanoseconds;
    }

    fn read(b: *const Bootstrap, path: []const u8) ?[]u8 {
        return b.root.readFileAlloc(b.io, path, b.arena, .unlimited) catch null;
    }

    /// The newest of the compiler's sources, by when each was changed.
    fn newestSource(b: *const Bootstrap) i96 {
        var newest: i96 = 0;
        for (sourceFiles) |path| newest = @max(newest, b.modified(path) orelse 0);
        for (sourceDirectories) |path| {
            var directory = b.root.openDir(b.io, path, .{ .iterate = true }) catch continue;
            defer directory.close(b.io);
            var walker = directory.walk(b.arena) catch continue;
            defer walker.deinit();
            while (walker.next(b.io) catch null) |entry| {
                if (entry.kind != .file or !isSource(entry.basename)) continue;
                const stat = entry.dir.statFile(b.io, entry.basename, .{}) catch continue;
                newest = @max(newest, stat.mtime.nanoseconds);
            }
        }
        return newest;
    }

    fn isSource(name: []const u8) bool {
        return std.mem.endsWith(u8, name, ".hf") or std.mem.endsWith(u8, name, ".js") or
            std.mem.endsWith(u8, name, ".c") or std.mem.endsWith(u8, name, ".zig");
    }

    /// The files of a directory whose names end so, sorted.
    fn filesOf(b: *const Bootstrap, path: []const u8, endings: []const []const u8) ?[][]const u8 {
        var directory = b.root.openDir(b.io, path, .{ .iterate = true }) catch return null;
        defer directory.close(b.io);
        var names: std.ArrayList([]const u8) = .empty;
        var iterator = directory.iterate();
        while (iterator.next(b.io) catch return null) |entry| {
            if (entry.kind != .file or entry.name[0] == '.') continue;
            for (endings) |ending| if (std.mem.endsWith(u8, entry.name, ending)) {
                names.append(b.arena, b.arena.dupe(u8, entry.name) catch return null) catch return null;
                break;
            };
        }
        std.mem.sort([]const u8, names.items, {}, lessThan);
        return names.items;
    }

    fn lessThan(_: void, x: []const u8, y: []const u8) bool {
        return std.mem.lessThan(u8, x, y);
    }

    /// heff's source, compiled by `compiler` to C as parts in `directory`.
    fn toC(b: *const Bootstrap, compiler: []const u8, directory: []const u8) bool {
        const name = std.fs.path.basename(compiler);
        const node = b.progress.start(b.concat(&.{ name, " compiles the source to C" }), 0);
        defer node.end();
        const start = Io.Timestamp.now(b.io, .awake);
        const argv = [_][]const u8{ compiler, "compile", "app/heff/main.hf", b.concat(&.{ directory, "/" }), "--name", "main", "--backend=c" };
        if (!b.run(&argv, node)) return false;
        b.say("heff build: {s} compiled the source to C in {d:.1} s", .{ name, b.seconds(start) });
        return true;
    }

    /// The parts in `directory` built by zig into `to`: each part an
    /// object -- a C file's by `zig cc`, the runtime's Zig by `zig
    /// build-obj` -- kept in `into` and named by what it is made of (the
    /// zig and the machine it builds for, the flags, and the part; a C
    /// part's heff.h too), so that a part as it was is not built again,
    /// wherever it is and whichever stage it is of; as many at once as
    /// there are cores, then the objects linked.  (zig's own cache knows
    /// a C file by its path, time and inode, not by what is in it, so it
    /// builds again a part written again, as every part is in a CI's
    /// checkout or its cache restored.)
    fn cc(b: *const Bootstrap, who: []const u8, into: []const u8, directory: []const u8, to: []const u8, compile: []const []const u8, link: []const []const u8) bool {
        const files = b.filesOf(directory, &.{".c"}) orelse return false;
        const zigs = b.filesOf(directory, &.{".zig"}) orelse return false;
        const zigCompile = zigFlags(b.arena, compile) catch return false;
        b.root.createDirPath(b.io, into) catch return false;
        var madeOf = std.crypto.hash.sha2.Sha256.init(.{});
        madeOf.update(b.zigIs);
        madeOf.update(std.mem.join(b.arena, " ", compile) catch return false);
        madeOf.update(b.read(b.concat(&.{ directory, "/heff.h" })) orelse "");
        var zigMadeOf = std.crypto.hash.sha2.Sha256.init(.{});
        zigMadeOf.update(b.zigIs);
        zigMadeOf.update(std.mem.join(b.arena, " ", zigCompile) catch return false);
        const objects = b.arena.alloc([]const u8, files.len + zigs.len) catch return false;
        var anew: std.ArrayList(Job) = .empty;
        for (files, 0..) |file, k| {
            const object = b.objectOf(into, madeOf, directory, file, file[0 .. file.len - 2]) orelse return false;
            objects[k] = object;
            if (b.modified(object) != null) continue;
            const argv = std.mem.concat(b.arena, []const u8, &.{
                &.{ b.zig, "cc" },
                compile,
                &.{ "-c", b.concat(&.{ directory, "/", file }), "-o", b.concat(&.{ object, ".part" }) },
            }) catch return false;
            anew.append(b.arena, .{ .argv = argv, .object = object }) catch return false;
        }
        for (zigs, files.len..) |file, k| {
            const object = b.objectOf(into, zigMadeOf, directory, file, file) orelse return false;
            objects[k] = object;
            if (b.modified(object) != null) continue;
            const argv = std.mem.concat(b.arena, []const u8, &.{
                &.{ b.zig, "build-obj", b.concat(&.{ directory, "/", file }) },
                zigCompile,
                &.{b.concat(&.{ "-femit-bin=", object, ".part" })},
            }) catch return false;
            anew.append(b.arena, .{ .argv = argv, .object = object }) catch return false;
        }

        const name = if (std.mem.eql(u8, who, "heff build")) std.fs.path.basename(to) else to;
        const node = b.progress.start(b.concat(&.{ "zig builds ", name }), anew.items.len);
        defer node.end();
        const start = Io.Timestamp.now(b.io, .awake);
        if (!b.compileParts(anew.items, node)) return false;
        var argv: std.ArrayList([]const u8) = .empty;
        argv.appendSlice(b.arena, &.{ b.zig, "cc" }) catch return false;
        argv.appendSlice(b.arena, link) catch return false;
        argv.appendSlice(b.arena, objects) catch return false;
        argv.appendSlice(b.arena, &.{ "-o", to, "-lm" }) catch return false;
        if (!b.run(argv.items, node)) return false;
        b.say("{s}: zig built {s} ({d} of its {d} parts anew) in {d:.1} s", .{ who, name, anew.items.len, objects.len, b.seconds(start) });
        return true;
    }

    /// The object a part of `directory` is kept as, in `into`: named
    /// `stem`, then by the hash of what all of its kind are made of,
    /// `madeOf`, and the part; and known as one of this build's.
    fn objectOf(b: *const Bootstrap, into: []const u8, madeOf: std.crypto.hash.sha2.Sha256, directory: []const u8, file: []const u8, stem: []const u8) ?[]const u8 {
        const bytes = b.read(b.concat(&.{ directory, "/", file })) orelse return null;
        var part = madeOf;
        part.update(bytes);
        var digest: [32]u8 = undefined;
        part.final(&digest);
        const hex = std.fmt.bytesToHex(digest, .lower);
        const name = b.concat(&.{ stem, "-", hex[0..16], ".o" });
        b.used.put(b.arena, name, {}) catch return null;
        return b.concat(&.{ into, "/", name });
    }

    /// A part to build: the command that builds it to OBJECT.part, and
    /// the object.
    const Job = struct { argv: []const []const u8, object: []const u8 };

    /// The parts to build, each to its object.
    const Work = struct {
        b: *const Bootstrap,
        jobs: []const Job,
        node: std.Progress.Node,
        next: std.atomic.Value(usize) = .init(0),
        failed: std.atomic.Value(bool) = .init(false),

        /// One worker: the next part not taken, while there is one and
        /// none failed, built to OBJECT.part, renamed once it is whole,
        /// so that an object there is is never half made.
        fn work(w: *Work) void {
            const b = w.b;
            while (!w.failed.load(.monotonic)) {
                const at = w.next.fetchAdd(1, .monotonic);
                if (at >= w.jobs.len) return;
                const job = w.jobs[at];
                if (!b.run(job.argv, .none)) return w.failed.store(true, .monotonic);
                Dir.rename(b.root, b.concat(&.{ job.object, ".part" }), b.root, job.object, b.io) catch
                    return w.failed.store(true, .monotonic);
                w.node.completeOne();
            }
        }
    };

    /// The parts built each to its object, as many at once as there are
    /// cores.
    fn compileParts(b: *const Bootstrap, jobs: []const Job, node: std.Progress.Node) bool {
        var w: Work = .{ .b = b, .jobs = jobs, .node = node };
        const count = @min(std.Thread.getCpuCount() catch 4, jobs.len);
        const workers = b.arena.alloc(std.Thread, count) catch return false;
        var started: usize = 0;
        for (workers) |*worker| {
            worker.* = std.Thread.spawn(.{}, Work.work, .{&w}) catch break;
            started += 1;
        }
        if (started == 0 and count > 0) w.work();
        for (workers[0..started]) |worker| worker.join();
        return !w.failed.load(.monotonic);
    }

    /// The objects in `into` no part of this build is made of, let go.
    fn prune(b: *const Bootstrap, into: []const u8) void {
        var directory = b.root.openDir(b.io, into, .{ .iterate = true }) catch return;
        defer directory.close(b.io);
        var gone: std.ArrayList([]const u8) = .empty;
        var iterator = directory.iterate();
        while (iterator.next(b.io) catch return) |entry|
            if (!b.used.contains(entry.name)) gone.append(b.arena, b.arena.dupe(u8, entry.name) catch return) catch return;
        for (gone.items) |name| directory.deleteFile(b.io, name) catch {};
    }

    /// The seed, unpacked into `into`, anew.
    fn unpack(b: *const Bootstrap, into: []const u8) !void {
        const compressed = b.read(seed) orelse return error.NoSeed;
        var input: Io.Reader = .fixed(compressed);
        var xz = try std.compress.xz.Decompress.init(&input, b.arena, &.{});
        try b.root.deleteTree(b.io, into);
        try b.root.createDirPath(b.io, into);
        var directory = try b.root.openDir(b.io, into, .{});
        defer directory.close(b.io);
        try std.tar.extract(b.io, directory, &xz.reader, .{});
    }

    fn seedHash(b: *const Bootstrap) [64]u8 {
        const bytes = b.read(seed) orelse b.fail("heff build: no {s}", .{seed});
        var digest: [32]u8 = undefined;
        std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
        return std.fmt.bytesToHex(digest, .lower);
    }

    fn build(b: *const Bootstrap, fromSeed: bool) void {
        const hash = b.seedHash();
        const heffSeed = std.mem.trim(u8, b.read(seedOf) orelse "", " \r\n");
        const ofThisSeed = std.mem.eql(u8, heffSeed, &hash);
        const built = b.modified(heff);
        if (!fromSeed and built != null and ofThisSeed and b.newestSource() <= built.?) {
            b.say("heff build: {s} is up to date", .{heff});
            return;
        }

        b.root.createDirPath(b.io, out) catch |err| b.fail("heff build: {s}: {t}", .{ out, err });
        const stage0 = out ++ "/heff-stage0" ++ exe;
        const stage1 = out ++ "/heff-stage1" ++ exe;
        const stage1C = out ++ "/heff-stage1.d";
        var first = false;
        if (!fromSeed and built != null) {
            if (!ofThisSeed) {
                b.say("heff build: the heff there is comes of {s}; from the seed", .{if (heffSeed.len > 0) "another seed" else "a seed not known"});
            } else if (Dir.copyFile(b.root, heff, b.root, stage0, b.io, .{})) |_| {
                // (the heff there is, set apart: the build writes over it)
                if (!b.toC(stage0, stage1C))
                    b.complain("heff build: the heff there is cannot compile the source; from the seed", .{})
                else if (!b.cc("heff build", kept, stage1C, stage1, &flags, &flags))
                    b.complain("heff build: the heff there is compiles the source to C zig cannot build; from the seed", .{})
                else
                    first = true;
            } else |err| b.complain("heff build: {s} could not be copied ({t}); from the seed", .{ heff, err });
        }
        if (!first) {
            const seedC = out ++ "/heff-seed.d";
            const seedExe = out ++ "/heff-seed" ++ exe;
            const start = Io.Timestamp.now(b.io, .awake);
            b.unpack(seedC) catch |err| b.fail("heff build: unpacking the seed failed: {t}", .{err});
            b.say("heff build: the seed unpacked in {d:.1} s", .{b.seconds(start)});
            if (!b.cc("heff build", kept, seedC, seedExe, &flags, &flags)) b.fail("heff build: zig cc failed", .{});
            if (!b.toC(seedExe, stage1C)) b.fail("heff build: the seed compiling app/heff/main.hf failed", .{});
            if (!b.cc("heff build", kept, stage1C, stage1, &flags, &flags)) b.fail("heff build: zig cc failed", .{});
        }
        if (!b.toC(stage1, parts)) b.fail("heff build: the first stage could not compile itself", .{});
        if (!b.cc("heff build", kept, parts, heff, &flags, &flags)) b.fail("heff build: zig cc failed", .{});
        b.prune(kept);
        // (the heff built comes of the seed there is: built from it, or with
        // a heff of it)
        b.root.writeFile(b.io, .{ .sub_path = seedOf, .data = b.concat(&.{ &hash, "\n" }) }) catch |err|
            b.fail("heff build: {s}: {t}", .{ seedOf, err });
        b.say("heff build: {s}, the compiler compiled by itself{s}", .{ heff, if (first) "" else " (from the seed)" });
    }

    /// heff (HEFF, else the one built) compiles its own source to
    /// .heff/out/heff-gen.d/, which must be .heff/out/heff.d/, file for
    /// file, byte for byte.
    fn fixpoint(b: *const Bootstrap) bool {
        const compiler = b.environment.get("HEFF") orelse heff;
        const gen = out ++ "/heff-gen.d";
        if (b.modified(compiler) == null or b.modified(parts) == null) {
            b.complain("heff fixpoint: no {s} and {s} (make build first)", .{ compiler, parts });
            return false;
        }
        const argv = [_][]const u8{ compiler, "compile", "app/heff/main.hf", gen ++ "/", "--name", "main", "--backend=c" };
        if (!b.run(&argv, b.progress)) {
            b.complain("heff fixpoint: the compiler refused its own source", .{});
            return false;
        }
        const endings = [_][]const u8{ ".c", ".h", ".zig" };
        const built = b.filesOf(parts, &endings) orelse return false;
        const child = b.filesOf(gen, &endings) orelse return false;
        if (built.len != child.len) return b.otherFiles(built, child);
        for (built, child) |x, y| if (!std.mem.eql(u8, x, y)) return b.otherFiles(built, child);
        var bytes: usize = 0;
        for (built) |name| {
            const x = b.read(b.concat(&.{ parts, "/", name })) orelse return false;
            const y = b.read(b.concat(&.{ gen, "/", name })) orelse return false;
            bytes += x.len;
            if (!std.mem.eql(u8, x, y)) {
                b.firstDifference(name, x, y);
                return false;
            }
        }
        b.say("heff fixpoint: the compiler compiles itself to itself, {d} files, {d} bytes of C, byte-identical", .{ built.len, bytes });
        return true;
    }

    fn otherFiles(b: *const Bootstrap, built: []const []const u8, child: []const []const u8) bool {
        b.complain("heff fixpoint: other files: {d} built, {d} of its child", .{ built.len, child.len });
        for (built) |name| if (!contains(child, name)) b.complain("  only built:     {s}", .{name});
        for (child) |name| if (!contains(built, name)) b.complain("  only its child: {s}", .{name});
        return false;
    }

    fn contains(names: []const []const u8, name: []const u8) bool {
        for (names) |other| if (std.mem.eql(u8, other, name)) return true;
        return false;
    }

    fn firstDifference(b: *const Bootstrap, name: []const u8, x: []const u8, y: []const u8) void {
        var xs = std.mem.splitScalar(u8, x, '\n');
        var ys = std.mem.splitScalar(u8, y, '\n');
        var line: usize = 1;
        while (true) : (line += 1) {
            const xl = xs.next() orelse "";
            const yl = ys.next() orelse "";
            if (!std.mem.eql(u8, xl, yl)) {
                b.complain("heff fixpoint: {s}, first difference at line {d}\n  built:     {s}\n  its child: {s}", .{ name, line, xl[0..@min(xl.len, 120)], yl[0..@min(yl.len, 120)] });
                return;
            }
        }
    }

    /// The seed renewed: heff, once it compiles itself to the same C, kept
    /// as that C in boot/heff.tar.xz -- a tar of heff.d's files by name,
    /// each of no owner and of time 0, so that the same C is the same
    /// seed, compressed by xz.
    fn boot(b: *const Bootstrap) void {
        if (!b.fixpoint()) b.fail("heff boot: the compiler does not compile itself to itself; the seed is left as it was", .{});
        const names = b.filesOf(parts, &.{""}) orelse b.fail("heff boot: no {s}", .{parts});
        var tarred: Io.Writer.Allocating = .init(b.arena);
        var tar: std.tar.Writer = .{ .underlying_writer = &tarred.writer };
        for (names) |name| {
            const bytes = b.read(b.concat(&.{ parts, "/", name })) orelse b.fail("heff boot: {s}/{s} could not be read", .{ parts, name });
            tar.writeFileBytes(name, bytes, .{ .mode = 0o644, .mtime = 0 }) catch |err| b.fail("heff boot: {t}", .{err});
        }
        tar.finishPedantically() catch |err| b.fail("heff boot: {t}", .{err});

        // (xz, by one thread: by more, it cuts the stream into blocks, as
        // many as the machine has cores)
        const renewed = seed ++ ".part";
        const file = b.root.createFile(b.io, renewed, .{}) catch |err| b.fail("heff boot: {s}: {t}", .{ renewed, err });
        var xz = std.process.spawn(b.io, .{
            .argv = &.{ "xz", "--compress", "--stdout", "--threads=1" },
            .stdin = .pipe,
            .stdout = .{ .file = file },
        }) catch |err| b.fail("heff boot: xz could not be run: {t}", .{err});
        xz.stdin.?.writeStreamingAll(b.io, tarred.written()) catch |err| b.fail("heff boot: xz: {t}", .{err});
        xz.stdin.?.close(b.io);
        xz.stdin = null;
        const term = xz.wait(b.io) catch |err| b.fail("heff boot: xz: {t}", .{err});
        file.close(b.io);
        if (!term.success()) b.fail("heff boot: xz failed", .{});
        Dir.rename(b.root, renewed, b.root, seed, b.io) catch |err| b.fail("heff boot: {s}: {t}", .{ seed, err });

        // (the heff built comes of the seed -- the seed is its C -- and is
        // kept so, by the seed's hash beside it: a build goes on with it,
        // not from the seed)
        const hash = b.seedHash();
        b.root.writeFile(b.io, .{ .sub_path = seedOf, .data = b.concat(&.{ &hash, "\n" }) }) catch |err|
            b.fail("heff boot: {s}: {t}", .{ seedOf, err });
        b.say("heff boot: {s} is the compiler, compiled by itself to C", .{seed});
    }

    /// heff for other systems: .heff/out/heff.d/ built by `zig cc` for
    /// each target (as `zig targets` names them) to
    /// .heff/out/native/TARGET/heff (heff.exe for Windows): stripped, and
    /// Linux's linked statically by musl, so it runs anywhere; the
    /// objects kept in .heff/out/native.o/, apart from the build's, which
    /// lets go of those of no part of it.
    fn native(b: *const Bootstrap, targets: []const []const u8) void {
        for (targets) |target| {
            const directory = b.concat(&.{ out, "/native/", target });
            b.root.createDirPath(b.io, directory) catch |err| b.fail("heff native: {s}: {t}", .{ directory, err });
            const program = b.concat(&.{ directory, "/", programName(target) });
            const compile = std.mem.concat(b.arena, []const u8, &.{ &.{ "-target", target, "-O2", "-w" }, unwind(isWindows(target)) }) catch
                b.fail("heff native: out of memory", .{});
            const link = [_][]const u8{ "-target", target, "-O2", "-s", "-w" };
            if (!b.cc("heff native", nativeKept, parts, program, compile, &link)) b.fail("heff native: {s} failed", .{target});
        }
        b.prune(nativeKept);
    }

    /// The release, in .heff/out/release/ anew: heff built for each of
    /// the platforms, as `native` builds it, and packed with what it
    /// ships as heff-VERSION-PLATFORM.tar.gz (.zip for Windows), the
    /// version package.hf's; and SHA256SUMS, of each archive.
    fn release(b: *const Bootstrap) void {
        const version = versionOf(b.read("package.hf") orelse b.fail("heff release: no package.hf", .{})) orelse
            b.fail("heff release: package.hf says no version (heff = \"X.Y.Z\")", .{});
        var targets: [platforms.len][]const u8 = undefined;
        for (platforms, &targets) |platform, *target| target.* = platform.target;
        b.native(&targets);

        b.root.deleteTree(b.io, releaseDir) catch |err| b.fail("heff release: {s}: {t}", .{ releaseDir, err });
        b.root.createDirPath(b.io, releaseDir) catch |err| b.fail("heff release: {s}: {t}", .{ releaseDir, err });
        // (zig's deflate is slow in bootstrap's build, which is not
        // optimized: the archives are packed at once, a thread each)
        const start = Io.Timestamp.now(b.io, .awake);
        var packs: [platforms.len]Pack = undefined;
        var threads: [platforms.len]?std.Thread = undefined;
        for (platforms, &packs, &threads) |platform, *pack, *thread| {
            pack.* = .{ .b = b, .version = version, .platform = platform };
            thread.* = std.Thread.spawn(.{}, Pack.pack, .{pack}) catch null;
            if (thread.* == null) pack.pack();
        }
        for (threads) |thread| if (thread) |started| started.join();
        var sums: std.ArrayList(u8) = .empty;
        for (&packs) |*pack| {
            defer pack.arena.deinit();
            sums.appendSlice(b.arena, b.concat(&.{ &pack.digest, "  ", pack.file, "\n" })) catch b.fail("heff release: out of memory", .{});
            b.say("heff release: {s}/{s}, {d} files, {d} bytes", .{ releaseDir, pack.file, pack.files, pack.size });
        }
        const summed = releaseDir ++ "/SHA256SUMS";
        b.root.writeFile(b.io, .{ .sub_path = summed, .data = sums.items }) catch |err| b.fail("heff release: {s}: {t}", .{ summed, err });
        b.say("heff release: {s}, the archives packed in {d:.1} s", .{ summed, b.seconds(start) });
    }

    /// The archive of a platform, packed by a thread of its own, in an
    /// arena of its own: its file's name, how many files it has, its
    /// size and its SHA-256.
    const Pack = struct {
        b: *const Bootstrap,
        version: []const u8,
        platform: Platform,
        arena: std.heap.ArenaAllocator = .init(std.heap.page_allocator),
        file: []const u8 = "",
        files: usize = 0,
        size: usize = 0,
        digest: [64]u8 = undefined,

        fn pack(p: *Pack) void {
            var each = p.b.*;
            each.arena = p.arena.allocator();
            const name = each.concat(&.{ "heff-", p.version, "-", p.platform.name });
            const windows = isWindows(p.platform.target);
            const contents = each.contentsFor(p.platform.target);
            const archive = if (windows) each.zip(name, contents) else each.gzip(each.tarball(name, contents));
            p.file = each.concat(&.{ name, if (windows) ".zip" else ".tar.gz" });
            const path = each.concat(&.{ releaseDir, "/", p.file });
            each.root.writeFile(each.io, .{ .sub_path = path, .data = archive }) catch |err| each.fail("heff release: {s}: {t}", .{ path, err });
            for (contents) |entry| p.files += @intFromBool(!entry.isDirectory());
            p.size = archive.len;
            var digest: [32]u8 = undefined;
            std.crypto.hash.sha2.Sha256.hash(archive, &digest, .{});
            p.digest = std.fmt.bytesToHex(digest, .lower);
        }
    };

    /// What an archive for `target` holds, as `heff install` lays it out
    /// in the heff home: bin/heff, and what it ships (app/heff/build.hf's
    /// `ships`), with the license, the README and the docs; each
    /// directory with a "/" after it, sorted by path.
    fn contentsFor(b: *const Bootstrap, target: []const u8) []const Entry {
        var entries: std.ArrayList(Entry) = .empty;
        const program = b.concat(&.{ out, "/native/", target, "/", programName(target) });
        b.add(&entries, .{ .path = "bin/", .bytes = "", .mode = 0o755 });
        b.add(&entries, .{
            .path = b.concat(&.{ "bin/", programName(target) }),
            .bytes = b.read(program) orelse b.fail("heff release: no {s}", .{program}),
            .mode = 0o755,
        });
        for (shipped) |path| {
            if (b.read(path)) |bytes| {
                b.add(&entries, .{ .path = path, .bytes = bytes, .mode = 0o644 });
                continue;
            }
            var directory = b.root.openDir(b.io, path, .{ .iterate = true }) catch b.fail("heff release: no {s}", .{path});
            defer directory.close(b.io);
            b.add(&entries, .{ .path = b.concat(&.{ path, "/" }), .bytes = "", .mode = 0o755 });
            var walker = directory.walk(b.arena) catch b.fail("heff release: out of memory", .{});
            defer walker.deinit();
            while (walker.next(b.io) catch |err| b.fail("heff release: {s}: {t}", .{ path, err })) |entry| {
                const inside = b.arena.dupe(u8, entry.path) catch b.fail("heff release: out of memory", .{});
                std.mem.replaceScalar(u8, inside, '\\', '/');
                if (hidden(inside)) continue;
                // of docs/, its pages only: not the site made of them
                if (std.mem.eql(u8, path, "docs") and site(inside, entry.kind == .file)) continue;
                const whole = b.concat(&.{ path, "/", inside });
                switch (entry.kind) {
                    .directory => b.add(&entries, .{ .path = b.concat(&.{ whole, "/" }), .bytes = "", .mode = 0o755 }),
                    .file => b.add(&entries, .{
                        .path = whole,
                        .bytes = b.read(whole) orelse b.fail("heff release: {s} could not be read", .{whole}),
                        .mode = 0o644,
                    }),
                    else => {},
                }
            }
        }
        std.mem.sort(Entry, entries.items, {}, Entry.lessThan);
        return entries.items;
    }

    fn add(b: *const Bootstrap, entries: *std.ArrayList(Entry), entry: Entry) void {
        entries.append(b.arena, entry) catch b.fail("heff release: out of memory", .{});
    }

    /// `entries` as a tar, each in the directory `name`: of no owner
    /// and of time 0, so that the same files make the same archive.
    fn tarball(b: *const Bootstrap, name: []const u8, entries: []const Entry) []const u8 {
        var tarred: Io.Writer.Allocating = .init(b.arena);
        var writer: std.tar.Writer = .{ .underlying_writer = &tarred.writer };
        writer.writeDir(b.concat(&.{ name, "/" }), .{ .mode = 0o755, .mtime = 0 }) catch |err| b.fail("heff release: {t}", .{err});
        for (entries) |entry| {
            const path = b.concat(&.{ name, "/", entry.path });
            const options: std.tar.Writer.Options = .{ .mode = entry.mode, .mtime = 0 };
            const written = if (entry.isDirectory())
                writer.writeDir(path, options)
            else
                writer.writeFileBytes(path, entry.bytes, options);
            written catch |err| b.fail("heff release: {s}: {t}", .{ entry.path, err });
        }
        writer.finishPedantically() catch |err| b.fail("heff release: {t}", .{err});
        return tarred.written();
    }

    /// `bytes` compressed by deflate in `container`: gzip's of no name
    /// and of time 0, or raw, as a zip's.
    fn deflate(b: *const Bootstrap, bytes: []const u8, container: std.compress.flate.Container) []const u8 {
        var compressed = Io.Writer.Allocating.initCapacity(b.arena, bytes.len / 2 + 64) catch b.fail("heff release: out of memory", .{});
        const window = b.arena.alloc(u8, std.compress.flate.max_window_len) catch b.fail("heff release: out of memory", .{});
        var compress = std.compress.flate.Compress.init(&compressed.writer, window, container, .default) catch |err|
            b.fail("heff release: deflate: {t}", .{err});
        compress.writer.writeAll(bytes) catch |err| b.fail("heff release: deflate: {t}", .{err});
        compress.finish() catch |err| b.fail("heff release: deflate: {t}", .{err});
        return compressed.written();
    }

    fn gzip(b: *const Bootstrap, bytes: []const u8) []const u8 {
        return b.deflate(bytes, .gzip);
    }

    /// `entries` as a zip, each in the directory `name`: each file
    /// deflated, of time 1980-01-01 00:00 (a zip's first) and of its
    /// Unix mode, so that the same files make the same archive.
    fn zip(b: *const Bootstrap, name: []const u8, entries: []const Entry) []const u8 {
        var archive: Io.Writer.Allocating = .init(b.arena);
        var central: Io.Writer.Allocating = .init(b.arena);
        const top: Entry = .{ .path = "", .bytes = "", .mode = 0o755 };
        b.zipEntry(&archive, &central, name, top) catch |err| b.fail("heff release: {t}", .{err});
        for (entries) |entry| b.zipEntry(&archive, &central, name, entry) catch |err|
            b.fail("heff release: {s}: {t}", .{ entry.path, err });
        const offset: u32 = @intCast(archive.written().len);
        const size: u32 = @intCast(central.written().len);
        const count: u16 = @intCast(entries.len + 1);
        zipEnd(&archive.writer, central.written(), count, size, offset) catch b.fail("heff release: out of memory", .{});
        return archive.written();
    }

    /// An entry of a zip: its local header and its data to `archive`,
    /// its header in the central directory to `central`.
    fn zipEntry(b: *const Bootstrap, archive: *Io.Writer.Allocating, central: *Io.Writer.Allocating, name: []const u8, entry: Entry) Io.Writer.Error!void {
        const path = b.concat(&.{ name, "/", entry.path });
        const directory = entry.isDirectory();
        const method: u16 = if (directory) 0 else 8;
        const data = if (directory) "" else b.deflate(entry.bytes, .raw);
        const crc = std.hash.Crc32.hash(entry.bytes);
        const offset: u32 = @intCast(archive.written().len);
        // (Unix's file type and mode, above MS-DOS's attributes)
        const unix: u32 = (if (directory) @as(u32, 0o040000) else 0o100000) | entry.mode;
        const attributes: u32 = (unix << 16) | (if (directory) @as(u32, 0x10) else 0);
        const date: u16 = (0 << 9) | (1 << 5) | 1;

        const l = &archive.writer;
        try l.writeAll(&std.zip.local_file_header_sig);
        try l.writeInt(u16, 20, .little); // version needed: 2.0
        try l.writeInt(u16, 0, .little); // flags
        try l.writeInt(u16, method, .little);
        try l.writeInt(u16, 0, .little); // time
        try l.writeInt(u16, date, .little);
        try l.writeInt(u32, crc, .little);
        try l.writeInt(u32, @intCast(data.len), .little);
        try l.writeInt(u32, @intCast(entry.bytes.len), .little);
        try l.writeInt(u16, @intCast(path.len), .little);
        try l.writeInt(u16, 0, .little); // no extra field
        try l.writeAll(path);
        try l.writeAll(data);

        const c = &central.writer;
        try c.writeAll(&std.zip.central_file_header_sig);
        try c.writeInt(u16, (3 << 8) | 20, .little); // made by Unix, 2.0
        try c.writeInt(u16, 20, .little);
        try c.writeInt(u16, 0, .little);
        try c.writeInt(u16, method, .little);
        try c.writeInt(u16, 0, .little);
        try c.writeInt(u16, date, .little);
        try c.writeInt(u32, crc, .little);
        try c.writeInt(u32, @intCast(data.len), .little);
        try c.writeInt(u32, @intCast(entry.bytes.len), .little);
        try c.writeInt(u16, @intCast(path.len), .little);
        try c.writeInt(u16, 0, .little); // no extra field
        try c.writeInt(u16, 0, .little); // no comment
        try c.writeInt(u16, 0, .little); // disk
        try c.writeInt(u16, 0, .little); // internal attributes
        try c.writeInt(u32, attributes, .little);
        try c.writeInt(u32, offset, .little);
        try c.writeAll(path);
    }
};

/// The platforms a release is for: each by its name in the archive's,
/// and the zig target heff is built for (Linux's linked statically, by
/// musl), sorted by name.
const Platform = struct { name: []const u8, target: []const u8 };
const platforms = [_]Platform{
    .{ .name = "linux-arm64", .target = "aarch64-linux-musl" },
    .{ .name = "linux-x64", .target = "x86_64-linux-musl" },
    .{ .name = "macos-arm64", .target = "aarch64-macos" },
    .{ .name = "windows-x64", .target = "x86_64-windows-gnu" },
};
const releaseDir = out ++ "/release";

/// What an archive has beside bin/: what heff ships, as `heff install`
/// puts it in the heff home (app/heff/build.hf's `ships`: lib/,
/// package.hf), and the license, the README and the docs' pages.
const shipped = [_][]const u8{ "LICENSE", "README.md", "docs", "lib", "package.hf" };

/// A file or a directory (its path ending in "/", or "", the archive's
/// own) of an archive.
const Entry = struct {
    path: []const u8,
    bytes: []const u8,
    mode: u32,

    fn isDirectory(entry: Entry) bool {
        return entry.path.len == 0 or std.mem.endsWith(u8, entry.path, "/");
    }

    fn lessThan(_: void, x: Entry, y: Entry) bool {
        return std.mem.lessThan(u8, x.path, y.path);
    }
};

/// A zip's central directory, and its end record: how many entries,
/// how long the central directory is and where it starts.
fn zipEnd(w: *Io.Writer, central: []const u8, count: u16, size: u32, offset: u32) Io.Writer.Error!void {
    try w.writeAll(central);
    try w.writeAll(&std.zip.end_record_sig);
    try w.writeInt(u16, 0, .little); // this disk
    try w.writeInt(u16, 0, .little); // the central directory's disk
    try w.writeInt(u16, count, .little);
    try w.writeInt(u16, count, .little);
    try w.writeInt(u32, size, .little);
    try w.writeInt(u32, offset, .little);
    try w.writeInt(u16, 0, .little); // no comment
}

fn isWindows(target: []const u8) bool {
    return std.mem.indexOf(u8, target, "windows") != null;
}

/// The name of heff built for `target`.
fn programName(target: []const u8) []const u8 {
    return if (isWindows(target)) "heff.exe" else "heff";
}

/// Of docs/, what is not its pages but the site made of them: its source
/// (src/), its packages (node_modules/), what it makes (dist/), and any
/// file not a .md one.
fn site(inside: []const u8, file: bool) bool {
    const first = inside[0 .. std.mem.indexOfScalar(u8, inside, '/') orelse inside.len];
    for ([_][]const u8{ "src", "node_modules", "dist" }) |part| if (std.mem.eql(u8, first, part)) return true;
    return file and !std.mem.endsWith(u8, inside, ".md");
}

/// A path any part of which is hidden (.git, .heff, .DS_Store).
fn hidden(path: []const u8) bool {
    var pieces = std.mem.splitScalar(u8, path, '/');
    while (pieces.next()) |part| if (part.len > 0 and part[0] == '.') return true;
    return false;
}

/// The version package.hf says: `heff = "X.Y.Z"`.
fn versionOf(text: []const u8) ?[]const u8 {
    const key = "heff = \"";
    const at = std.mem.indexOf(u8, text, key) orelse return null;
    const rest = text[at + key.len ..];
    const end = std.mem.indexOfScalar(u8, rest, '"') orelse return null;
    return rest[0..end];
}

/// What `zig env` says of `field`: `.field = "VALUE"`.
fn zon(said: []const u8, field: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, said, '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " ");
        if (trimmed.len < field.len + 6 or trimmed[0] != '.' or !std.mem.startsWith(u8, trimmed[1..], field)) continue;
        const rest = trimmed[1 + field.len ..];
        if (!std.mem.startsWith(u8, rest, " = \"")) continue;
        const value = rest[4..];
        const end = std.mem.indexOfScalar(u8, value, '"') orelse continue;
        return value[0..end];
    }
    return null;
}

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const io = init.io;
    const arguments = try init.minimal.args.toSlice(arena);
    if (arguments.len < 3) {
        try Io.File.stderr().writeStreamingAll(io, "bootstrap ZIG (build | seed | fixpoint | boot | native TARGET... | release)\n");
        std.process.exit(2);
    }
    const root = Dir.cwd();
    const here = try root.realPathFileAlloc(io, ".", arena);
    // (the library each stage compiles with: the repository's, wherever
    // the stage is)
    if (init.environ_map.get("HEFF_LIB") == null) {
        try init.environ_map.put("HEFF_LIB", try std.fs.path.join(arena, &.{ here, "lib" }));
    }
    const command = arguments[2];
    const zig = arguments[1];
    const said = std.process.run(arena, io, .{ .argv = &.{ zig, "env" } }) catch |err| {
        try Io.File.stderr().writeStreamingAll(io, "heff build: zig env failed (https://ziglang.org/download/)\n");
        return err;
    };
    // (zig keeps what it makes of each part too, in its cache: not the
    // build's, .zig-cache/, where zig run here would keep it, beside
    // build.zig, nor the one it keeps for all, which builds in two
    // checkouts at once would share, and whose tmp/ grows without end
    // (#87), but this checkout's, begun afresh each run: the objects
    // worth keeping are kept here, in .heff/out/heff.o/)
    if (init.environ_map.get("ZIG_LOCAL_CACHE_DIR") == null) {
        root.deleteTree(io, zigCache) catch {};
        try init.environ_map.put("ZIG_LOCAL_CACHE_DIR", try std.fs.path.join(arena, &.{ here, zigCache }));
    }
    var used: std.StringHashMapUnmanaged(void) = .empty;
    const b: Bootstrap = .{
        .arena = arena,
        .io = io,
        .zig = zig,
        .zigIs = if (zon(said.stdout, "version")) |version|
            if (zon(said.stdout, "target")) |target| try std.mem.concat(arena, u8, &.{ version, " ", target }) else said.stdout
        else
            said.stdout,
        .used = &used,
        .environment = init.environ_map,
        .progress = std.Progress.start(io, .{ .root_name = command }),
        .root = root,
    };
    defer b.progress.end();
    if (std.mem.eql(u8, command, "build")) {
        b.build(false);
    } else if (std.mem.eql(u8, command, "seed")) {
        b.build(true);
    } else if (std.mem.eql(u8, command, "fixpoint")) {
        if (!b.fixpoint()) std.process.exit(1);
    } else if (std.mem.eql(u8, command, "boot")) {
        b.boot();
    } else if (std.mem.eql(u8, command, "release")) {
        if (b.modified(parts) == null) b.fail("heff release: no {s} (make build first)", .{parts});
        b.release();
    } else if (std.mem.eql(u8, command, "native")) {
        if (b.modified(parts) == null) b.fail("heff native: no {s} (make build first)", .{parts});
        b.native(arguments[3..]);
    } else {
        b.fail("bootstrap: no command {s}", .{command});
    }
}
