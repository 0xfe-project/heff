//! The compiler, bootstrapped, as build.zig runs it: a compiler there is
//! (stage 0) compiles app/heff/main.hf to a first stage, and the first
//! stage compiles the same source to C, .heff/out/heff.d/ (a file a
//! module) -- made by the backend of the source, whatever stage 0's was --
//! which `zig cc` builds into .heff/out/heff, `heff`.  Stage 0 is the heff
//! built before (.heff/out/heff), as Go builds with a Go there is, if it
//! comes of the seed there is: built from it, or with a heff that came of
//! it (.heff/out/heff.seed, the seed's hash).  A seed is renewed when the
//! source wants what an older heff has not, so a heff of another seed, or
//! of one not known, is not built with.  Where there is no heff of the
//! seed, or it cannot make the first stage, the seed: boot/heff.tar.xz,
//! heff compiled by itself to C, unpacked to .heff/out/heff-seed.d/ and
//! built by zig for this machine.  zig builds each part of C apart, and
//! the objects are kept by what they are made of, in .heff/out/heff.o/.
//!
//!   bootstrap ZIG build            the compiler, from the heff there is
//!   bootstrap ZIG seed             the compiler, from the seed, always
//!   bootstrap ZIG fixpoint         heff compiles itself to the same C
//!   bootstrap ZIG boot             the seed renewed, once it does
//!   bootstrap ZIG native TARGET... heff for other systems
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

