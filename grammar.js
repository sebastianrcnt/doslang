// Minimal Tree-sitter grammar for Ferro syntax highlighting in Zed.
// This intentionally models lexical structure rather than full Ferro semantics.

module.exports = grammar({
  name: "ferro",

  extras: $ => [
    /[\s\uFEFF\u2060\u200B]/,
  ],

  rules: {
    source_file: $ => repeat(choice(
      $.line_comment,
      $.block_comment,
      $.string_literal,
      $.char_literal,
      $.integer_literal,
      $.builtin,
      $.builtin_type,
      $.keyword,
      $.identifier,
      $.operator,
      $.punctuation,
    )),

    line_comment: _ => token(seq("//", /[^\n]*/)),

    // Ferro block comments may nest. Keeping this rule recursive makes syntax
    // highlighting follow the compiler lexer instead of flattening nested /* */.
    block_comment: $ => seq(
      "/*",
      repeat(choice(
        $.block_comment,
        /[^*/]+/,
        /\*[^/]/,
        /\/[^*]/,
      )),
      "*/",
    ),

    string_literal: _ => token(seq(
      '"',
      repeat(choice(
        /[^"\\\n\r]/,
        /\\(x[0-9A-Fa-f]{2}|u[0-9A-Fa-f]{4}|[nrt\\'"0])/,
      )),
      '"',
    )),

    char_literal: _ => token(seq(
      "'",
      choice(
        /[^'\\\n\r]/,
        /\\(x[0-9A-Fa-f]{2}|u[0-9A-Fa-f]{4}|[nrt\\'"0])/,
      ),
      "'",
    )),

    integer_literal: _ => token(prec(2, choice(
      /0[xX][0-9A-Fa-f_]+/,
      /0[bB][01_]+/,
      /0[oO][0-7_]+/,
      /[0-9][0-9_]*/,
    ))),

    builtin: _ => token(prec(3, /@[A-Za-z_][A-Za-z0-9_]*/)),

    builtin_type: _ => token(prec(3, choice(
      "i8", "i16", "i32",
      "u8", "u16", "u32",
      "usize", "isize",
      "bool", "char", "void", "str",
    ))),

    keyword: _ => token(prec(3, choice(
      "unit", "import", "pub", "fn", "struct", "packed", "enum", "error",
      "const", "static", "var", "let", "mut",
      "if", "else", "while", "for", "in", "match",
      "return", "break", "continue", "defer",
      "unsafe", "critical", "shared", "atomic", "comptime", "asm",
      "try", "catch", "orelse", "as", "extern",
      "interrupt", "interrupt_safe", "far",
      "true", "false", "null", "undefined",
      "self", "Self", "type",
      "and", "or", "not",
    ))),

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    operator: _ => token(choice(
      "<<=", ">>=",
      "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
      "==", "!=", "<=", ">=", "<<", ">>", "->", "=>", "..",
      "+%", "-%", "*%",
      "+", "-", "*", "/", "%", "=", "<", ">", "&", "|", "^", "~", "!", "?",
    )),

    punctuation: _ => token(choice(
      "(", ")", "{", "}", "[", "]", ",", ";", ":", ".",
    )),
  },
});
