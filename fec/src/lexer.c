#include "lexer.h"
#include <ctype.h>
#include <string.h>

typedef struct FeKw { const char *s; FeTokKind k; } FeKw;
static const FeKw keywords[] = {
    {"unit",FE_TOK_UNIT},{"import",FE_TOK_IMPORT},{"pub",FE_TOK_PUB},{"fn",FE_TOK_FN},
    {"struct",FE_TOK_STRUCT},{"enum",FE_TOK_ENUM},{"error",FE_TOK_ERROR_KW},{"const",FE_TOK_CONST},
    {"static",FE_TOK_STATIC},{"var",FE_TOK_VAR},{"let",FE_TOK_LET},{"mut",FE_TOK_MUT},
    {"if",FE_TOK_IF},{"else",FE_TOK_ELSE},{"while",FE_TOK_WHILE},{"for",FE_TOK_FOR},{"in",FE_TOK_IN},
    {"match",FE_TOK_MATCH},{"return",FE_TOK_RETURN},{"break",FE_TOK_BREAK},{"continue",FE_TOK_CONTINUE},
    {"defer",FE_TOK_DEFER},{"unsafe",FE_TOK_UNSAFE},{"comptime",FE_TOK_COMPTIME},{"asm",FE_TOK_ASM},
    {"try",FE_TOK_TRY},{"catch",FE_TOK_CATCH},{"as",FE_TOK_AS},{"extern",FE_TOK_EXTERN},
    {"interrupt",FE_TOK_INTERRUPT},{"interrupt_safe",FE_TOK_INTERRUPT_SAFE},{"far",FE_TOK_FAR},
    {"true",FE_TOK_TRUE},{"false",FE_TOK_FALSE},{"null",FE_TOK_NULL},{"undefined",FE_TOK_UNDEFINED},
    {"shared",FE_TOK_SHARED},{"atomic",FE_TOK_ATOMIC},{"critical",FE_TOK_CRITICAL},
    {"self",FE_TOK_SELF},{"Self",FE_TOK_SELFTYPE},{"type",FE_TOK_TYPE},
    {"packed",FE_TOK_PACKED},{"orelse",FE_TOK_ORELSE},{"and",FE_TOK_AND_KW},{"or",FE_TOK_OR_KW},{"not",FE_TOK_NOT},
    {0,FE_TOK_UNKNOWN}
};

static int at(FeLexer *l, unsigned long n, char c) { return l->pos + n < l->length && l->src[l->pos+n] == c; }
static FeLoc here(FeLexer *l, unsigned long line, unsigned long col)
{ FeLoc x; x.file=l->file; x.line=line; x.col=col; return x; }
static char cur(FeLexer *l) { return l->pos < l->length ? l->src[l->pos] : '\0'; }
static void advance(FeLexer *l)
{
    if (l->pos >= l->length) return;
    if (l->src[l->pos] == '\n') { l->line++; l->col = 1; }
    else l->col++;
    l->pos++;
}
static void skip_space(FeLexer *l)
{
    for (;;) {
        while (isspace((unsigned char)cur(l))) advance(l);
        if (at(l,0,'/') && at(l,1,'/')) {
            while (cur(l) && cur(l) != '\n') advance(l);
            continue;
        }
        if (at(l,0,'/') && at(l,1,'*')) {
            unsigned long depth = 0;
            advance(l); advance(l); depth = 1;
            while (depth && cur(l)) {
                if (at(l,0,'/') && at(l,1,'*')) { advance(l); advance(l); depth++; }
                else if (at(l,0,'*') && at(l,1,'/')) { advance(l); advance(l); depth--; }
                else advance(l);
            }
            if (depth) fe_diag_error(l->diags, here(l,l->line,l->col), "unterminated block comment");
            continue;
        }
        break;
    }
}

void fe_lexer_init(FeLexer *l, const char *src, unsigned long length, const char *file, FeDiags *d)
{
    l->src=src; l->length=length; l->pos=0; l->line=1; l->col=1; l->file=file; l->diags=d;
}

static FeTokKind keyword(const char *s, unsigned long n)
{
    unsigned long i;
    for (i=0; keywords[i].s; i++) {
        if (strlen(keywords[i].s)==n && memcmp(keywords[i].s,s,n)==0) return keywords[i].k;
    }
    return FE_TOK_IDENT;
}
static FeToken tok(FeLexer *l, FeTokKind k, unsigned long start, unsigned long line, unsigned long col)
{
    FeToken t; t.kind=k; t.begin=l->src+start; t.length=l->pos-start; t.loc.file=l->file; t.loc.line=line; t.loc.col=col; return t;
}
static int digit_for_base(char c, int base)
{
    int d;
    if (c >= '0' && c <= '9') d=c-'0';
    else if (c >= 'a' && c <= 'f') d=c-'a'+10;
    else if (c >= 'A' && c <= 'F') d=c-'A'+10;
    else return 0;
    return d < base;
}

