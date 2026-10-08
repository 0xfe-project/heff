// The standard streams' side of lib/std/io.hf: an effect provider
// (the shape of one is said in lib/std/dom.provider.js).  What is
// written clears the lines std.progress draws on the terminal, if there
// are any, first (`$heffStatus`, progress.provider.js's), and they come
// back below it.
export const $host = "node";
export const stdio = () => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Write": globalThis.$heffStatus?.hold(); process.stdout.write(a[0], () => k(a[1])); return;
    case "WriteErr": globalThis.$heffStatus?.hold(); process.stderr.write(a[0], () => k(a[1])); return;
    case "ReadStdin": { let text = ""; process.stdin.setEncoding("utf8"); process.stdin.on("data", (d) => { text += d; }); process.stdin.on("end", () => k(a[0](text))); return; }
    default: throw new Error("heff: not an operation of stdio: " + (p && p.$));
  }
};
