// Load one compiled module and print its values as JSON (run in a child
// process, so that a module that loops while loading can be timed out).
import { pathToFileURL } from "node:url";
import { inspect } from "./runtime-inspect.mjs";
const [file, ...names] = process.argv.slice(2);
const mod = await import(pathToFileURL(file));
const out = {};
for (const n of names) if (n in mod) out[n] = inspect(mod[n]);
process.stdout.write(JSON.stringify(out));
