// The lexer of the Haskell-like syntax.  Every token knows its column and
// whether it is the first on its line: that is all the layout rule needs.
//
//   var    x, map, Bwd/elemAt (a `/` inside a name, before a letter), x'
//   con    Cons, True         (a name with a capital)
//   tag    'cons              (a tag, written as such)
//   field  .x                 (right after what it projects, no space)
//   int    42, -3             (`-` right before a digit, not after an operand)
//   float  1.5, -0.25, 6.02e23 (digits on both sides of the `.`)
//   str    "…"
//   sym    ->, ::, =, <-, \, |, :=, ~, and any other run of symbol characters
//   punct  ( ) [ ] { } ,
//   kw     data type theory foreign import case of if then else do where
//          let in test sum Type Int Float String Array infixl infixr infix
//   watch  `-- >>>`, at the start of a doctest line (the rest of the line is
//          an expression)
//
// Comments are not tokens; they are returned apart, for the formatter.
const keywords = new Set(["data", "type", "theory", "foreign", "import", "case", "of", "if", "then", "else",
  "do", "where", "let", "in", "test", "sum", "Type", "Int", "Float", "String", "Array", "abandon", "infixl", "infixr", "infix"]);
const symChars = new Set("!#$%&*+./<=>?@\\^|-~:");
const isLower = (c) => (c >= "a" && c <= "z") || c === "_";
const isUpper = (c) => c >= "A" && c <= "Z";
const isLetter = (c) => isLower(c) || isUpper(c);
const isDigit = (c) => c >= "0" && c <= "9";
const isNameChar = (c) => isLetter(c) || isDigit(c) || c === "'";

export const lexImpl = (src) => {
  const tokens = [], errors = [], comments = [];
  let i = 0, line = 0, lineStart = 0, first = true;
  const push = (kind, text, start, end) => {
    tokens.push({ kind, text, start, end, line, col: start - lineStart, first });
    first = false;
  };
  // the previous token ends where this one starts, and is an operand
  const operandBefore = (start) => {
    const t = tokens[tokens.length - 1];
    if (!t || t.end !== start) return false;
    return t.kind === "var" || t.kind === "con" || t.kind === "int" || t.kind === "float" || t.kind === "str" || t.kind === "field" ||
      (t.kind === "punct" && (t.text === ")" || t.text === "]" || t.text === "}"));
  };
  while (i < src.length) {
    const c = src[i];
    if (c === "\n") { i++; line++; lineStart = i; first = true; continue; }
    if (c === " " || c === "\t" || c === "\r") { i++; continue; }
    // comments: two dashes or more, not part of an operator (`---` is one,
    // `-->` is not); a doctest line `-- >>> e` is a watch
    let dashes = i;
    while (src[dashes] === "-") dashes++;
    if (c === "-" && dashes - i >= 2 && !symChars.has(src[dashes] ?? " ") || (c === "-" && src.startsWith("-- >>>", i))) {
      if (src.startsWith("-- >>>", i)) { push("watch", "-- >>>", i, i + 6); i += 6; continue; }
      const s = i;
      while (i < src.length && src[i] !== "\n") i++;
      comments.push({ start: s, end: i, text: src.slice(s, i) });
      continue;
    }
    if (c === "{" && src[i + 1] === "-") {
      let depth = 0;
      const s = i;
      while (i < src.length) {
        if (src[i] === "{" && src[i + 1] === "-") { depth++; i += 2; continue; }
        if (src[i] === "-" && src[i + 1] === "}") { depth--; i += 2; if (depth === 0) break; continue; }
        if (src[i] === "\n") { line++; lineStart = i + 1; first = true; }
        i++;
      }
      if (depth > 0) errors.push({ start: s, end: i, msg: "a `{-` comment is never closed" });
      comments.push({ start: s, end: i, text: src.slice(s, i) });
      continue;
    }
    const s = i;
    if (isLetter(c)) {
      while (i < src.length && (isNameChar(src[i]) || (src[i] === "/" && isLetter(src[i + 1] ?? "")))) i++;
      const text = src.slice(s, i);
      push(keywords.has(text) ? "kw" : isUpper(c) ? "con" : "var", text, s, i);
      continue;
    }
    if (c === "'" && isLetter(src[i + 1] ?? "")) {
      i++;
      while (i < src.length && (isNameChar(src[i]) || src[i] === "-")) i++;
      push("tag", src.slice(s + 1, i), s, i);
      continue;
    }
    if (c === "." && isLetter(src[i + 1] ?? "") && operandBefore(s)) {
      i++;
      while (i < src.length && isNameChar(src[i])) i++;
      push("field", src.slice(s + 1, i), s, i);
      continue;
    }
    if (isDigit(c) || (c === "-" && isDigit(src[i + 1] ?? "") && !operandBefore(s))) {
      i++;
      while (i < src.length && isDigit(src[i])) i++;
      let kind = "int";
      if (src[i] === "." && isDigit(src[i + 1] ?? "")) {
        kind = "float";
        i++;
        while (i < src.length && isDigit(src[i])) i++;
      }
      if (kind === "float" && (src[i] === "e" || src[i] === "E") && (isDigit(src[i + 1] ?? "") || ((src[i + 1] === "-" || src[i + 1] === "+") && isDigit(src[i + 2] ?? "")))) {
        i += 2;
        while (i < src.length && isDigit(src[i])) i++;
      }
      push(kind, src.slice(s, i), s, i);
      continue;
    }
    if (c === '"') {
      i++;
      let text = "";
      while (i < src.length && src[i] !== '"' && src[i] !== "\n") {
        if (src[i] === "\\" && i + 1 < src.length) {
          const e = src[i + 1];
          text += e === "n" ? "\n" : e === "t" ? "\t" : e === "r" ? "\r" : e;
          i += 2;
        } else text += src[i++];
      }
      if (src[i] !== '"') errors.push({ start: s, end: i, msg: "a string is never closed" });
      else i++;
      push("str", text, s, i);
      continue;
    }
    if ("()[]{},".includes(c)) { i++; push("punct", c, s, i); continue; }
    if (symChars.has(c)) {
      // (at least one character: a symbol never stops the lexer)
      do i++; while (i < src.length && symChars.has(src[i]) && !(src[i] === "-" && src[i + 1] === "-"));
      push("sym", src.slice(s, i), s, i);
      continue;
    }
    errors.push({ start: i, end: i + 1, msg: "unexpected character " + JSON.stringify(c) });
    i++;
  }
  tokens.push({ kind: "eof", text: "", start: src.length, end: src.length, line: line + 1, col: 0, first: true });
  return { tokens, errors, comments };
};
