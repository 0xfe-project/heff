// The repository the tests are of, and the compiler they run: `HEFF`,
// else the one built (`make build`), .heff/out/heff.
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { run } from "./pool.mjs";

export const root = join(dirname(fileURLToPath(import.meta.url)), "..");
export const heffBin = process.env.HEFF
  ? resolve(process.env.HEFF)
  : join(root, ".heff", "out", process.platform === "win32" ? "heff.exe" : "heff");

// (heff run with `args`, as test/pool.mjs runs a process -- once it has
// its slot -- `opts` as its; a run that does not end in two minutes a
// failure, not a wait)
export const heff = (args, opts = {}) => run(heffBin, args, { timeout: 120000, ...opts });
