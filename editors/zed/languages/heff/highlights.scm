; Later patterns win over earlier ones.

(name) @variable
(field) @property

((constructor) @type
  (#not-match? @type "^(Type|Int|Float64|String|Rune|Array|Int8|Int16|Int32|Int64|UInt8|UInt16|UInt32|UInt64|Float32)$"))
((constructor) @type.builtin
  (#match? @type.builtin "^(Type|Int|Float64|String|Rune|Array|Int8|Int16|Int32|Int64|UInt8|UInt16|UInt32|UInt64|Float32)$"))

(signature name: (name) @function)
(signature name: (operator_name (operator) @function))
(local_signature name: (name) @function)
(local_signature name: (operator_name (operator) @function))
(declaration name: (constructor) @type)

(keyword) @keyword
(module_header) @keyword
[
  "primitive"
  "instance"
  "data"
  "type"
  "theory"
  "import"
] @keyword

(watch) @keyword

(integer) @number
(float) @number
(string) @string
(escape) @string.escape
(rune) @string.special

(comment) @comment
(block_comment) @comment
(doc_comment) @comment.doc

(operator) @operator
":" @operator

["," ";"] @punctuation.delimiter
["(" ")" "[" "]" "b[" "{" "}"] @punctuation.bracket
