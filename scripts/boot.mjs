#!/usr/bin/env node
// The seed renewed: heff (built by scripts/build.mjs), once it compiles
// itself to the same C (test/gen-fixpoint.mjs), is kept as that C --
// .heff/out/heff.d/, a file a module -- in boot/heff.tar.xz, the seed a
// build starts from where there is no heff: zig builds it for whatever
// machine the build is on.  Wanted when the language changes so that
// the old seed cannot compile the new source; otherwise the old seed
// does.
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const run = (cmd, args, env = process.env) => spawnSync(cmd, args, { cwd: root, stdio: "inherit", env }).status === 0;

if (!run(process.execPath, [join(root, "scripts", "build.mjs")])) process.exit(1);
if (!run(process.execPath, [join(root, "test", "gen-fixpoint.mjs")])) {
  console.error("heff boot: the compiler does not compile itself to itself; the seed is left as it was");
  process.exit(1);
}
// (`tar` with xz: libarchive's, on macOS and Windows; GNU's with xz, on
// Linux.  macOS's would add a `._NAME` of each file's attributes)
if (!run("tar", ["-cJf", join("boot", "heff.tar.xz"), "-C", join(".heff", "out", "heff.d"), "."],
  { ...process.env, COPYFILE_DISABLE: "1" })) process.exit(1);
console.log("heff boot: boot/heff.tar.xz is the compiler, compiled by itself to C");
