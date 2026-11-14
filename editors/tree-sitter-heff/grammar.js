/**
 * heff's tree-sitter grammar, for editors that colour with one (Zed).
 *
 * A file is lines of tokens, as VS Code's grammar sees it
 * (editors/vscode/syntaxes/heff.tmLanguage.json), not a program: what
 * heff's parser makes of them is the language server's to say.  A line
 * may open with a head, the one place a token is more than a token --
 * a signature's name, a declaration's, the module's header, a watch --
 * and brackets group what is in them, lines too.
 *
 * Comments, operators and the start of a watch are the scanner's
 * (src/scanner.c): block comments nest, and `--` begins a comment only
 * where it is not part of an operator.
 */

const SYMBOL_NAME = /[a-z_][A-Za-z0-9_']*(\/[A-Za-z][A-Za-z0-9_']*)*/;
const KEYWORDS = [
  "data", "type", "theory", "primitive", "instance", "mutual", "import", "as",
  "case", "of", "if", "then", "else", "do", "where", "once", "let", "mut", "in",
  "while", "for", "break", "continue", "return", "try", "catch", "with", "test",
  "abandon", "infixl", "infixr", "infix",
];

module.exports = grammar({
  name: "heff",

  externals: ($) => [
    $.comment,
    $.doc_comment,
    $.block_comment,
    $.watch_marker,
    $.doc_watch_marker,
    $.operator,
    $._string_content,
  ],

  extras: ($) => [/[ \t\f\r]/, $.comment, $.doc_comment, $.block_comment],

  word: ($) => $.name,

  conflicts: ($) => [[$.operator_name, $._item], [$.signature, $.keyword], [$.signature, $._item]],

  rules: {
    source_file: ($) =>
      seq(
        optional($._line),
        repeat(choice(seq($._newline, optional($._line)), seq($._indent, optional($._indented_line)))),
      ),

    // a line's end, and (`_indent`) the next one's indentation: a signature
    // on an indented line is a `where`'s, or a `let`'s
    _newline: (_) => /\n/,
    _indent: (_) => /\n[ \t]+/,

    _indented_line: ($) =>
      choice(
        seq(choice(alias($.signature, $.local_signature), $.watch), repeat($._item)),
        prec(-1, repeat1($._item)),
      ),

    _line: ($) =>
      choice(
        seq($._head, repeat($._item)),
        prec(-1, repeat1($._item)),
      ),

    _head: ($) => choice($.signature, $.declaration, $.module_header, $.import, $.watch),

    // `x : A`, `(::) : A`, `primitive f : A`, `instance i : A`, at the start
    // of a line (a `where`'s too), not `x :: xs`
    signature: ($) =>
      prec.dynamic(
        1,
        seq(
          optional(field("keyword", choice("primitive", "instance"))),
          field("name", choice($.name, $.operator_name)),
          ":",
        ),
      ),

    operator_name: ($) => seq("(", $.operator, ")"),

    // `data T`, `type T`, `theory T`: the type it makes
    declaration: ($) => prec(1, seq(choice("data", "type", "theory"), field("name", $.constructor))),

    // `module m`, `module (x, T(..))`
    module_header: (_) => "module",

    import: ($) => prec(1, seq("import", field("path", $._path))),

    _path: ($) => prec.right(seq($.name, repeat($.field))),

    // `-- >>> e` and, in a doc comment, `-- | >>> e`: the rest of the
    // line an expression
    watch: ($) => choice($.watch_marker, $.doc_watch_marker),

    _item: ($) =>
      choice(
        $.name,
        $.constructor,
        $.field,
        $.keyword,
        $.integer,
        $.float,
        $.string,
        $.rune,
        $.operator,
        ":",
        ",",
        ";",
        "`",
        $.parens,
        $.brackets,
        $.binary,
        $.braces,
      ),

    keyword: (_) => choice(...KEYWORDS),

    _inside: ($) => repeat1(choice($._item, $._newline, $._indent)),

    parens: ($) => seq("(", optional($._inside), ")"),
    brackets: ($) => seq("[", optional($._inside), "]"),
    // `b[1, 2]`: a binary
    binary: ($) => seq("b[", optional($._inside), "]"),
    braces: ($) => seq("{", optional($._inside), "}"),

    name: (_) => SYMBOL_NAME,
    constructor: (_) => /[A-Z][A-Za-z0-9_']*(\/[A-Za-z][A-Za-z0-9_']*)*/,
    // a field: right after what it projects (`r.x`, `List.map`)
    field: (_) => /\.[A-Za-z][A-Za-z0-9_']*/,

    integer: (_) =>
      token(
        choice(
          /0[xX][0-9A-Fa-f](_?[0-9A-Fa-f])*/,
          /0[oO][0-7](_?[0-7])*/,
          /0[bB][01](_?[01])*/,
          /[0-9](_?[0-9])*/,
        ),
      ),
    float: (_) => /[0-9](_?[0-9])*\.[0-9](_?[0-9])*([eE][+-]?[0-9](_?[0-9])*)?/,

    string: ($) =>
      seq(
        '"',
        repeat(choice($._string_content, $.escape)),
        token.immediate('"'),
      ),
    escape: (_) => token.immediate(/\\(u\{[0-9A-Fa-f]+\}|x[0-9A-Fa-f]{2}|.)/),

    rune: (_) => /'([^'\\\n]|\\u\{[0-9A-Fa-f]+\}|\\x[0-9A-Fa-f]{2}|\\.)'/,
  },
});
