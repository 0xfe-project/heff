#!/usr/bin/env node
// bat's heff, editors/bat/heff.sublime-syntax, made from VS Code's,
// editors/vscode/syntaxes/heff.tmLanguage.json: the TextMate grammar as a
// Sublime syntax, which bat (syntect) reads.  Run again after changing the
// grammar; with `--check` it writes nothing, and fails if what it would
// write is not what is there.
//
// A rule with `match` stays one; one with `begin` and `end` pushes a
// context of its own, `NAME-body`, whose `name` and `contentName` are its
// meta_scope and meta_content_scope, `end` popping it -- tried first, as
// TextMate tries it, unless `applyEndPatternLast`; a rule of `patterns`
// only is its patterns.  Each of the repository's rules is a context of
// that name, and the grammar's own patterns are `main`.
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const from = join(here, "..", "vscode", "syntaxes", "heff.tmLanguage.json");
const to = join(here, "heff.sublime-syntax");
const fail = (msg) => { console.error(`editors/bat/build.mjs: ${msg}`); process.exit(1); };
const tm = JSON.parse(readFileSync(from, "utf8"));

// (a regex as it is, but for what a Sublime syntax reads otherwise:
// `{{NAME}}` is a variable there, and `end` cannot refer to `begin`'s groups)
const re = (s, isEnd = false) => {
  if (/\{\{[A-Za-z0-9_]+\}\}/.test(s)) fail(`${s}: {{NAME}} would be a variable`);
  if (isEnd && /\\[1-9]/.test(s)) fail(`${s}: an end referring to its begin's groups`);
  return s;
};
const include = (inc) =>
  inc === "$self" || inc === "$base" ? "main"
  : inc.startsWith("#") ? inc.slice(1)
  : fail(`an include of another grammar, ${inc}`);
const captures = (cs) => cs && Object.fromEntries(Object.entries(cs).map(([k, c]) =>
  c.patterns ? fail(`patterns within a capture (${k})`) : [k, c.name]).filter(([, n]) => n));

// name -> { meta: [[key, scope]], rules }, in the order they are first named
const contexts = new Map();
const define = (name, meta, make) => {
  if (contexts.has(name)) fail(`two contexts named ${name}`);
  contexts.set(name, null);
  contexts.set(name, { meta: meta.filter(([, s]) => s), rules: make() });
};

// (a rule, as the rules of the context `ctx`)
const rules = (rule, ctx) => {
  if (rule.include) return [{ include: include(rule.include) }];
  if (rule.match) {
    return [{ comment: rule.comment, match: re(rule.match), scope: rule.name, captures: captures(rule.captures) }];
  }
  if (rule.begin) {
    if (rule.while) fail(`${ctx}: a begin/while rule`);
    if (rule.end === undefined) fail(`${ctx}: a begin without an end`);
    const body = `${ctx}-body`;
    define(body, [["meta_scope", rule.name], ["meta_content_scope", rule.contentName]], () => {
      const end = { match: re(rule.end, true), captures: captures(rule.endCaptures ?? rule.captures), pop: true };
      const inner = (rule.patterns ?? []).flatMap((r, i) => rules(r, `${body}-${i}`));
      return rule.applyEndPatternLast ? [...inner, end] : [end, ...inner];
    });
    return [{ comment: rule.comment, match: re(rule.begin), captures: captures(rule.beginCaptures ?? rule.captures), push: body }];
  }
  if (rule.patterns) return rule.patterns.flatMap((r, i) => rules(r, `${ctx}-${i}`));
  fail(`${ctx}: a rule of none of match, begin, patterns, include`);
};

define("main", [], () => tm.patterns.flatMap((r, i) => rules(r, `main-${i}`)));
for (const [name, rule] of Object.entries(tm.repository ?? {})) define(name, [], () => rules(rule, name));

// (YAML: a scope as it is, a regex quoted)
const quote = (s) => `'${s.replaceAll("'", "''")}'`;
const scope = (s) => /^[A-Za-z][\w.-]*( [A-Za-z][\w.-]*)*$/.test(s) ? s : quote(s);
const out = [
  "%YAML 1.2",
  "---",
  "# Made from the VS Code extension's grammar: change that, and make",
  "# this again (make bat).",
  `name: ${scope(tm.name)}`,
  `scope: ${scope(tm.scopeName)}`,
  "file_extensions:",
  ...tm.fileTypes.map((e) => `  - ${e}`),
  ...(tm.firstLineMatch ? [`first_line_match: ${quote(re(tm.firstLineMatch))}`] : []),
  "contexts:",
];
for (const [name, { meta, rules }] of contexts) {
  out.push(`  ${name}:`);
  for (const [key, s] of meta) out.push(`    - ${key}: ${scope(s)}`);
  for (const r of rules) {
    if (r.comment) out.push(...r.comment.split("\n").map((l) => `    # ${l}`));
    if (r.include) { out.push(`    - include: ${r.include}`); continue; }
    out.push(`    - match: ${quote(r.match)}`);
    if (r.scope) out.push(`      scope: ${scope(r.scope)}`);
    if (r.captures && Object.keys(r.captures).length) {
      out.push("      captures:");
      for (const [k, s] of Object.entries(r.captures)) out.push(`        ${k}: ${scope(s)}`);
    }
    if (r.push) out.push(`      push: ${r.push}`);
    if (r.pop) out.push("      pop: true");
  }
}
const text = out.join("\n") + "\n";

if (process.argv.includes("--check")) {
  if (!existsSync(to) || readFileSync(to, "utf8") !== text) {
    fail("heff.sublime-syntax is not made from the grammar as it is: run `node editors/bat/build.mjs`");
  }
} else writeFileSync(to, text);
