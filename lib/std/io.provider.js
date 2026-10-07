// The standard streams' side of lib/std/io.hf: an effect provider
// (the shape of one is said in lib/std/dom.provider.js).
export const $host = "node";
export const stdio = () => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Write": process.stdout.write(a[0], () => k(a[1])); return;
    case "WriteErr": process.stderr.write(a[0], () => k(a[1])); return;
    case "ReadStdin": { let text = ""; process.stdin.setEncoding("utf8"); process.stdin.on("data", (d) => { text += d; }); process.stdin.on("end", () => k(a[0](text))); return; }
    default: throw new Error("heff: not an operation of stdio: " + (p && p.$));
  }
};
