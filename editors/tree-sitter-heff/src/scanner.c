// The tokens of heff's a regular expression cannot say: comments (block
// comments nest, and `--` begins one only where it is not an operator's),
// the start of a watch, and operators (which end before a `--`).  As
// src/heff/syntax/lex.hf has them; editors/vscode's grammar says the same.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
  COMMENT,
  DOC_COMMENT,
  BLOCK_COMMENT,
  WATCH_MARKER,
  DOC_WATCH_MARKER,
  OPERATOR,
  STRING_CONTENT,
};

void *tree_sitter_heff_external_scanner_create(void) { return NULL; }
void tree_sitter_heff_external_scanner_destroy(void *payload) {}
unsigned tree_sitter_heff_external_scanner_serialize(void *payload, char *buffer) { return 0; }
void tree_sitter_heff_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {}

static bool is_symbol(int32_t c) {
  switch (c) {
  case '!': case '#': case '$': case '%': case '&': case '*': case '+':
  case '.': case '/': case '<': case '=': case '>': case '?': case '@':
  case '\\': case '^': case '|': case '-': case '~': case ':':
    return true;
  default:
    return false;
  }
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void to_line_end(TSLexer *lexer) {
  while (!lexer->eof(lexer) && lexer->lookahead != '\n') advance(lexer);
  lexer->mark_end(lexer);
}

// after `{-`: up to its `-}`, the ones within it too
static bool block_comment(TSLexer *lexer) {
  unsigned depth = 1;
  while (!lexer->eof(lexer)) {
    if (lexer->lookahead == '{') {
      advance(lexer);
      if (lexer->lookahead == '-') { advance(lexer); depth++; }
    } else if (lexer->lookahead == '-') {
      advance(lexer);
      if (lexer->lookahead == '}') {
        advance(lexer);
        if (--depth == 0) break;
      }
    } else {
      advance(lexer);
    }
  }
  lexer->mark_end(lexer);
  lexer->result_symbol = BLOCK_COMMENT;
  return true;
}

// after the dashes of a comment, `-- `: is `>>>` next?
static bool watch_follows(TSLexer *lexer) {
  for (int i = 0; i < 3; i++) {
    if (lexer->lookahead != '>') return false;
    advance(lexer);
  }
  return true;
}

// after `--` and more dashes, none of the operators' symbols next: a
// comment, a doc comment (`-- |`), or a watch (`-- >>>`, `-- | >>>`) where
// one can be
static bool line_comment(TSLexer *lexer, unsigned dashes, const bool *valid) {
  lexer->mark_end(lexer);
  lexer->result_symbol = COMMENT;
  if (lexer->lookahead != ' ') { to_line_end(lexer); return true; }
  advance(lexer);
  if (dashes == 2 && lexer->lookahead == '>') {
    if (watch_follows(lexer) && valid[WATCH_MARKER]) {
      lexer->mark_end(lexer);
      lexer->result_symbol = WATCH_MARKER;
      return true;
    }
    to_line_end(lexer);
    return true;
  }
  if (lexer->lookahead == '|') {
    advance(lexer);
    lexer->result_symbol = DOC_COMMENT;
    if (dashes == 2 && lexer->lookahead == ' ') {
      advance(lexer);
      if (lexer->lookahead == '>' && watch_follows(lexer) && valid[DOC_WATCH_MARKER]) {
        lexer->mark_end(lexer);
        lexer->result_symbol = DOC_WATCH_MARKER;
        return true;
      }
    }
  }
  to_line_end(lexer);
  return true;
}

bool tree_sitter_heff_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid) {
  // within a string (not where any token can be, as when recovering from an
  // error): up to its `"`, an escape, or the line's end -- no comment there
  if (valid[STRING_CONTENT] && !valid[OPERATOR]) {
    bool some = false;
    while (!lexer->eof(lexer) && lexer->lookahead != '"' && lexer->lookahead != '\\' && lexer->lookahead != '\n') {
      advance(lexer);
      some = true;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = STRING_CONTENT;
    return some;
  }

  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' || lexer->lookahead == '\f')
    lexer->advance(lexer, true);

  if (lexer->lookahead == '{') {
    advance(lexer);
    if (lexer->lookahead != '-') return false;
    advance(lexer);
    return block_comment(lexer);
  }

  if (!is_symbol(lexer->lookahead)) return false;

  // `--`, more dashes, and no other symbol: a comment
  if (lexer->lookahead == '-') {
    unsigned dashes = 0;
    while (lexer->lookahead == '-') {
      advance(lexer);
      dashes++;
      if (dashes == 1) lexer->mark_end(lexer);
    }
    if (dashes >= 2 && !is_symbol(lexer->lookahead))
      return line_comment(lexer, dashes, valid);
    if (!valid[OPERATOR]) return false;
    // an operator: its first `-`, then (`-->`) one more when a symbol
    // other than a dash follows
    if (dashes > 2) {
      // `--->`: `-`, then `-->`
      lexer->result_symbol = OPERATOR;
      return true;
    }
    if (dashes == 2) lexer->mark_end(lexer);
  } else {
    if (!valid[OPERATOR]) return false;
    int32_t first = lexer->lookahead;
    advance(lexer);
    lexer->mark_end(lexer);
    // `:` alone is the grammar's, and `.x` a field
    if (first == ':' && !is_symbol(lexer->lookahead)) return false;
    if (first == '.' && !is_symbol(lexer->lookahead)) {
      int32_t c = lexer->lookahead;
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return false;
    }
  }

  // the rest: symbols, a `-` only when no `-` follows it
  for (;;) {
    int32_t c = lexer->lookahead;
    if (!is_symbol(c)) break;
    if (c == '-') {
      advance(lexer);
      if (lexer->lookahead == '-') break;
    } else {
      advance(lexer);
    }
    lexer->mark_end(lexer);
  }
  lexer->result_symbol = OPERATOR;
  return true;
}
