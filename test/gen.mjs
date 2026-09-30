// Random Hippogriff programs: type-directed, so most of them are well typed.
// For the compiler's test (test/compile-check-js.mjs).
// --- deterministic random ---------------------------------------------------
let seed = 1;
export const setSeed = (s) => { seed = Number(s) >>> 0 || 1; };
export const rnd = () => {
  seed ^= seed << 13; seed >>>= 0;
  seed ^= seed >>> 17;
  seed ^= seed << 5; seed >>>= 0;
  return seed / 4294967296;
};
export const pick = (xs) => xs[Math.floor(rnd() * xs.length)];
export const chance = (p) => rnd() < p;
export const range = (a, b) => a + Math.floor(rnd() * (b - a + 1));

// --- type-directed generator --------------------------------------------------
// A type is { k: "sum", name, ctors: [[tag, [argTy...]]] } | { k: "int" }.
// The program is in heff's syntax (`.hf`); a `case` is laid out, its
// alternatives on lines of their own, each nested one indented further.
export function genProgram() {
  const out = [];
  const sums = [];
  const fns = []; // { name, params: [ty], ret: ty }
  const bool = { k: "sum", name: "Bool", ctors: [["true", []], ["false", []]] };
  out.push("data Bool = 'true | 'false\n");
  sums.push(bool);
  const nSums = range(1, 3);
  for (let i = 0; i < nSums; i++) {
    const name = `T${i}`;
    const self = { k: "sum", name, ctors: [] };
    const nCtors = range(1, 3);
    for (let j = 0; j < nCtors; j++) {
      const nArgs = range(0, 2);
      const args = [];
      for (let a = 0; a < nArgs; a++) {
        args.push(pick([bool, ...sums, self, { k: "int" }]));
      }
      self.ctors.push([`c${i}${j}`, args]);
    }
    // a recursive type needs a base case
    if (!self.ctors.some(([, as]) => as.every((t) => t !== self))) self.ctors.push([`z${i}`, []]);
    sums.push(self);
    out.push(`data ${name} = ` + self.ctors.map(([t, as]) => `'${t}` + as.map((a) => " " + tyStr(a)).join("")).join(" | ") + "\n");
  }
  // a theory, and a record of it
  if (chance(0.7)) {
    const carrier = pick(sums);
    out.push("theory Th where\n  t : Type\n  op : t -> t -> t\n  pick : t -> Bool\n");
    const env = [{ name: "x", ty: carrier }, { name: "y", ty: carrier }];
    out.push(`Inst : Th\nInst = { t = ${carrier.name}, op = \\x y -> ${genExpr(carrier, env, fns, 2, null, 2)}, pick = \\x -> ${genExpr(bool, env.slice(0, 1), fns, 2, null, 2)} }\n`);
    if (chance(0.5)) out.push(`Spec : Th ~ { t = ${carrier.name} }\nSpec = Inst\n`);
    if (chance(0.6)) {
      out.push(`useTh : (m : Th) -> m.t -> Bool\nuseTh m a = m.pick (m.op a a)\n`);
      out.push(`usedTh : Bool\nusedTh = useTh Inst ${paren(genExpr(carrier, [], fns, 2, null, 2))}\n`);
    }
  }
  const nFns = range(1, 4);
  for (let i = 0; i < nFns; i++) {
    const params = [];
    const np = range(1, 3);
    for (let p = 0; p < np; p++) params.push(pick(sums));
    const ret = pick([...sums, ...sums]);
    const f = { name: `f${i}`, params, ret };
    const env = params.map((t, p) => ({ name: `p${p}`, ty: t }));
    const body = genExpr(ret, env, fns, 3, f, 0);
    out.push(`${f.name} : ${[...params, ret].map(tyStr).join(" -> ")}\n${f.name} ${params.map((_, p) => `p${p}`).join(" ")} = ${body}\n`);
    fns.push(f);
  }
  const nVals = range(1, 3);
  for (let i = 0; i < nVals; i++) {
    const ty = pick(sums);
    out.push(`v${i} : ${tyStr(ty)}\nv${i} = ${genExpr(ty, [], fns, 3, null, 0)}\n`);
  }
  if (chance(0.3)) out.push(`ann : Bool\nann = (${genExpr(bool, [], fns, 2, null, 2)} : Bool)\n`);
  return out.join("\n");
}

function tyStr(t) {
  switch (t.k) {
    case "sum": return t.name;
    case "int": return "Int";
  }
}

// (an argument: parenthesized if it is more than a word)
function paren(s) {
  return /\s/.test(s) && !/^\(.*\)$/s.test(s) ? `(${s})` : s;
}

// an expression of type `ty`; `self` is the function being defined (a call
// of it is recursion); `ind` is the column a `case`'s alternatives must go
// past
function genExpr(ty, env, fns, depth, self, ind) {
  const opts = [];
  const vars = env.filter((v) => v.ty === ty);
  if (vars.length) opts.push(() => pick(vars).name);
  if (ty.k === "int") opts.push(() => String(range(0, 99)));
  if (ty.k === "sum") {
    const ctors = depth <= 0 ? ty.ctors.filter(([, as]) => as.length === 0) : ty.ctors;
    const cs = ctors.length ? ctors : ty.ctors;
    opts.push(() => {
      const [t, as] = pick(cs);
      if (as.length === 0) return `'${t}`;
      return `'${t} ` + as.map((a) => paren(genExpr(a, env, fns, depth - 1, self, ind))).join(" ");
    });
    if (depth > 0) {
      const scrutinees = env.filter((v) => v.ty.k === "sum");
      if (scrutinees.length) opts.push(() => {
        const s = pick(scrutinees);
        const col = ind + 2;
        const arms = s.ty.ctors.map(([t, as], i) => {
          const names = as.map((_, j) => `${s.name}_${i}_${j}`);
          const env2 = env.concat(as.map((a, j) => ({ name: names[j], ty: a })));
          const body = genExpr(ty, env2, fns, depth - 1, self, col);
          return " ".repeat(col) + `'${t}${names.map((n) => " " + n).join("")} -> ${body}`;
        });
        return `(case ${s.name} of\n${arms.join("\n")})`;
      });
    }
    const callable = fns.filter((f) => f.ret === ty);
    if (depth > 0 && callable.length) opts.push(() => {
      const f = pick(callable);
      return `${f.name} ` + f.params.map((p) => paren(genExpr(p, env, fns, depth - 1, self, ind))).join(" ");
    });
    if (depth > 0 && self && self.ret === ty && chance(0.5)) opts.push(() =>
      `${self.name} ` + self.params.map((p) => paren(genExpr(p, env, fns, depth - 1, null, ind))).join(" "));
    if (chance(0.05)) opts.push(() => "abandon");
  }
  if (!opts.length) return "abandon";
  return pick(opts)();
}