// (for any machine of this kind, not only this one: zig's default is this
// machine's processor, whose instructions another may not have)
const flags = [_][]const u8{ "-mcpu=baseline", "-O2", "-w" };

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
            std.mem.endsWith(u8, name, ".c");
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
    /// object, kept in `into` and named by what it is made of -- the zig
    /// and the machine it builds for, the flags, heff.h and the part -- so
    /// that a part as it was is not built again, wherever it is and
    /// whichever stage it is of; as many at once as there are cores, then
    /// the objects linked.  (zig's own cache knows a C file by its path,
    /// time and inode, not by what is in it, so it builds again a part
    /// written again, as every part is in a CI's checkout or its cache
    /// restored.)
    fn cc(b: *const Bootstrap, who: []const u8, into: []const u8, directory: []const u8, to: []const u8, compile: []const []const u8, link: []const []const u8) bool {
        const files = b.filesOf(directory, &.{".c"}) orelse return false;
        b.root.createDirPath(b.io, into) catch return false;
        var madeOf = std.crypto.hash.sha2.Sha256.init(.{});
        madeOf.update(b.zigIs);
        madeOf.update(std.mem.join(b.arena, " ", compile) catch return false);
        madeOf.update(b.read(b.concat(&.{ directory, "/heff.h" })) orelse "");
        const objects = b.arena.alloc([]const u8, files.len) catch return false;
        var anew: std.ArrayList(usize) = .empty;
        for (files, 0..) |file, k| {
            const bytes = b.read(b.concat(&.{ directory, "/", file })) orelse return false;
            var part = madeOf;
            part.update(bytes);
            var digest: [32]u8 = undefined;
            part.final(&digest);
            const hex = std.fmt.bytesToHex(digest, .lower);
            const name = b.concat(&.{ file[0 .. file.len - 2], "-", hex[0..16], ".o" });
            b.used.put(b.arena, name, {}) catch return false;
            objects[k] = b.concat(&.{ into, "/", name });
            if (b.modified(objects[k]) == null) anew.append(b.arena, k) catch return false;
        }

        const name = if (std.mem.eql(u8, who, "heff build")) std.fs.path.basename(to) else to;
        const node = b.progress.start(b.concat(&.{ "zig builds ", name }), anew.items.len);
        defer node.end();
        const start = Io.Timestamp.now(b.io, .awake);
        if (!b.compileParts(compile, directory, files, objects, anew.items, node)) return false;
        var argv: std.ArrayList([]const u8) = .empty;
        argv.appendSlice(b.arena, &.{ b.zig, "cc" }) catch return false;
        argv.appendSlice(b.arena, link) catch return false;
        argv.appendSlice(b.arena, objects) catch return false;
        argv.appendSlice(b.arena, &.{ "-o", to, "-lm" }) catch return false;
        if (!b.run(argv.items, node)) return false;
        b.say("{s}: zig built {s} ({d} of its {d} parts anew) in {d:.1} s", .{ who, name, anew.items.len, files.len, b.seconds(start) });
        return true;
    }

    /// The parts `anew` of `files`, to compile each to its object.
    const Work = struct {
        b: *const Bootstrap,
        compile: []const []const u8,
        directory: []const u8,
        files: []const []const u8,
        objects: []const []const u8,
        anew: []const usize,
        node: std.Progress.Node,
        next: std.atomic.Value(usize) = .init(0),
        failed: std.atomic.Value(bool) = .init(false),

        /// One worker: the next part not taken, while there is one and
        /// none failed, compiled to OBJECT.part, renamed once it is whole,
        /// so that an object there is is never half made.
        fn work(w: *Work) void {
            const b = w.b;
            while (!w.failed.load(.monotonic)) {
                const at = w.next.fetchAdd(1, .monotonic);
                if (at >= w.anew.len) return;
                const k = w.anew[at];
                const part = b.concat(&.{ w.objects[k], ".part" });
                var argv: std.ArrayList([]const u8) = .empty;
                argv.appendSlice(b.arena, &.{ b.zig, "cc" }) catch return w.failed.store(true, .monotonic);
                argv.appendSlice(b.arena, w.compile) catch return w.failed.store(true, .monotonic);
                argv.appendSlice(b.arena, &.{ "-c", b.concat(&.{ w.directory, "/", w.files[k] }), "-o", part }) catch
                    return w.failed.store(true, .monotonic);
                if (!b.run(argv.items, .none)) return w.failed.store(true, .monotonic);
                Dir.rename(b.root, part, b.root, w.objects[k], b.io) catch return w.failed.store(true, .monotonic);
                w.node.completeOne();
            }
        }
    };

    /// The parts `anew` of `files` compiled each to its object, as many
    /// at once as there are cores.
    fn compileParts(b: *const Bootstrap, compile: []const []const u8, directory: []const u8, files: []const []const u8, objects: []const []const u8, anew: []const usize, node: std.Progress.Node) bool {
        var w: Work = .{ .b = b, .compile = compile, .directory = directory, .files = files, .objects = objects, .anew = anew, .node = node };
        const jobs = @min(std.Thread.getCpuCount() catch 4, anew.len);
        const workers = b.arena.alloc(std.Thread, jobs) catch return false;
        var started: usize = 0;
        for (workers) |*worker| {
            worker.* = std.Thread.spawn(.{}, Work.work, .{&w}) catch break;
            started += 1;
        }
        if (started == 0 and jobs > 0) w.work();
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
        const endings = [_][]const u8{ ".c", ".h" };
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
            const program = b.concat(&.{ directory, if (std.mem.indexOf(u8, target, "windows") != null) "/heff.exe" else "/heff" });
            const compile = [_][]const u8{ "-target", target, "-O2", "-w" };
            const link = [_][]const u8{ "-target", target, "-O2", "-s", "-w" };
            if (!b.cc("heff native", nativeKept, parts, program, &compile, &link)) b.fail("heff native: {s} failed", .{target});
        }
        b.prune(nativeKept);
    }
};

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
        try Io.File.stderr().writeStreamingAll(io, "bootstrap ZIG (build | seed | fixpoint | boot | native TARGET...)\n");
        std.process.exit(2);
    }
    const root = Dir.cwd();
    // (the library each stage compiles with: the repository's, wherever
    // the stage is)
    if (init.environ_map.get("HEFF_LIB") == null) {
        const here = try root.realPathFileAlloc(io, ".", arena);
        try init.environ_map.put("HEFF_LIB", try std.fs.path.join(arena, &.{ here, "lib" }));
    }
    const command = arguments[2];
    const zig = arguments[1];
    const said = std.process.run(arena, io, .{ .argv = &.{ zig, "env" } }) catch |err| {
        try Io.File.stderr().writeStreamingAll(io, "heff build: zig env failed (https://ziglang.org/download/)\n");
        return err;
    };
    // (zig keeps what it makes of each part too, in its cache: the one it
    // keeps for all, not the build's, .zig-cache/, where zig run here
    // would keep it, beside build.zig)
    if (init.environ_map.get("ZIG_LOCAL_CACHE_DIR") == null) {
        if (zon(said.stdout, "global_cache_dir")) |global| try init.environ_map.put("ZIG_LOCAL_CACHE_DIR", global);
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
    } else if (std.mem.eql(u8, command, "native")) {
        if (b.modified(parts) == null) b.fail("heff native: no {s} (make build first)", .{parts});
        b.native(arguments[3..]);
    } else {
        b.fail("bootstrap: no command {s}", .{command});
    }
}