FeToken fe_lexer_next(FeLexer *l)
{
    unsigned long start, line, col;
    char c;
    skip_space(l);
    start=l->pos; line=l->line; col=l->col; c=cur(l);
    if (!c) return tok(l,FE_TOK_EOF,start,line,col);
    if (isalpha((unsigned char)c) || c=='_') {
        advance(l);
        while (isalnum((unsigned char)cur(l)) || cur(l)=='_') advance(l);
        return tok(l,keyword(l->src+start,l->pos-start),start,line,col);
    }
    if (isdigit((unsigned char)c)) {
        int base=10, had_digit=0;
        if (c=='0' && (at(l,1,'x') || at(l,1,'X'))) { advance(l); advance(l); base=16; }
        else if (c=='0' && (at(l,1,'b') || at(l,1,'B'))) { advance(l); advance(l); base=2; }
        else if (c=='0' && (at(l,1,'o') || at(l,1,'O'))) { advance(l); advance(l); base=8; }
        while (cur(l)=='_' || digit_for_base(cur(l),base)) { if(cur(l)!='_') had_digit=1; advance(l); }
        if (!had_digit) fe_diag_error(l->diags,here(l,line,col),"integer literal has no digits");
        if (isalnum((unsigned char)cur(l))) {
            fe_diag_error(l->diags,here(l,line,col),"invalid digit in integer literal");
            while (isalnum((unsigned char)cur(l)) || cur(l)=='_') advance(l);
        }
        return tok(l,FE_TOK_INT,start,line,col);
    }
    if (c=='\'' || c=='"') {
        char quote=c; int bad=0, units=0; advance(l);
        while (cur(l) && cur(l)!=quote) {
            if (cur(l)=='\n' || cur(l)=='\r') { bad=1; break; }
            units++;
            if (cur(l)=='\\') {
                advance(l);
                if (!cur(l)) { bad=1; break; }
                if (cur(l)=='x') { int i; advance(l); for(i=0;i<2;i++) { if(!digit_for_base(cur(l),16)) bad=1; else advance(l); } }
                else if (cur(l)=='u') { int i; advance(l); for(i=0;i<4;i++) { if(!digit_for_base(cur(l),16)) bad=1; else advance(l); } }
                else if (strchr("nrt\\'\"0",cur(l))) advance(l);
                else { bad=1; advance(l); }
            } else advance(l);
        }
        if (cur(l)==quote) advance(l); else bad=1;
        if (quote=='\'' && units != 1) bad=1;
        if (bad) fe_diag_error(l->diags,here(l,line,col),quote=='\''?"invalid character literal":"unterminated or invalid string literal");
        return tok(l,quote=='\''?FE_TOK_CHAR:FE_TOK_STRING,start,line,col);
    }
    advance(l);
    switch(c) {
    case '(': return tok(l,FE_TOK_LPAREN,start,line,col); case ')': return tok(l,FE_TOK_RPAREN,start,line,col);
    case '{': return tok(l,FE_TOK_LBRACE,start,line,col); case '}': return tok(l,FE_TOK_RBRACE,start,line,col);
    case '[': return tok(l,FE_TOK_LBRACKET,start,line,col); case ']': return tok(l,FE_TOK_RBRACKET,start,line,col);
    case ',': return tok(l,FE_TOK_COMMA,start,line,col); case ';': return tok(l,FE_TOK_SEMI,start,line,col);
    case ':': return tok(l,FE_TOK_COLON,start,line,col); case '@': return tok(l,FE_TOK_AT,start,line,col);
    case '?': return tok(l,FE_TOK_QUESTION,start,line,col);
    case '.': if (cur(l)=='.') { advance(l); return tok(l,FE_TOK_DOTDOT,start,line,col); } return tok(l,FE_TOK_DOT,start,line,col);
    case '+': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_PLUS_EQ,start,line,col);} if(cur(l)=='%'){advance(l);return tok(l,FE_TOK_PLUS_WRAP,start,line,col);} return tok(l,FE_TOK_PLUS,start,line,col);
    case '-': if(cur(l)=='>'){advance(l);return tok(l,FE_TOK_ARROW,start,line,col);} if(cur(l)=='='){advance(l);return tok(l,FE_TOK_MINUS_EQ,start,line,col);} if(cur(l)=='%'){advance(l);return tok(l,FE_TOK_MINUS_WRAP,start,line,col);} return tok(l,FE_TOK_MINUS,start,line,col);
    case '*': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_STAR_EQ,start,line,col);} if(cur(l)=='%'){advance(l);return tok(l,FE_TOK_STAR_WRAP,start,line,col);} return tok(l,FE_TOK_STAR,start,line,col);
    case '/': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_SLASH_EQ,start,line,col);} return tok(l,FE_TOK_SLASH,start,line,col);
    case '%': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_PERCENT_EQ,start,line,col);} return tok(l,FE_TOK_PERCENT,start,line,col);
    case '=': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_EQEQ,start,line,col);} if(cur(l)=='>'){advance(l);return tok(l,FE_TOK_FATARROW,start,line,col);} return tok(l,FE_TOK_EQ,start,line,col);
    case '!': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_NE,start,line,col);} return tok(l,FE_TOK_BANG,start,line,col);
    case '<': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_LE,start,line,col);} if(cur(l)=='<'){advance(l);if(cur(l)=='='){advance(l);return tok(l,FE_TOK_SHL_EQ,start,line,col);}return tok(l,FE_TOK_SHL,start,line,col);} return tok(l,FE_TOK_LT,start,line,col);
    case '>': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_GE,start,line,col);} if(cur(l)=='>'){advance(l);if(cur(l)=='='){advance(l);return tok(l,FE_TOK_SHR_EQ,start,line,col);}return tok(l,FE_TOK_SHR,start,line,col);} return tok(l,FE_TOK_GT,start,line,col);
    case '&': if(cur(l)=='&'){advance(l);fe_diag_error(l->diags,here(l,line,col),"&& is not a Ferro logical operator; use 'and'");return tok(l,FE_TOK_UNKNOWN,start,line,col);} if(cur(l)=='='){advance(l);return tok(l,FE_TOK_AND_EQ,start,line,col);} return tok(l,FE_TOK_AND,start,line,col);
    case '|': if(cur(l)=='|'){advance(l);fe_diag_error(l->diags,here(l,line,col),"|| is not a Ferro logical operator; use 'or'");return tok(l,FE_TOK_UNKNOWN,start,line,col);} if(cur(l)=='='){advance(l);return tok(l,FE_TOK_OR_EQ,start,line,col);} return tok(l,FE_TOK_OR,start,line,col);
    case '^': if(cur(l)=='='){advance(l);return tok(l,FE_TOK_XOR_EQ,start,line,col);} return tok(l,FE_TOK_XOR,start,line,col);
    default: fe_diag_error(l->diags,here(l,line,col),"unknown character"); return tok(l,FE_TOK_UNKNOWN,start,line,col);
    }
}

