// The compiler the tests run: `HEFF`, else the one built
// (scripts/build.mjs), .heff/out/heff.
import { spawnSync } from "node:child_process";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export const root = join(dirname(fileURLToPath(import.meta.url)), "..");
export const heffBin = process.env.HEFF
  ? resolve(process.env.HEFF)
  : join(root, ".heff", "out", process.platform === "win32" ? "heff.exe" : "heff");

// (heff run with `args`; `opts` as spawnSync's, a run that does not end
// in two minutes a failure, not a wait)
export const heff = (args, opts = {}) =>
  spawnSync(heffBin, args, { encoding: "utf8", maxBuffer: 1 << 28, timeout: 120000, ...opts });
