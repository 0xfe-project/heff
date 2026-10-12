// The standard streams' side of lib/std/io.hf: an effect provider
// (the shape of one is said in lib/browser/dom.provider.js).  What is
// written clears the lines std.progress draws on the terminal, if there
// are any, first (`$heffStatus`, progress.provider.js's), and they come
// back below it.
// Kept for self-hosting and the tests only (`--backend=node`): lib/std/
// is the C backend's, and no other module of it gets a provider for
// Node.js alone.
export const $host = "node";
export const stdio = () => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Write": globalThis.$heffStatus?.hold(); process.stdout.write($toBytes(a[0]), () => k(a[1])); return;
    case "WriteErr": globalThis.$heffStatus?.hold(); process.stderr.write($toBytes(a[0]), () => k(a[1])); return;
    case "ReadStdin": { const got = []; process.stdin.on("data", (d) => { got.push(d); }); process.stdin.on("end", () => k(a[0]($fromBytes(Buffer.concat(got))))); return; }
    default: throw new Error("heff: not an operation of stdio: " + (p && p.$));
  }
};
