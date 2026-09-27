// Random Hippogriff programs: type-directed, so most of them are well typed.
// Shared by the differential fuzzer and the compiler test.
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
// A type is { k: "sum", name, ctors: [[tag, [argTy...]]] } | { k: "int" }
// | { k: "fn", dom, cod } | { k: "rec", name, fields: [[l, ty]] }.
export function genProgram() {
  const out = [];
  const sums = [];
  const fns = []; // { name, params: [ty], ret: ty }
  const values = []; // { name, ty }
  const bool = { k: "sum", name: "Bool", ctors: [["true", []], ["false", []]] };
  out.push("def Bool : Type := sum\n  'true\n  'false\nend\n");
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
    out.push(`def ${name} : Type := sum\n` + self.ctors.map(([t, as]) => `  '${t}` + as.map((a) => " " + tyStr(a)).join("")).join("\n") + "\nend\n");
  }
  // a theory, and a struct of it
  let th = null;
  if (chance(0.7)) {
    const carrier = pick(sums);
    th = { name: "Th", carrier };
    out.push("theory Th := sig\n  t : Type\n  op : t -> t -> t\n  pick : t -> Bool\nend\n");
    const x = "x", y = "y";
    const env = [{ name: x, ty: carrier }, { name: y, ty: carrier }];
    out.push(`def Inst : Th := struct\n  t := ${carrier.name}\n  op := x => y => ${genExpr(carrier, env, fns, 2, null)}\n  pick := x => ${genExpr(bool, env.slice(0, 1), fns, 2, null)}\nend\n`);
    if (chance(0.5)) {
      out.push(`def Spec : Th ~ [ .t := ${carrier.name} ] := Inst\n`);
    }
    if (chance(0.6)) {
      out.push(`def useTh (M : Th) (a : M.t) : Bool := M.pick (M.op a a)\n`);
      out.push(`def usedTh : Bool := useTh Inst ${genExpr(carrier, [], fns, 2, null)}\n`);
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
    const body = genExpr(ret, env, fns, 3, f);
    out.push(`def ${f.name} ${params.map((t, p) => `(p${p} : ${tyStr(t)})`).join(" ")} : ${tyStr(ret)} := ${body}\n`);
    fns.push(f);
  }
  const nVals = range(1, 3);
  for (let i = 0; i < nVals; i++) {
    const ty = pick(sums);
    out.push(`def v${i} : ${tyStr(ty)} := ${genExpr(ty, [], fns, 3, null)}\n`);
    values.push({ name: `v${i}`, ty });
  }
  if (chance(0.3)) out.push(`def ann : Bool := (${genExpr(bool, [], fns, 2, null)} : Bool)\n`);
  return out.join("\n");
}

function tyStr(t) {
  switch (t.k) {
    case "sum": return t.name;
    case "int": return "Int";
    case "fn": return `(${tyStr(t.dom)} -> ${tyStr(t.cod)})`;
  }
}

function paren(s) {
  return /\s/.test(s) && !/^\(.*\)$/.test(s) ? `(${s})` : s;
}

// an expression of type `ty`; `self` is the function being defined, callable
// only under `rec`
function genExpr(ty, env, fns, depth, self) {
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
      return `'${t} ` + as.map((a) => paren(genExpr(a, env, fns, depth - 1, self))).join(" ");
    });
    if (depth > 0) {
      const scrutinees = env.filter((v) => v.ty.k === "sum");
      if (scrutinees.length) opts.push(() => {
        const s = pick(scrutinees);
        const arms = s.ty.ctors.map(([t, as], i) => {
          const names = as.map((_, j) => `${s.name}_${i}_${j}`);
          const env2 = env.concat(as.map((a, j) => ({ name: names[j], ty: a })));
          const body = genExpr(ty, env2, fns, depth - 1, self);
          return `  '${t}${names.map((n) => " " + n).join("")} => ${body}`;
        });
        return `match ${s.name}\n${arms.join("\n")}\nend`;
      });
    }
    const callable = fns.filter((f) => f.ret === ty);
    if (depth > 0 && callable.length) opts.push(() => {
      const f = pick(callable);
      return `${f.name} ` + f.params.map((p) => paren(genExpr(p, env, fns, depth - 1, self))).join(" ");
    });
    if (depth > 0 && self && self.ret === ty && chance(0.5)) opts.push(() =>
      `rec (${self.name} ` + self.params.map((p) => paren(genExpr(p, env, fns, depth - 1, null))).join(" ") + ")");
    if (chance(0.05)) opts.push(() => "abandon");
  }
  if (!opts.length) return "abandon";
  return pick(opts)();
}