const char *fe_token_name(FeTokKind k)
{
    switch(k) {
    case FE_TOK_EOF:return "eof"; case FE_TOK_IDENT:return "identifier"; case FE_TOK_INT:return "integer";
    case FE_TOK_CHAR:return "character"; case FE_TOK_STRING:return "string"; case FE_TOK_UNIT:return "unit";
    case FE_TOK_FN:return "fn"; case FE_TOK_STRUCT:return "struct"; case FE_TOK_ENUM:return "enum";
    case FE_TOK_ERROR_KW:return "error"; case FE_TOK_CONST:return "const"; case FE_TOK_LET:return "let";
    case FE_TOK_VAR:return "var"; case FE_TOK_IF:return "if"; case FE_TOK_ELSE:return "else";
    case FE_TOK_WHILE:return "while"; case FE_TOK_FOR:return "for"; case FE_TOK_MATCH:return "match";
    case FE_TOK_RETURN:return "return"; case FE_TOK_BREAK:return "break"; case FE_TOK_CONTINUE:return "continue";
    case FE_TOK_TRUE:return "true"; case FE_TOK_FALSE:return "false"; case FE_TOK_NULL:return "null";
    case FE_TOK_UNDEFINED:return "undefined"; case FE_TOK_AND_KW:return "and"; case FE_TOK_OR_KW:return "or";
    case FE_TOK_NOT:return "not"; case FE_TOK_BANG:return "!";
    case FE_TOK_LBRACE:return "{"; case FE_TOK_RBRACE:return "}"; case FE_TOK_LPAREN:return "("; case FE_TOK_RPAREN:return ")";
    case FE_TOK_SEMI:return ";"; case FE_TOK_COLON:return ":"; case FE_TOK_COMMA:return ",";
    case FE_TOK_EQ:return "="; case FE_TOK_ARROW:return "->"; case FE_TOK_FATARROW:return "=>";
    default:return "token";
    }
}
