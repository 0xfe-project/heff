// Load one compiled module and print its values as JSON (run in a child
// process, so that a module that loops can be timed out).  A name given as
// `!name` is delayed (`HgL.Lazy t`, or `Unit -> t`): forced here, and one
// that abandons shows as `<abandoned>`.
import { pathToFileURL } from "node:url";
import { inspect } from "./runtime-inspect.mjs";
const [file, ...names] = process.argv.slice(2);
const mod = await import(pathToFileURL(file));
const out = {};
for (const a of names) {
  const delayed = a.startsWith("!"), n = delayed ? a.slice(1) : a;
  if (!(n in mod)) continue;
  try { out[n] = inspect(delayed ? mod[n]() : mod[n]); } catch (e) {
    const msg = String(e && e.message || e);
    out[n] = msg === "abandon" ? "<abandoned>" : `<threw ${msg}>`;
  }
}
process.stdout.write(JSON.stringify(out));
