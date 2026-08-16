; Ferro syntax highlighting for Zed.
; Reuse tree-sitter-rust for tokenization and recolor Ferro-only words with
; query predicates. This intentionally stays small: highlighting only, no LSP.

(line_comment) @comment
(block_comment) @comment

(string_literal) @string
(raw_string_literal) @string
(char_literal) @string
(escape_sequence) @escape

(integer_literal) @number
(float_literal) @number
(boolean_literal) @constant.builtin

(primitive_type) @type.builtin
(type_identifier) @type
(field_identifier) @property

; Keywords shared by Ferro and Rust are recognized structurally by the reused
; grammar.
[
  "as"
  "break"
  "const"
  "continue"
  "else"
  "enum"
  "extern"
  "fn"
  "for"
  "if"
  "in"
  "let"
  "match"
  "pub"
  "return"
  "static"
  "struct"
  "type"
  "unsafe"
  "while"
] @keyword

(mutable_specifier) @keyword
(self) @variable.builtin

; Ferro-only keywords generally arrive as identifiers while the Rust grammar is
; recovering, so classify them by spelling. Include both identifier shapes since
; recovery can choose either depending on context.
((identifier) @keyword
  (#match? @keyword "^(unit|import|packed|error|var|defer|critical|shared|atomic|comptime|asm|try|catch|orelse|interrupt|interrupt_safe|far|and|or|not)$"))

((type_identifier) @keyword
  (#match? @keyword "^(unit|import|packed|error|var|defer|critical|shared|atomic|comptime|asm|try|catch|orelse|interrupt|interrupt_safe|far|and|or|not)$"))

((identifier) @keyword
  (#eq? @keyword "mut"))

; Ferro literals not understood as Rust literals.
((identifier) @constant.builtin
  (#match? @constant.builtin "^(null|undefined)$"))

; Ferro's `void` is not a Rust primitive type.
((identifier) @type.builtin
  (#eq? @type.builtin "void"))
((type_identifier) @type.builtin
  (#eq? @type.builtin "void"))

; Builtins are written as @name in Ferro. With the reused grammar, the @ is
; recovered separately while the builtin name remains queryable.
((identifier) @function.builtin
  (#match? @function.builtin "^(size_of|align_of|bits|target|ptr_cast|seg_ptr|port_in8|port_in16|port_out8|port_out16|volatile_load|volatile_store|trap|unreachable|line|file|print|fprint|sprint|compile_error|as_far_fn|call_far)$"))

; Structural coloring copied from node shapes known to exist in tree-sitter-rust.
(call_expression
  function: (identifier) @function)

(call_expression
  function: (field_expression
    field: (field_identifier) @function.method))

(function_item (identifier) @function)
(parameter (identifier) @variable.parameter)

[
  "+" "-" "*" "/" "%"
  "=" "==" "!=" "<" ">" "<=" ">="
  "&" "|" "^" "!"
  "<<" ">>"
  "->" "=>"
] @operator
