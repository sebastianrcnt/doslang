; Ferro syntax highlighting for Zed.
; The extension intentionally reuses tree-sitter-rust for structural tokenization,
; then recolors Ferro-specific words with query predicates. This keeps the editor
; support tiny while the Ferro grammar is still evolving.

(line_comment) @comment
(block_comment) @comment

(string_literal) @string
(raw_string_literal) @string
(char_literal) @string
(escape_sequence) @string.escape

(integer_literal) @number
(float_literal) @number
(boolean_literal) @boolean

(primitive_type) @type.builtin
(type_identifier) @type
(field_identifier) @property

; Rust grammar already recognizes the Ferro keywords shared with Rust.
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

; Ferro-only keywords generally arrive as identifiers when parsed by the Rust
; grammar, so classify them by spelling. Capture both identifier shapes because
; error recovery may choose either one depending on context.
((identifier) @keyword
  (#match? @keyword "^(unit|import|packed|error|var|defer|critical|shared|atomic|comptime|asm|try|catch|orelse|interrupt|interrupt_safe|far|undefined|and|or|not)$"))

((type_identifier) @keyword
  (#match? @keyword "^(unit|import|packed|error|var|defer|critical|shared|atomic|comptime|asm|try|catch|orelse|interrupt|interrupt_safe|far|undefined|and|or|not)$"))

; `mut` is a Ferro keyword too; tree-sitter-rust exposes it as mutable_specifier
; in normal Rust positions and as an identifier during error recovery.
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

; Builtins are written as @name in Ferro. Reusing the Rust grammar means the @
; itself is recovered as punctuation/error, while the name remains available.
((identifier) @function.builtin
  (#match? @function.builtin "^(size_of|align_of|bits|target|ptr_cast|seg_ptr|port_in8|port_in16|port_out8|port_out16|volatile_load|volatile_store|trap|unreachable|line|file|print|fprint|sprint|compile_error|as_far_fn|call_far)$"))

; Useful structural coloring that survives even when the Rust parser is in
; recovery around Ferro-only syntax.
(call_expression
  function: (identifier) @function)

(call_expression
  function: (field_expression
    field: (field_identifier) @function.method))

(function_item
  name: (identifier) @function)

(parameter
  pattern: (identifier) @variable.parameter)

[
  "+" "-" "*" "/" "%"
  "=" "==" "!=" "<" ">" "<=" ">="
  "&" "|" "^" "!"
  "<<" ">>"
  "->" "=>"
] @operator
