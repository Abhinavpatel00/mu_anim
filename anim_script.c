/*
 * anim_script.c — Compact pipe-oriented DSL (.anim) interpreter for mu_anim.
 * ============================================================================
 *  Compiles .anim scripts into AnimScene mobjects, ValueTracker + topological
 *  updater DAGs (`:=` reactive bindings), and seekable AnimTimeline steps.
 */
#define _POSIX_C_SOURCE 200809L
#include "anim_script.h"
#include "anim_internal.h"

#include <ctype.h>

/* ------------------------------------------------------------------ limits */

enum {
    SC_MAX_PARAMS   = 8,
    SC_MAX_ARGS     = 24,
    SC_MAX_SPECS    = 256,
    SC_MAX_ERR      = 512
};

/* ------------------------------------------------------------------- lexer */

typedef enum TokKind {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_NUM,
    TOK_HEX,
    TOK_STR,
    TOK_RAW_STR,
    TOK_TEX_LIT,
    TOK_IDENT,

    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_CARET,
    TOK_BAR, TOK_AMP, TOK_TILDE,
    TOK_BANG,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_AND, TOK_OR,
    TOK_ASSIGN,     /* =  */
    TOK_REACT,      /* := */
    TOK_ARROW,      /* -> */
    TOK_FAT_ARROW,  /* => */
    TOK_DOTDOT,     /* .. */

    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACK, TOK_RBRACK,
    TOK_COMMA, TOK_COLON, TOK_SEMI, TOK_DOT,

    /* Keywords */
    KW_SCENE,
    KW_PLAY, KW_LAGGED, KW_SEQUENCE, KW_AT, KW_WAIT,
    KW_FN, KW_RETURN, KW_FOR, KW_IN, KW_IF, KW_ELSE,
    KW_TRUE, KW_FALSE, KW_DEFAULT, KW_NULL
} TokKind;

typedef struct Token {
    TokKind     kind;
    const char *text; /* interned slice in sc->str_pool */
    size_t      len;
    double      num;
    uint32_t    hex_rgb;
    uint8_t     hex_alpha;
    bool        hex_has_alpha;
    int         line;
    int         col;
} Token;

/* --------------------------------------------------------------- AST nodes */

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef enum ExprKind {
    EX_NUM = 0,
    EX_BOOL,
    EX_STR,
    EX_TEX,
    EX_HEX,
    EX_IDENT,
    EX_LIST,
    EX_VEC,
    EX_RANGE,
    EX_BAG,
    EX_LAMBDA,
    EX_UNARY,
    EX_BINARY,
    EX_INDEX,
    EX_FIELD,
    EX_CALL,
    EX_PIPE,
    EX_PAIR /* a -> b inside bags like bg: #000 -> #111 */
} ExprKind;

typedef struct BagField {
    const char *key;
    Expr       *val;
} BagField;

struct Expr {
    ExprKind kind;
    int      line;
    union {
        struct { double val; } num;
        struct { bool val; } boolean;
        struct { const char *str; } str;
        struct { uint32_t rgb; uint8_t alpha; bool has_alpha; } hex;
        struct { const char *name; } ident;
        struct { Expr **items; uint32_t count; } list;
        struct { Expr *lo; Expr *hi; Expr *step; } range;
        struct { BagField *fields; uint32_t count; } bag;
        struct { const char **params; uint32_t param_count; Expr *body; } lambda;
        struct { TokKind op; Expr *rhs; } unary;
        struct { TokKind op; Expr *lhs; Expr *rhs; } binary;
        struct { Expr *obj; Expr *idx; } index;
        struct { Expr *obj; const char *field; } field;
        struct { Expr *callee; Expr **args; const char **arg_names; uint32_t arg_count; } call;
        struct { Expr *lhs; const char *fn_name; Expr **args; const char **arg_names; uint32_t arg_count; } pipe;
    } u;
};

typedef enum StmtKind {
    ST_EXPR = 0,
    ST_ASSIGN,
    ST_REACT,
    ST_SCENE,
    ST_BLOCK,
    ST_IF,
    ST_FOR_LIST,
    ST_FN,
    ST_RETURN,
    ST_PLAY,
    ST_WAIT,
    ST_ARROW_CUE /* inside play: lhs -> rhs */
} StmtKind;

struct Stmt {
    StmtKind kind;
    int      line;
    union {
        struct { Expr *expr; } expr;
        struct { const char *name; Expr *rhs; } assign;
        struct { BagField *fields; uint32_t count; } scene;
        struct { Stmt **items; uint32_t count; } block;
        struct { Expr *cond; Stmt *then_b; Stmt *else_b; } ifs;
        struct { const char *var; Expr *list; Stmt *body; } for_l;
        struct { const char *name; const char **params; uint32_t param_count; Stmt *body; } fn;
        struct { Expr *expr; } ret;
        struct {
            int      mode; /* 0=play, 1=lagged, 2=sequence, 3=at */
            Expr    *t0;   /* start time for at() */
            Expr    *dur;
            Expr    *lag;
            Expr    *rate;
            Stmt    *body;
        } play;
        struct { Expr *dur; } wait;
        struct { Expr *lhs; Expr *rhs; } arrow_cue;
    } u;
};

/* ----------------------------------------------------------- Runtime Value */

typedef enum ValKind {
    VAL_NIL = 0,
    VAL_NUM,
    VAL_BOOL,
    VAL_STR,
    VAL_VEC,
    VAL_COL,
    VAL_MOB,
    VAL_TRACKER,
    VAL_CAM,
    VAL_RATE,
    VAL_LIST,
    VAL_RANGE,
    VAL_BAG,
    VAL_FN,
    VAL_PAIR_COL
} ValKind;

typedef struct Val Val;

typedef struct ValList {
    Val     *items;
    uint32_t count;
} ValList;

typedef struct ValBag {
    const char **keys;
    Val         *vals;
    uint32_t     count;
} ValBag;

typedef struct ScriptFn {
    bool         is_lambda;
    const char  *params[SC_MAX_PARAMS];
    uint32_t     param_count;
    Expr        *lambda_body;
    Stmt        *fn_body;
} ScriptFn;

struct Val {
    ValKind kind;
    union {
        double        num;
        bool          b;
        const char   *str;
        AnimVec3      vec;
        AnimColor     col;
        AnimId        mob;
        AnimTrackerId tracker;
        AnimCamera    cam;
        AnimRateKind  rate;
        ValList       list;
        struct { double lo, hi, step; bool has_step; } range;
        ValBag        bag;
        ScriptFn     *fn;
        struct { AnimColor c0, c1; } pair_col;
    } u;
};

/* --------------------------------------------------- Env & Reactive Bindings */

typedef struct VarSlot {
    const char *name;
    bool        is_reactive;
    Expr       *react_expr;
    Val         val;
} VarSlot;

typedef struct EnvScope {
    VarSlot        *vars;
    uint32_t        count;
    uint32_t        cap;
    struct EnvScope *parent;
} EnvScope;

typedef enum ReactBindKind {
    RB_MOVE_TO = 0,
    RB_NEXT_TO,
    RB_LINE,
    RB_ARROW,
    RB_ARC,
    RB_RIGHT_ANGLE,
    RB_DECIMAL,
    RB_PLOT,
    RB_AREA,
    RB_RIEMANN,
    RB_IMPLICIT,
    RB_CONTOUR,
    RB_POLAR,
    RB_PARAM,
    RB_SURFACE,
    RB_PHYS_SPRING,
    RB_PHYS_PENDULUM,
    RB_PHYS_WAVE
} ReactBindKind;

typedef struct ScriptCbCtx {
    AnimScript *sc;
    ScriptFn   *fn;
} ScriptCbCtx;

typedef struct ReactBinding {
    AnimScript   *sc;
    ReactBindKind kind;
    AnimId        target;
    AnimId        ref_mob;
    Expr         *e0;
    Expr         *e1;
    Expr         *e2;
    Expr         *e3;
    float         f0;
    float         f1;
    float         f2;
    float         f3;
    uint32_t      u0;
    uint32_t      u1;
    AnimColor     c0;
    AnimColor     c1;
    AnimVec3      dir;
    ScriptCbCtx  *cb_ctx;
    float        *levels;
} ReactBinding;

/* ----------------------------------------------------------- Allocator Slab */

typedef struct MemBlock {
    void            *ptr;
    struct MemBlock *next;
} MemBlock;

struct AnimScript {
    AnimScene      *scene;
    AnimTimeline   *tl;
    AnimScriptOpts *opts;

    const char     *filename;
    const char     *src;
    Token          *toks;
    uint32_t        tok_count;
    uint32_t        tok_cap;
    uint32_t        pos;

    EnvScope       *global;
    EnvScope       *cur_env;
    bool            returning;
    Val             ret_val;

    /* Active timeline cue collector inside play/lagged/sequence/at */
    bool            in_play;
    AnimSpec        play_specs[SC_MAX_SPECS];
    uint32_t        play_count;

    uint64_t        rng_state;
    MemBlock       *mem_head;
    bool            has_error;
    char            err[SC_MAX_ERR];
};

static void *sc_alloc(AnimScript *sc, size_t sz)
{
    void     *p = calloc(1, sz ? sz : 1u);
    MemBlock *b = (MemBlock *)malloc(sizeof *b);
    MU_ASSERT(p != NULL && b != NULL);
    b->ptr       = p;
    b->next      = sc->mem_head;
    sc->mem_head = b;
    return p;
}

static char *sc_strdup_n(AnimScript *sc, const char *s, size_t n)
{
    char *p = (char *)sc_alloc(sc, n + 1u);
    if (n > 0u)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void sc_error(AnimScript *sc, int line, const char *fmt, ...)
{
    va_list ap;
    char    msg[384];
    if (sc->has_error)
        return;
    sc->has_error = true;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(sc->err, sizeof sc->err, "%s:%d: %s",
             sc->filename ? sc->filename : "<script>", line > 0 ? line : 1, msg);
}

/* ------------------------------------------------------------------- Lexer */

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_cont(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void push_tok(AnimScript *sc, Token t)
{
    if (sc->tok_count >= sc->tok_cap)
    {
        sc->tok_cap = sc->tok_cap ? sc->tok_cap * 2u : 256u;
        sc->toks = (Token *)realloc(sc->toks, (size_t)sc->tok_cap * sizeof *sc->toks);
        MU_ASSERT(sc->toks != NULL);
    }
    sc->toks[sc->tok_count++] = t;
}

static TokKind keyword_or_ident(const char *s, size_t n)
{
    if (n == 5 && memcmp(s, "scene", 5) == 0)    return KW_SCENE;
    if (n == 4 && memcmp(s, "play", 4) == 0)     return KW_PLAY;
    if (n == 6 && memcmp(s, "lagged", 6) == 0)   return KW_LAGGED;
    if (n == 8 && memcmp(s, "sequence", 8) == 0) return KW_SEQUENCE;
    if (n == 2 && memcmp(s, "at", 2) == 0)       return KW_AT;
    if (n == 4 && memcmp(s, "wait", 4) == 0)     return KW_WAIT;
    if (n == 2 && memcmp(s, "fn", 2) == 0)       return KW_FN;
    if (n == 6 && memcmp(s, "return", 6) == 0)   return KW_RETURN;
    if (n == 3 && memcmp(s, "for", 3) == 0)      return KW_FOR;
    if (n == 2 && memcmp(s, "in", 2) == 0)       return KW_IN;
    if (n == 2 && memcmp(s, "if", 2) == 0)       return KW_IF;
    if (n == 4 && memcmp(s, "else", 4) == 0)     return KW_ELSE;
    if (n == 4 && memcmp(s, "true", 4) == 0)     return KW_TRUE;
    if (n == 5 && memcmp(s, "false", 5) == 0)    return KW_FALSE;
    if (n == 7 && memcmp(s, "default", 7) == 0)  return KW_DEFAULT;
    if (n == 4 && memcmp(s, "null", 4) == 0)     return KW_NULL;
    return TOK_IDENT;
}

static bool tokenize(AnimScript *sc, const char *src)
{
    const char *p    = src;
    int         line = 1;
    int         col  = 1;

    sc->tok_count = 0u;
    while (*p)
    {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r')
        {
            p++; col++;
            continue;
        }
        if (c == '\n')
        {
            push_tok(sc, (Token){ .kind = TOK_NEWLINE, .line = line, .col = col });
            p++; line++; col = 1;
            continue;
        }
        if (c == '/' && p[1] == '/')
        {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '/' && p[1] == '*')
        {
            p += 2; col += 2;
            while (*p && !(p[0] == '*' && p[1] == '/'))
            {
                if (*p == '\n') { line++; col = 1; } else col++;
                p++;
            }
            if (*p) { p += 2; col += 2; }
            continue;
        }
        if (c == '#')
        {
            const char *s = p + 1;
            uint32_t    hd = 0u;
            size_t      hn = 0u;
            Token       t = { .kind = TOK_HEX, .line = line, .col = col };
            while (hex_nibble(s[hn]) >= 0 && hn < 8u)
                hn++;
            if (hn != 6u && hn != 8u)
            {
                sc_error(sc, line, "invalid hex color (expected #RRGGBB or #RRGGBBAA)");
                return false;
            }
            for (size_t k = 0u; k < 6u; ++k)
                hd = (hd << 4) | (uint32_t)hex_nibble(s[k]);
            t.hex_rgb = hd;
            if (hn == 8u)
            {
                t.hex_has_alpha = true;
                t.hex_alpha     = (uint8_t)((hex_nibble(s[6]) << 4) | hex_nibble(s[7]));
            }
            else
            {
                t.hex_alpha = 255u;
            }
            push_tok(sc, t);
            p += 1u + hn; col += (int)(1u + hn);
            continue;
        }
        if (c == '"')
        {
            char   buf[4096];
            size_t blen = 0u;
            int    sline = line, scol = col;
            p++; col++;
            while (*p && *p != '"')
            {
                char ch = *p++;
                col++;
                if (ch == '\n') { line++; col = 1; }
                if (ch == '\\' && *p)
                {
                    char esc = *p++;
                    col++;
                    if (esc == 'n') ch = '\n';
                    else if (esc == 't') ch = '\t';
                    else ch = esc;
                }
                if (blen + 1u < sizeof buf)
                    buf[blen++] = ch;
            }
            if (*p != '"') { sc_error(sc, sline, "unterminated string literal"); return false; }
            p++; col++;
            push_tok(sc, (Token){ .kind = TOK_STR, .text = sc_strdup_n(sc, buf, blen), .len = blen, .line = sline, .col = scol });
            continue;
        }
        if (c == '`')
        {
            const char *start = p + 1;
            int         sline = line, scol = col;
            size_t      slen;
            p++; col++;
            while (*p && *p != '`')
            {
                if (*p == '\n') { line++; col = 1; } else col++;
                p++;
            }
            if (*p != '`') { sc_error(sc, sline, "unterminated raw backtick string"); return false; }
            slen = (size_t)(p - start);
            p++; col++;
            push_tok(sc, (Token){ .kind = TOK_RAW_STR, .text = sc_strdup_n(sc, start, slen), .len = slen, .line = sline, .col = scol });
            continue;
        }
        if (c == '$')
        {
            char buf[2048];
            size_t blen = 0u;
            int sline = line, scol = col;
            p++; col++;
            while (*p && *p != '$')
            {
                if (*p == '\\' && p[1] == '$')
                {
                    if (blen + 1u < sizeof buf) buf[blen++] = '$';
                    p += 2; col += 2;
                    continue;
                }
                if (*p == '\n') { line++; col = 1; } else col++;
                if (blen + 1u < sizeof buf) buf[blen++] = *p;
                p++;
            }
            if (*p != '$') { sc_error(sc, sline, "unterminated '$...$' math literal"); return false; }
            p++; col++;
            push_tok(sc, (Token){ .kind = TOK_TEX_LIT, .text = sc_strdup_n(sc, buf, blen), .len = blen, .line = sline, .col = scol });
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9'))
        {
            char  *end = NULL;
            double v;
            int    scol = col;
            const char *q = p;
            while (*q >= '0' && *q <= '9') q++;
            if (q > p && q[0] == '.' && q[1] == '.')
            {
                char tmp[64];
                size_t l = MU_MIN((size_t)(q - p), sizeof tmp - 1u);
                memcpy(tmp, p, l);
                tmp[l] = '\0';
                v = strtod(tmp, NULL);
                col += (int)(q - p);
                p = q;
            }
            else
            {
                v = strtod(p, &end);
                col += (int)(end - p);
                p = end;
            }
            /* Optional unit suffixes: ms, s, deg, rad */
            if (p[0] == 'm' && p[1] == 's' && !is_ident_cont(p[2]))
            {
                v *= 0.001; p += 2; col += 2;
            }
            else if (p[0] == 's' && !is_ident_cont(p[1]))
            {
                p += 1; col += 1;
            }
            else if (p[0] == 'd' && p[1] == 'e' && p[2] == 'g' && !is_ident_cont(p[3]))
            {
                v *= (double)ANIM_PI / 180.0; p += 3; col += 3;
            }
            else if (p[0] == 'r' && p[1] == 'a' && p[2] == 'd' && !is_ident_cont(p[3]))
            {
                p += 3; col += 3;
            }
            push_tok(sc, (Token){ .kind = TOK_NUM, .num = v, .line = line, .col = scol });
            continue;
        }
        if (is_ident_start(c))
        {
            const char *start = p;
            int         scol  = col;
            size_t      len;
            TokKind     kk;
            while (is_ident_cont(*p)) { p++; col++; }
            len = (size_t)(p - start);
            kk  = keyword_or_ident(start, len);
            push_tok(sc, (Token){ .kind = kk, .text = sc_strdup_n(sc, start, len), .len = len, .line = line, .col = scol });
            continue;
        }
        /* Multi-char operators */
        if (c == ':' && p[1] == '=') { push_tok(sc, (Token){ .kind = TOK_REACT, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '-' && p[1] == '>') { push_tok(sc, (Token){ .kind = TOK_ARROW, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '=' && p[1] == '>') { push_tok(sc, (Token){ .kind = TOK_FAT_ARROW, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '=' && p[1] == '=') { push_tok(sc, (Token){ .kind = TOK_EQ, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '!' && p[1] == '=') { push_tok(sc, (Token){ .kind = TOK_NEQ, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '<' && p[1] == '=') { push_tok(sc, (Token){ .kind = TOK_LE, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '>' && p[1] == '=') { push_tok(sc, (Token){ .kind = TOK_GE, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '&' && p[1] == '&') { push_tok(sc, (Token){ .kind = TOK_AND, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '|' && p[1] == '|') { push_tok(sc, (Token){ .kind = TOK_OR, .line = line, .col = col }); p += 2; col += 2; continue; }
        if (c == '.' && p[1] == '.') { push_tok(sc, (Token){ .kind = TOK_DOTDOT, .line = line, .col = col }); p += 2; col += 2; continue; }

        {
            TokKind k = TOK_EOF;
            switch (c)
            {
                case '+': k = TOK_PLUS;    break;
                case '-': k = TOK_MINUS;   break;
                case '*': k = TOK_STAR;    break;
                case '/': k = TOK_SLASH;   break;
                case '%': k = TOK_PERCENT; break;
                case '^': k = TOK_CARET;   break;
                case '|': k = TOK_BAR;     break;
                case '&': k = TOK_AMP;     break;
                case '~': k = TOK_TILDE;   break;
                case '!': k = TOK_BANG;    break;
                case '<': k = TOK_LT;      break;
                case '>': k = TOK_GT;      break;
                case '=': k = TOK_ASSIGN;  break;
                case '(': k = TOK_LPAREN;  break;
                case ')': k = TOK_RPAREN;  break;
                case '{': k = TOK_LBRACE;  break;
                case '}': k = TOK_RBRACE;  break;
                case '[': k = TOK_LBRACK;  break;
                case ']': k = TOK_RBRACK;  break;
                case ',': k = TOK_COMMA;   break;
                case ':': k = TOK_COLON;   break;
                case ';': k = TOK_SEMI;    break;
                case '.': k = TOK_DOT;     break;
                default:
                    sc_error(sc, line, "unexpected character '%c'", c);
                    return false;
            }
            push_tok(sc, (Token){ .kind = k, .line = line, .col = col });
            p++; col++;
        }
    }
    push_tok(sc, (Token){ .kind = TOK_EOF, .line = line, .col = col });
    return true;
}

/* ------------------------------------------------------------------ Parser */

static Token peek(AnimScript *sc)
{
    if (sc->pos < sc->tok_count)
        return sc->toks[sc->pos];
    return sc->toks[sc->tok_count - 1u];
}

static Token peek_skip_nl(AnimScript *sc, uint32_t offset)
{
    uint32_t i = sc->pos;
    uint32_t seen = 0u;
    while (i < sc->tok_count)
    {
        if (sc->toks[i].kind != TOK_NEWLINE)
        {
            if (seen == offset)
                return sc->toks[i];
            seen++;
        }
        i++;
    }
    return sc->toks[sc->tok_count - 1u];
}

static void skip_nl(AnimScript *sc)
{
    while (sc->pos < sc->tok_count && sc->toks[sc->pos].kind == TOK_NEWLINE)
        sc->pos++;
}

static void skip_sep(AnimScript *sc)
{
    while (sc->pos < sc->tok_count &&
           (sc->toks[sc->pos].kind == TOK_NEWLINE || sc->toks[sc->pos].kind == TOK_SEMI))
        sc->pos++;
}

static Token advance(AnimScript *sc)
{
    Token t = peek(sc);
    if (sc->pos + 1u < sc->tok_count)
        sc->pos++;
    return t;
}

static bool match(AnimScript *sc, TokKind k)
{
    if (peek(sc).kind == k)
    {
        advance(sc);
        return true;
    }
    return false;
}

static bool expect(AnimScript *sc, TokKind k, const char *what)
{
    skip_nl(sc);
    if (peek(sc).kind == k)
    {
        advance(sc);
        return true;
    }
    sc_error(sc, peek(sc).line, "expected %s", what);
    return false;
}

static Expr *new_expr(AnimScript *sc, ExprKind k, int line)
{
    Expr *e = (Expr *)sc_alloc(sc, sizeof *e);
    e->kind = k;
    e->line = line;
    return e;
}

static Stmt *new_stmt(AnimScript *sc, StmtKind k, int line)
{
    Stmt *s = (Stmt *)sc_alloc(sc, sizeof *s);
    s->kind = k;
    s->line = line;
    return s;
}

static Expr *parse_expr(AnimScript *sc);
static Stmt *parse_stmt(AnimScript *sc);

static bool is_lambda_ahead(AnimScript *sc)
{
    uint32_t i = sc->pos;
    if (sc->toks[i].kind != TOK_LPAREN)
        return false;
    i++;
    while (i < sc->tok_count && sc->toks[i].kind != TOK_RPAREN)
    {
        TokKind k = sc->toks[i].kind;
        if (k != TOK_IDENT && k != TOK_COMMA && k != TOK_NEWLINE)
            return false;
        i++;
    }
    if (i >= sc->tok_count || sc->toks[i].kind != TOK_RPAREN)
        return false;
    i++;
    while (i < sc->tok_count && sc->toks[i].kind == TOK_NEWLINE) i++;
    return (i < sc->tok_count && sc->toks[i].kind == TOK_FAT_ARROW);
}

static Expr *parse_bag_expr(AnimScript *sc, int line)
{
    Expr     *e   = new_expr(sc, EX_BAG, line);
    BagField *buf = (BagField *)sc_alloc(sc, 64u * sizeof *buf);
    uint32_t  cnt = 0u;

    advance(sc); /* '{' */
    skip_sep(sc);
    while (peek(sc).kind != TOK_RBRACE && peek(sc).kind != TOK_EOF && !sc->has_error)
    {
        Token ktok = advance(sc);
        Expr *val;
        if (ktok.kind != TOK_IDENT && ktok.kind != TOK_STR)
        {
            sc_error(sc, ktok.line, "expected key name in config bag");
            return e;
        }
        if (!expect(sc, TOK_COLON, "':' in config bag"))
            return e;
        skip_nl(sc);
        val = parse_expr(sc);
        if (match(sc, TOK_ARROW))
        {
            Expr *p = new_expr(sc, EX_PAIR, val->line);
            p->u.binary.op  = TOK_ARROW;
            p->u.binary.lhs = val;
            p->u.binary.rhs = parse_expr(sc);
            val = p;
        }
        if (cnt < 64u)
        {
            buf[cnt].key = ktok.text;
            buf[cnt].val = val;
            cnt++;
        }
        skip_nl(sc);
        if (!match(sc, TOK_COMMA))
            skip_sep(sc);
        skip_sep(sc);
    }
    expect(sc, TOK_RBRACE, "'}'");
    e->u.bag.fields = buf;
    e->u.bag.count  = cnt;
    return e;
}

static void parse_call_args(AnimScript *sc, Expr ***out_args, const char ***out_names, uint32_t *out_count)
{
    Expr       **args  = (Expr **)sc_alloc(sc, SC_MAX_ARGS * sizeof *args);
    const char **names = (const char **)sc_alloc(sc, SC_MAX_ARGS * sizeof *names);
    uint32_t     ac    = 0u;

    skip_nl(sc);
    while (peek(sc).kind != TOK_RPAREN && peek(sc).kind != TOK_EOF && !sc->has_error)
    {
        const char *kname = NULL;
        Token       t0    = peek(sc);
        Token       t1    = peek_skip_nl(sc, 1u);
        Expr       *val;

        if ((t0.kind == TOK_IDENT || t0.kind == KW_DEFAULT) &&
            t1.kind == TOK_COLON)
        {
            kname = advance(sc).text;
            advance(sc); /* ':' */
            skip_nl(sc);
        }
        val = parse_expr(sc);
        if (ac < SC_MAX_ARGS)
        {
            args[ac]  = val;
            names[ac] = kname;
            ac++;
        }
        skip_nl(sc);
        if (!match(sc, TOK_COMMA))
            break;
        skip_nl(sc);
    }
    expect(sc, TOK_RPAREN, "')'");
    *out_args  = args;
    *out_names = names;
    *out_count = ac;
}

static Expr *parse_primary(AnimScript *sc)
{
    Token t;
    skip_nl(sc);
    t = peek(sc);

    if (t.kind == TOK_NUM)
    {
        Expr *e = new_expr(sc, EX_NUM, t.line);
        e->u.num.val = t.num;
        advance(sc);
        return e;
    }
    if (t.kind == KW_TRUE || t.kind == KW_FALSE)
    {
        Expr *e = new_expr(sc, EX_BOOL, t.line);
        e->u.boolean.val = (t.kind == KW_TRUE);
        advance(sc);
        return e;
    }
    if (t.kind == TOK_STR || t.kind == TOK_RAW_STR)
    {
        Expr *e = new_expr(sc, EX_STR, t.line);
        e->u.str.str = t.text;
        advance(sc);
        return e;
    }
    if (t.kind == TOK_TEX_LIT)
    {
        Expr *e = new_expr(sc, EX_TEX, t.line);
        e->u.str.str = t.text;
        advance(sc);
        return e;
    }
    if (t.kind == TOK_HEX)
    {
        Expr *e = new_expr(sc, EX_HEX, t.line);
        e->u.hex.rgb       = t.hex_rgb;
        e->u.hex.alpha     = t.hex_alpha;
        e->u.hex.has_alpha = t.hex_has_alpha;
        advance(sc);
        return e;
    }
    if (t.kind == KW_DEFAULT)
    {
        Expr *e = new_expr(sc, EX_IDENT, t.line);
        e->u.ident.name = "default";
        advance(sc);
        return e;
    }
    if (t.kind == KW_NULL)
    {
        Expr *e = new_expr(sc, EX_IDENT, t.line);
        e->u.ident.name = "null";
        advance(sc);
        return e;
    }
    if (t.kind == TOK_IDENT)
    {
        advance(sc);
        /* Paren-free 1-arg lambda: `x => expr` */
        if (peek(sc).kind == TOK_FAT_ARROW)
        {
            Expr        *e      = new_expr(sc, EX_LAMBDA, t.line);
            const char **params = (const char **)sc_alloc(sc, sizeof *params);
            advance(sc); /* '=>' */
            params[0]               = t.text;
            e->u.lambda.params      = params;
            e->u.lambda.param_count = 1u;
            e->u.lambda.body        = parse_expr(sc);
            return e;
        }
        {
            Expr *e = new_expr(sc, EX_IDENT, t.line);
            e->u.ident.name = t.text;
            return e;
        }
    }
    if (t.kind == TOK_LBRACK)
    {
        Expr  *e    = new_expr(sc, EX_LIST, t.line);
        Expr **buf  = (Expr **)sc_alloc(sc, 128u * sizeof *buf);
        uint32_t cnt = 0u;
        advance(sc);
        skip_nl(sc);
        while (peek(sc).kind != TOK_RBRACK && peek(sc).kind != TOK_EOF && !sc->has_error)
        {
            if (cnt < 128u)
                buf[cnt++] = parse_expr(sc);
            else
                (void)parse_expr(sc);
            skip_nl(sc);
            if (!match(sc, TOK_COMMA))
                break;
            skip_nl(sc);
        }
        expect(sc, TOK_RBRACK, "']'");
        e->u.list.items = buf;
        e->u.list.count = cnt;
        return e;
    }
    if (t.kind == TOK_LBRACE)
        return parse_bag_expr(sc, t.line);
    if (t.kind == TOK_LPAREN)
    {
        if (is_lambda_ahead(sc))
        {
            Expr        *e      = new_expr(sc, EX_LAMBDA, t.line);
            const char **params = (const char **)sc_alloc(sc, SC_MAX_PARAMS * sizeof *params);
            uint32_t     pc     = 0u;
            advance(sc); /* '(' */
            skip_nl(sc);
            while (peek(sc).kind != TOK_RPAREN && peek(sc).kind != TOK_EOF)
            {
                Token pt = advance(sc);
                if (pt.kind != TOK_IDENT) { sc_error(sc, pt.line, "expected lambda parameter"); return e; }
                if (pc < SC_MAX_PARAMS) params[pc++] = pt.text;
                skip_nl(sc);
                if (!match(sc, TOK_COMMA)) break;
                skip_nl(sc);
            }
            expect(sc, TOK_RPAREN, "')'");
            expect(sc, TOK_FAT_ARROW, "'=>'");
            e->u.lambda.params      = params;
            e->u.lambda.param_count = pc;
            e->u.lambda.body        = parse_expr(sc);
            return e;
        }
        else
        {
            Expr *first;
            advance(sc); /* '(' */
            skip_nl(sc);
            first = parse_expr(sc);
            skip_nl(sc);
            if (match(sc, TOK_COMMA))
            {
                Expr   *e   = new_expr(sc, EX_VEC, t.line);
                Expr  **buf = (Expr **)sc_alloc(sc, 16u * sizeof *buf);
                uint32_t cnt = 0u;
                buf[cnt++] = first;
                do {
                    skip_nl(sc);
                    if (cnt < 16u) buf[cnt++] = parse_expr(sc);
                    else (void)parse_expr(sc);
                    skip_nl(sc);
                } while (match(sc, TOK_COMMA));
                expect(sc, TOK_RPAREN, "')'");
                e->u.list.items = buf;
                e->u.list.count = cnt;
                return e;
            }
            expect(sc, TOK_RPAREN, "')'");
            return first;
        }
    }

    sc_error(sc, t.line, "unexpected token in expression");
    advance(sc);
    return new_expr(sc, EX_NUM, t.line);
}

static Expr *parse_postfix(AnimScript *sc)
{
    Expr *e = parse_primary(sc);
    for (;;)
    {
        if (peek(sc).kind == TOK_LPAREN)
        {
            Token pt   = advance(sc);
            Expr *call = new_expr(sc, EX_CALL, pt.line);
            call->u.call.callee = e;
            parse_call_args(sc, &call->u.call.args, &call->u.call.arg_names, &call->u.call.arg_count);
            e = call;
        }
        else if (peek(sc).kind == TOK_LBRACK)
        {
            Token bt = advance(sc);
            Expr *ix = new_expr(sc, EX_INDEX, bt.line);
            ix->u.index.obj = e;
            ix->u.index.idx = parse_expr(sc);
            expect(sc, TOK_RBRACK, "']'");
            e = ix;
        }
        else if (peek(sc).kind == TOK_DOT)
        {
            Token dt = advance(sc);
            Token ft = advance(sc);
            if (ft.kind != TOK_IDENT)
            { sc_error(sc, dt.line, "expected field or method name after '.'"); return e; }
            if (peek(sc).kind == TOK_LPAREN)
            {
                Expr *pe = new_expr(sc, EX_PIPE, dt.line);
                advance(sc); /* '(' */
                pe->u.pipe.lhs     = e;
                pe->u.pipe.fn_name = ft.text;
                parse_call_args(sc, &pe->u.pipe.args, &pe->u.pipe.arg_names, &pe->u.pipe.arg_count);
                e = pe;
            }
            else
            {
                Expr *fe = new_expr(sc, EX_FIELD, dt.line);
                fe->u.field.obj   = e;
                fe->u.field.field = ft.text;
                e = fe;
            }
        }
        else
        {
            break;
        }
    }
    return e;
}

static Expr *parse_unary(AnimScript *sc)
{
    Token t;
    skip_nl(sc);
    t = peek(sc);
    if (t.kind == TOK_MINUS || t.kind == TOK_BANG || t.kind == TOK_PLUS)
    {
        Expr *e = new_expr(sc, EX_UNARY, t.line);
        advance(sc);
        e->u.unary.op  = t.kind;
        e->u.unary.rhs = parse_unary(sc);
        return e;
    }
    return parse_postfix(sc);
}

static Expr *parse_power(AnimScript *sc)
{
    Expr *lhs = parse_unary(sc);
    if (peek(sc).kind == TOK_CARET)
    {
        Token op = advance(sc);
        Expr *e  = new_expr(sc, EX_BINARY, op.line);
        e->u.binary.op  = op.kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_power(sc);
        return e;
    }
    return lhs;
}

static Expr *parse_mul(AnimScript *sc)
{
    Expr *lhs = parse_power(sc);
    while (peek(sc).kind == TOK_STAR || peek(sc).kind == TOK_SLASH || peek(sc).kind == TOK_PERCENT)
    {
        Token op = advance(sc);
        Expr *e  = new_expr(sc, EX_BINARY, op.line);
        e->u.binary.op  = op.kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_power(sc);
        lhs = e;
    }
    return lhs;
}

static Expr *parse_add(AnimScript *sc)
{
    Expr *lhs = parse_mul(sc);
    while (peek(sc).kind == TOK_PLUS || peek(sc).kind == TOK_MINUS)
    {
        Token op = advance(sc);
        Expr *e  = new_expr(sc, EX_BINARY, op.line);
        e->u.binary.op  = op.kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_mul(sc);
        lhs = e;
    }
    return lhs;
}

static Expr *parse_range(AnimScript *sc)
{
    Expr *lhs = parse_add(sc);
    if (peek(sc).kind == TOK_DOTDOT)
    {
        Token op = advance(sc);
        Expr *r  = new_expr(sc, EX_RANGE, op.line);
        r->u.range.lo   = lhs;
        r->u.range.hi   = parse_add(sc);
        r->u.range.step = match(sc, TOK_COLON) ? parse_add(sc) : NULL;
        return r;
    }
    return lhs;
}

static Expr *parse_cmp(AnimScript *sc)
{
    Expr *lhs = parse_range(sc);
    while (peek(sc).kind == TOK_LT || peek(sc).kind == TOK_LE ||
           peek(sc).kind == TOK_GT || peek(sc).kind == TOK_GE ||
           peek(sc).kind == TOK_EQ || peek(sc).kind == TOK_NEQ)
    {
        Token op = advance(sc);
        Expr *e  = new_expr(sc, EX_BINARY, op.line);
        e->u.binary.op  = op.kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_range(sc);
        lhs = e;
    }
    return lhs;
}

static Expr *parse_bit(AnimScript *sc)
{
    Expr *lhs = parse_cmp(sc);
    while (peek(sc).kind == TOK_BAR || peek(sc).kind == TOK_AMP)
    {
        Token op = advance(sc);
        Expr *e  = new_expr(sc, EX_BINARY, op.line);
        e->u.binary.op  = op.kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_cmp(sc);
        lhs = e;
    }
    return lhs;
}

static Expr *parse_logic(AnimScript *sc)
{
    Expr *lhs = parse_bit(sc);
    while (peek(sc).kind == TOK_AND || peek(sc).kind == TOK_OR)
    {
        Token op = advance(sc);
        Expr *e  = new_expr(sc, EX_BINARY, op.line);
        e->u.binary.op  = op.kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_bit(sc);
        lhs = e;
    }
    return lhs;
}

static Expr *parse_expr(AnimScript *sc)
{
    return parse_logic(sc);
}

static Stmt *parse_block(AnimScript *sc)
{
    Token   bt   = peek(sc);
    Stmt   *blk  = new_stmt(sc, ST_BLOCK, bt.line);
    Stmt  **arr  = (Stmt **)sc_alloc(sc, 512u * sizeof *arr);
    uint32_t cap = 512u, cnt = 0u;

    if (!expect(sc, TOK_LBRACE, "'{'"))
        return blk;
    skip_sep(sc);
    while (peek(sc).kind != TOK_RBRACE && peek(sc).kind != TOK_EOF && !sc->has_error)
    {
        Stmt *st = parse_stmt(sc);
        if (cnt >= cap)
        {
            cap *= 2u;
            Stmt **nb = (Stmt **)sc_alloc(sc, (size_t)cap * sizeof *nb);
            memcpy(nb, arr, (size_t)cnt * sizeof *nb);
            arr = nb;
        }
        arr[cnt++] = st;
        skip_sep(sc);
    }
    expect(sc, TOK_RBRACE, "'}'");
    blk->u.block.items = arr;
    blk->u.block.count = cnt;
    return blk;
}

static bool is_fn_shorthand_ahead(const AnimScript *sc)
{
    uint32_t i = sc->pos;
    if (i + 2u >= sc->tok_count || sc->toks[i].kind != TOK_IDENT || sc->toks[i + 1u].kind != TOK_LPAREN)
        return false;
    i += 2u;
    while (i < sc->tok_count && sc->toks[i].kind != TOK_RPAREN)
    {
        if (sc->toks[i].kind != TOK_IDENT)
            return false;
        i++;
        if (i < sc->tok_count && sc->toks[i].kind == TOK_COMMA)
            i++;
        else
            break;
    }
    if (i >= sc->tok_count || sc->toks[i].kind != TOK_RPAREN)
        return false;
    i++;
    return (i < sc->tok_count && sc->toks[i].kind == TOK_ASSIGN);
}

static Stmt *parse_stmt(AnimScript *sc)
{
    Token t;
    skip_sep(sc);
    t = peek(sc);

    if (t.kind == KW_SCENE)
    {
        Stmt *s = new_stmt(sc, ST_SCENE, t.line);
        Expr *b;
        advance(sc);
        skip_nl(sc);
        if (peek(sc).kind != TOK_LBRACE) { sc_error(sc, t.line, "expected '{' after scene"); return s; }
        b = parse_bag_expr(sc, t.line);
        s->u.scene.fields = b->u.bag.fields;
        s->u.scene.count  = b->u.bag.count;
        return s;
    }
    if (t.kind == KW_PLAY || t.kind == KW_LAGGED || t.kind == KW_SEQUENCE || t.kind == KW_AT)
    {
        Stmt *s = new_stmt(sc, ST_PLAY, t.line);
        bool  has_paren;
        advance(sc);
        has_paren = match(sc, TOK_LPAREN);
        if (t.kind == KW_PLAY)
        {
            s->u.play.mode = 0;
            if (has_paren || peek(sc).kind == TOK_NUM)
            {
                s->u.play.dur = parse_expr(sc);
                if (match(sc, TOK_COMMA))
                {
                    if (peek(sc).kind == TOK_IDENT && peek_skip_nl(sc, 1u).kind == TOK_COLON)
                    {
                        advance(sc); advance(sc); /* e.g. rate: */
                    }
                    s->u.play.rate = parse_expr(sc);
                }
            }
        }
        else if (t.kind == KW_LAGGED)
        {
            s->u.play.mode = 1;
            if (has_paren || peek(sc).kind == TOK_NUM)
            {
                s->u.play.dur = parse_expr(sc);
                while (match(sc, TOK_COMMA))
                {
                    if (peek(sc).kind == TOK_IDENT && peek_skip_nl(sc, 1u).kind == TOK_COLON)
                    {
                        Token kw = advance(sc); advance(sc);
                        if (strcmp(kw.text, "rate") == 0) s->u.play.rate = parse_expr(sc);
                        else s->u.play.lag = parse_expr(sc);
                    }
                    else if (!s->u.play.lag) s->u.play.lag = parse_expr(sc);
                    else s->u.play.rate = parse_expr(sc);
                }
            }
        }
        else if (t.kind == KW_SEQUENCE)
        {
            s->u.play.mode = 2;
            if (has_paren || peek(sc).kind == TOK_NUM)
            {
                s->u.play.dur = parse_expr(sc);
                if (match(sc, TOK_COMMA))
                {
                    if (peek(sc).kind == TOK_IDENT && peek_skip_nl(sc, 1u).kind == TOK_COLON) { advance(sc); advance(sc); }
                    s->u.play.rate = parse_expr(sc);
                }
            }
        }
        else
        {
            s->u.play.mode = 3;
            s->u.play.t0   = parse_expr(sc);
            expect(sc, TOK_COMMA, "',' in at(start, dur)");
            s->u.play.dur  = parse_expr(sc);
            if (match(sc, TOK_COMMA))
            {
                if (peek(sc).kind == TOK_IDENT && peek_skip_nl(sc, 1u).kind == TOK_COLON) { advance(sc); advance(sc); }
                s->u.play.rate = parse_expr(sc);
            }
        }
        if (has_paren)
            expect(sc, TOK_RPAREN, "')'");
        match(sc, TOK_COLON);
        skip_nl(sc);
        s->u.play.body = (peek(sc).kind == TOK_LBRACE) ? parse_block(sc) : parse_stmt(sc);
        return s;
    }
    if (t.kind == KW_WAIT || t.kind == TOK_TILDE)
    {
        Stmt *s = new_stmt(sc, ST_WAIT, t.line);
        advance(sc);
        s->u.wait.dur = parse_expr(sc);
        return s;
    }
    if (is_fn_shorthand_ahead(sc))
    {
        Stmt        *s      = new_stmt(sc, ST_FN, t.line);
        const char **params = (const char **)sc_alloc(sc, SC_MAX_PARAMS * sizeof *params);
        uint32_t     pc     = 0u;
        Stmt        *ret    = new_stmt(sc, ST_RETURN, t.line);
        s->u.fn.name = advance(sc).text;
        advance(sc); /* '(' */
        while (peek(sc).kind != TOK_RPAREN && peek(sc).kind != TOK_EOF)
        {
            Token pt = advance(sc);
            if (pc < SC_MAX_PARAMS) params[pc++] = pt.text;
            if (!match(sc, TOK_COMMA)) break;
        }
        expect(sc, TOK_RPAREN, "')'");
        expect(sc, TOK_ASSIGN, "'='");
        ret->u.ret.expr     = parse_expr(sc);
        s->u.fn.params      = params;
        s->u.fn.param_count = pc;
        s->u.fn.body        = ret;
        return s;
    }
    if (t.kind == KW_FN)
    {
        Stmt        *s      = new_stmt(sc, ST_FN, t.line);
        const char **params = (const char **)sc_alloc(sc, SC_MAX_PARAMS * sizeof *params);
        uint32_t     pc     = 0u;
        Token        nt;
        advance(sc);
        nt = advance(sc);
        if (nt.kind != TOK_IDENT) { sc_error(sc, t.line, "expected function name"); return s; }
        s->u.fn.name = nt.text;
        expect(sc, TOK_LPAREN, "'('");
        skip_nl(sc);
        while (peek(sc).kind != TOK_RPAREN && peek(sc).kind != TOK_EOF)
        {
            Token pt = advance(sc);
            if (pt.kind != TOK_IDENT) { sc_error(sc, pt.line, "expected param name"); return s; }
            if (pc < SC_MAX_PARAMS) params[pc++] = pt.text;
            skip_nl(sc);
            if (!match(sc, TOK_COMMA)) break;
            skip_nl(sc);
        }
        expect(sc, TOK_RPAREN, "')'");
        s->u.fn.params      = params;
        s->u.fn.param_count = pc;
        s->u.fn.body        = parse_block(sc);
        return s;
    }
    if (t.kind == KW_RETURN)
    {
        Stmt *s = new_stmt(sc, ST_RETURN, t.line);
        advance(sc);
        if (peek(sc).kind != TOK_NEWLINE && peek(sc).kind != TOK_SEMI && peek(sc).kind != TOK_RBRACE)
            s->u.ret.expr = parse_expr(sc);
        return s;
    }
    if (t.kind == KW_IF)
    {
        Stmt *s = new_stmt(sc, ST_IF, t.line);
        advance(sc);
        s->u.ifs.cond   = parse_expr(sc);
        s->u.ifs.then_b = parse_block(sc);
        skip_nl(sc);
        if (match(sc, KW_ELSE))
        {
            skip_nl(sc);
            s->u.ifs.else_b = (peek(sc).kind == KW_IF) ? parse_stmt(sc) : parse_block(sc);
        }
        return s;
    }
    if (t.kind == KW_FOR)
    {
        Token vt;
        Expr *e1;
        Stmt *s;
        advance(sc);
        vt = advance(sc);
        if (vt.kind != TOK_IDENT) { sc_error(sc, t.line, "expected loop variable"); return new_stmt(sc, ST_EXPR, t.line); }
        expect(sc, KW_IN, "'in'");
        e1 = parse_expr(sc);
        s = new_stmt(sc, ST_FOR_LIST, t.line);
        s->u.for_l.var  = vt.text;
        s->u.for_l.list = e1;
        s->u.for_l.body = parse_block(sc);
        return s;
    }
    if (t.kind == TOK_LBRACE)
        return parse_block(sc);

    /* Assignment (`=`, `:=`), arrow cue (`->`), or expression */
    if (t.kind == TOK_IDENT &&
        (peek_skip_nl(sc, 1u).kind == TOK_ASSIGN || peek_skip_nl(sc, 1u).kind == TOK_REACT))
    {
        Token    id = advance(sc);
        Token    op = advance(sc);
        Stmt    *s  = new_stmt(sc, op.kind == TOK_REACT ? ST_REACT : ST_ASSIGN, id.line);
        s->u.assign.name = id.text;
        s->u.assign.rhs  = parse_expr(sc);
        return s;
    }

    {
        Expr *e = parse_expr(sc);
        if (match(sc, TOK_ARROW))
        {
            Stmt *s = new_stmt(sc, ST_ARROW_CUE, e->line);
            s->u.arrow_cue.lhs = e;
            s->u.arrow_cue.rhs = parse_expr(sc);
            return s;
        }
        {
            Stmt *s = new_stmt(sc, ST_EXPR, e->line);
            s->u.expr.expr = e;
            return s;
        }
    }
}

/* ------------------------------------------------------------- Environment */

static EnvScope *env_push(AnimScript *sc, EnvScope *parent)
{
    EnvScope *e = (EnvScope *)sc_alloc(sc, sizeof *e);
    e->parent = parent;
    return e;
}

static VarSlot *env_find(EnvScope *e, const char *name)
{
    while (e)
    {
        uint32_t i;
        for (i = 0u; i < e->count; ++i)
        {
            if (strcmp(e->vars[i].name, name) == 0)
                return &e->vars[i];
        }
        e = e->parent;
    }
    return NULL;
}

static void env_set(AnimScript *sc, EnvScope *e, const char *name, Val v, bool is_react, Expr *react_expr)
{
    VarSlot *slot = env_find(e, name);
    if (!slot)
    {
        if (e->count >= e->cap)
        {
            uint32_t nc = e->cap ? e->cap * 2u : 32u;
            VarSlot *nv = (VarSlot *)sc_alloc(sc, (size_t)nc * sizeof *nv);
            if (e->count > 0u)
                memcpy(nv, e->vars, (size_t)e->count * sizeof *nv);
            e->vars = nv;
            e->cap  = nc;
        }
        slot = &e->vars[e->count++];
        slot->name = name;
    }
    slot->is_reactive = is_react;
    slot->react_expr  = react_expr;
    slot->val         = v;
}

/* --------------------------------------------------------- Value Utilities */

static Val val_nil(void)                   { return (Val){ .kind = VAL_NIL }; }
static Val val_num(double x)               { Val v = { .kind = VAL_NUM }; v.u.num = x; return v; }
static Val val_bool(bool b)                { Val v = { .kind = VAL_BOOL }; v.u.b = b; return v; }
static Val val_str(const char *s)          { Val v = { .kind = VAL_STR }; v.u.str = s ? s : ""; return v; }
static Val val_vec(AnimVec3 p)             { Val v = { .kind = VAL_VEC }; v.u.vec = p; return v; }
static Val val_col(AnimColor c)            { Val v = { .kind = VAL_COL }; v.u.col = c; return v; }
static Val val_mob(AnimId id)              { Val v = { .kind = VAL_MOB }; v.u.mob = id; return v; }
static Val val_trk(AnimTrackerId tid)      { Val v = { .kind = VAL_TRACKER }; v.u.tracker = tid; return v; }
static Val val_cam(AnimCamera c)           { Val v = { .kind = VAL_CAM }; v.u.cam = c; return v; }
static Val val_rate(AnimRateKind r)        { Val v = { .kind = VAL_RATE }; v.u.rate = r; return v; }

static double as_num(AnimScript *sc, Val v, double def_v)
{
    if (v.kind == VAL_NUM)     return v.u.num;
    if (v.kind == VAL_BOOL)    return v.u.b ? 1.0 : 0.0;
    if (v.kind == VAL_TRACKER) return (double)anim_tracker_get(sc->scene, v.u.tracker);
    return def_v;
}

static bool as_bool(AnimScript *sc, Val v)
{
    if (v.kind == VAL_BOOL)    return v.u.b;
    if (v.kind == VAL_NUM)     return fabs(v.u.num) > 1e-9;
    if (v.kind == VAL_TRACKER) return fabsf(anim_tracker_get(sc->scene, v.u.tracker)) > 1e-6f;
    if (v.kind == VAL_STR)     return v.u.str[0] != '\0';
    if (v.kind == VAL_MOB)     return ANIM_ID_VALID(v.u.mob);
    return v.kind != VAL_NIL;
}

static AnimVec3 as_vec(AnimScript *sc, Val v, AnimVec3 def_v)
{
    if (v.kind == VAL_VEC) return v.u.vec;
    if (v.kind == VAL_MOB) return anim_center(sc->scene, v.u.mob);
    return def_v;
}

static AnimColor as_col(Val v, AnimColor def_c)
{
    if (v.kind == VAL_COL) return v.u.col;
    return def_c;
}

static const char *as_str(Val v)
{
    if (v.kind == VAL_STR) return v.u.str;
    return NULL;
}

static AnimId as_mob(Val v)
{
    if (v.kind == VAL_MOB) return v.u.mob;
    return ANIM_ID_NIL;
}

static AnimRateKind as_rate(Val v, AnimRateKind def_r)
{
    if (v.kind == VAL_RATE) return v.u.rate;
    if (v.kind == VAL_NUM)  return (AnimRateKind)(int)v.u.num;
    return def_r;
}

static bool bag_get(const ValBag *b, const char *key, Val *out)
{
    uint32_t i;
    for (i = 0u; i < b->count; ++i)
    {
        if (strcmp(b->keys[i], key) == 0)
        {
            if (out) *out = b->vals[i];
            return true;
        }
    }
    return false;
}

static AnimCamera bag_to_camera(AnimScript *sc, Val v, AnimCamera base)
{
    if (v.kind == VAL_CAM)
        return v.u.cam;
    if (v.kind == VAL_VEC)
    {
        base.pos = v.u.vec;
        return base;
    }
    if (v.kind == VAL_BAG)
    {
        Val f;
        if (bag_get(&v.u.bag, "pos", &f))    base.pos    = as_vec(sc, f, base.pos);
        if (bag_get(&v.u.bag, "target", &f)) base.target = as_vec(sc, f, base.target);
        if (bag_get(&v.u.bag, "up", &f))     base.up     = as_vec(sc, f, base.up);
        if (bag_get(&v.u.bag, "fov", &f))    base.fov_deg = (float)as_num(sc, f, base.fov_deg);
        if (bag_get(&v.u.bag, "zoom", &f))   base.zoom    = (float)as_num(sc, f, base.zoom);
        if (bag_get(&v.u.bag, "ortho", &f))  base.ortho_blend = (float)as_num(sc, f, base.ortho_blend);
        if (bag_get(&v.u.bag, "frame_height", &f)) base.frame_height = (float)as_num(sc, f, base.frame_height);
    }
    return base;
}

/* ---------------------------------------------------- Reactive Dependency Scan */

static void collect_reactive_deps(AnimScript *sc, const Expr *e,
                                  AnimTrackerId *out_trks, uint32_t *io_tc, uint32_t cap_t,
                                  AnimId *out_mobs, uint32_t *io_mc, uint32_t cap_m,
                                  int depth);

static void collect_stmt_deps(AnimScript *sc, const Stmt *st,
                              AnimTrackerId *out_trks, uint32_t *io_tc, uint32_t cap_t,
                              AnimId *out_mobs, uint32_t *io_mc, uint32_t cap_m,
                              int depth)
{
    uint32_t i;
    if (!st || depth > 16) return;
    switch (st->kind)
    {
        case ST_EXPR:
            collect_reactive_deps(sc, st->u.expr.expr, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case ST_ASSIGN:
        case ST_REACT:
            collect_reactive_deps(sc, st->u.assign.rhs, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case ST_RETURN:
            collect_reactive_deps(sc, st->u.ret.expr, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case ST_BLOCK:
            for (i = 0u; i < st->u.block.count; ++i)
                collect_stmt_deps(sc, st->u.block.items[i], out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case ST_IF:
            collect_reactive_deps(sc, st->u.ifs.cond, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            collect_stmt_deps(sc, st->u.ifs.then_b, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            collect_stmt_deps(sc, st->u.ifs.else_b, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        default:
            break;
    }
}

static void collect_reactive_deps(AnimScript *sc, const Expr *e,
                                  AnimTrackerId *out_trks, uint32_t *io_tc, uint32_t cap_t,
                                  AnimId *out_mobs, uint32_t *io_mc, uint32_t cap_m,
                                  int depth)
{
    uint32_t i;
    if (!e || depth > 16)
        return;

    switch (e->kind)
    {
        case EX_IDENT:
        {
            VarSlot *vs = env_find(sc->cur_env, e->u.ident.name);
            if (vs)
            {
                if (vs->val.kind == VAL_TRACKER)
                {
                    bool dup = false;
                    for (i = 0u; i < *io_tc; ++i)
                        if (out_trks[i].index == vs->val.u.tracker.index) { dup = true; break; }
                    if (!dup && *io_tc < cap_t)
                        out_trks[(*io_tc)++] = vs->val.u.tracker;
                }
                else if (vs->val.kind == VAL_MOB && ANIM_ID_VALID(vs->val.u.mob))
                {
                    bool dup = false;
                    for (i = 0u; i < *io_mc; ++i)
                        if (out_mobs[i].index == vs->val.u.mob.index) { dup = true; break; }
                    if (!dup && *io_mc < cap_m)
                        out_mobs[(*io_mc)++] = vs->val.u.mob;
                }
                if (vs->is_reactive && vs->react_expr)
                    collect_reactive_deps(sc, vs->react_expr, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
                if (vs->val.kind == VAL_FN && vs->val.u.fn)
                {
                    if (vs->val.u.fn->is_lambda && vs->val.u.fn->lambda_body)
                        collect_reactive_deps(sc, vs->val.u.fn->lambda_body, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
                    else if (vs->val.u.fn->fn_body)
                        collect_stmt_deps(sc, vs->val.u.fn->fn_body, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
                }
            }
            break;
        }
        case EX_LAMBDA:
            collect_reactive_deps(sc, e->u.lambda.body, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_LIST:
        case EX_VEC:
            for (i = 0u; i < e->u.list.count; ++i)
                collect_reactive_deps(sc, e->u.list.items[i], out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_RANGE:
            collect_reactive_deps(sc, e->u.range.lo, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            collect_reactive_deps(sc, e->u.range.hi, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            collect_reactive_deps(sc, e->u.range.step, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_UNARY:
            collect_reactive_deps(sc, e->u.unary.rhs, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_BINARY:
        case EX_PAIR:
            collect_reactive_deps(sc, e->u.binary.lhs, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            collect_reactive_deps(sc, e->u.binary.rhs, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_INDEX:
            collect_reactive_deps(sc, e->u.index.obj, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            collect_reactive_deps(sc, e->u.index.idx, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_FIELD:
            collect_reactive_deps(sc, e->u.field.obj, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_CALL:
            collect_reactive_deps(sc, e->u.call.callee, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            for (i = 0u; i < e->u.call.arg_count; ++i)
                collect_reactive_deps(sc, e->u.call.args[i], out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        case EX_PIPE:
            collect_reactive_deps(sc, e->u.pipe.lhs, out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            for (i = 0u; i < e->u.pipe.arg_count; ++i)
                collect_reactive_deps(sc, e->u.pipe.args[i], out_trks, io_tc, cap_t, out_mobs, io_mc, cap_m, depth + 1);
            break;
        default:
            break;
    }
}

static bool expr_is_reactive(AnimScript *sc, const Expr *e)
{
    AnimTrackerId trks[ANIM_MAX_DEPS];
    AnimId        mobs[ANIM_MAX_DEPS];
    uint32_t      tc = 0u, mc = 0u;
    collect_reactive_deps(sc, e, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    return tc > 0u;
}

/* --------------------------------------------------------------- Evaluator */

static Val eval_expr(AnimScript *sc, const Expr *e);
static void exec_stmt(AnimScript *sc, const Stmt *st);

static Val invoke_script_fn(AnimScript *sc, ScriptFn *fn, const Val *args, uint32_t arg_count, int line)
{
    EnvScope *prev = sc->cur_env;
    VarSlot   local_slots[64];
    EnvScope  loc  = { .vars = local_slots, .count = 0u, .cap = 64u, .parent = sc->global };
    uint32_t  i;
    Val       res  = val_nil();

    for (i = 0u; i < fn->param_count && i < 64u; ++i)
    {
        local_slots[i].name        = fn->params[i];
        local_slots[i].is_reactive = false;
        local_slots[i].react_expr  = NULL;
        local_slots[i].val         = (i < arg_count) ? args[i] : val_nil();
        loc.count++;
    }
    sc->cur_env = &loc;
    if (fn->is_lambda && fn->lambda_body)
    {
        res = eval_expr(sc, fn->lambda_body);
    }
    else if (fn->fn_body)
    {
        bool prev_ret = sc->returning;
        Val  prev_rv  = sc->ret_val;
        sc->returning = false;
        sc->ret_val   = val_nil();
        exec_stmt(sc, fn->fn_body);
        res           = sc->ret_val;
        sc->returning = prev_ret;
        sc->ret_val   = prev_rv;
    }
    sc->cur_env = prev;
    (void)line;
    return res;
}

static float cb_scalar_fn(float x, void *user);
static float cb_implicit_fn(float x, float y, void *user);
static AnimVec3 cb_param_fn(float t, void *user);
static AnimVec3 cb_surface_fn(float u, float v, void *user);

static void eval_domain_expr(AnimScript *sc, const Expr *e0, const Expr *e1, float def0, float def1, float *out0, float *out1, float *out_step)
{
    *out0 = def0;
    *out1 = def1;
    if (e0)
    {
        Val v0 = eval_expr(sc, e0);
        if (v0.kind == VAL_RANGE)
        {
            *out0 = (float)v0.u.range.lo;
            *out1 = (float)v0.u.range.hi;
            if (out_step && v0.u.range.has_step) *out_step = (float)v0.u.range.step;
        }
        else
        {
            *out0 = (float)as_num(sc, v0, def0);
            if (e1) *out1 = (float)as_num(sc, eval_expr(sc, e1), def1);
        }
    }
}

static void script_reactive_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    ReactBinding *rb = (ReactBinding *)user;
    AnimScript   *sc = rb->sc;
    EnvScope     *prev = sc->cur_env;
    (void)dt;
    sc->scene   = s;
    sc->cur_env = sc->global;

    switch (rb->kind)
    {
        case RB_MOVE_TO:
            if (rb->e0)
                anim_move_to(s, target, as_vec(sc, eval_expr(sc, rb->e0), ANIM_ORIGIN));
            break;
        case RB_NEXT_TO:
            if (ANIM_ID_VALID(rb->ref_mob))
                anim_next_to(s, target, rb->ref_mob, rb->dir, rb->f0);
            break;
        case RB_LINE:
            if (rb->e0 && rb->e1)
            {
                AnimVec3 a = as_vec(sc, eval_expr(sc, rb->e0), ANIM_ORIGIN);
                AnimVec3 b = as_vec(sc, eval_expr(sc, rb->e1), ANIM_RIGHT);
                anim_vmob_clear_paths(s, target);
                anim_vmob_begin_subpath(s, target, a);
                anim_vmob_line_to(s, target, b);
            }
            break;
        case RB_ARROW:
            if (rb->e0 && rb->e1)
            {
                AnimVec3 a = as_vec(sc, eval_expr(sc, rb->e0), ANIM_ORIGIN);
                AnimVec3 b = as_vec(sc, eval_expr(sc, rb->e1), ANIM_RIGHT);
                anim__arrow_set(s, target, a, b, rb->f0, rb->f1);
            }
            break;
        case RB_ARC:
            if (rb->e0 && rb->e1 && rb->e2)
            {
                float r  = (float)as_num(sc, eval_expr(sc, rb->e0), 1.0);
                float a0 = (float)as_num(sc, eval_expr(sc, rb->e1), 0.0);
                float da = (float)as_num(sc, eval_expr(sc, rb->e2), ANIM_PI);
                anim__arc_set(s, target, r, a0, da);
            }
            break;
        case RB_RIGHT_ANGLE:
            if (rb->e0 && rb->e1 && rb->e2)
            {
                AnimVec3 a = as_vec(sc, eval_expr(sc, rb->e0), ANIM_LEFT);
                AnimVec3 v = as_vec(sc, eval_expr(sc, rb->e1), ANIM_ORIGIN);
                AnimVec3 b = as_vec(sc, eval_expr(sc, rb->e2), ANIM_UP);
                AnimVec3 d1 = ANIM_V3(a.x - v.x, a.y - v.y, a.z - v.z);
                AnimVec3 d2 = ANIM_V3(b.x - v.x, b.y - v.y, b.z - v.z);
                float l1 = sqrtf(d1.x*d1.x + d1.y*d1.y + d1.z*d1.z);
                float l2 = sqrtf(d2.x*d2.x + d2.y*d2.y + d2.z*d2.z);
                if (l1 > 1e-5f && l2 > 1e-5f)
                {
                    AnimVec3 u1 = ANIM_V3(d1.x / l1 * rb->f0, d1.y / l1 * rb->f0, d1.z / l1 * rb->f0);
                    AnimVec3 u2 = ANIM_V3(d2.x / l2 * rb->f0, d2.y / l2 * rb->f0, d2.z / l2 * rb->f0);
                    AnimVec3 p1 = ANIM_V3(v.x + u1.x, v.y + u1.y, v.z + u1.z);
                    AnimVec3 p2 = ANIM_V3(p1.x + u2.x, p1.y + u2.y, p1.z + u2.z);
                    AnimVec3 p3 = ANIM_V3(v.x + u2.x, v.y + u2.y, v.z + u2.z);
                    anim_vmob_clear_paths(s, target);
                    anim_vmob_begin_subpath(s, target, p1);
                    anim_vmob_line_to(s, target, p2);
                    anim_vmob_line_to(s, target, p3);
                }
            }
            break;
        case RB_DECIMAL:
            if (rb->e0)
                anim_decimal_number_set(s, target, (float)as_num(sc, eval_expr(sc, rb->e0), 0.0));
            break;
        case RB_PLOT:
            if (rb->cb_ctx)
            {
                float x0, x1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &x0, &x1, NULL);
                fresh = anim_plot(s, rb->ref_mob, cb_scalar_fn, x0, x1, rb->u0, rb->cb_ctx);
                anim__replace_from(s, target, fresh, false);
            }
            break;
        case RB_AREA:
            if (rb->cb_ctx)
            {
                float x0, x1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &x0, &x1, NULL);
                fresh = anim_area_under_curve(s, rb->ref_mob, cb_scalar_fn, x0, x1, rb->u0, rb->cb_ctx);
                anim__replace_from(s, target, fresh, false);
            }
            break;
        case RB_RIEMANN:
            if (rb->cb_ctx)
            {
                float x0, x1, dx = rb->f2;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &x0, &x1, &dx);
                if (rb->e2) dx = (float)as_num(sc, eval_expr(sc, rb->e2), dx);
                fresh = anim_riemann_rectangles(s, rb->ref_mob, cb_scalar_fn, x0, x1, dx, rb->cb_ctx, rb->c0, rb->c1);
                anim__replace_from(s, target, fresh, true);
            }
            break;
        case RB_IMPLICIT:
            if (rb->cb_ctx)
            {
                float x0, x1, y0, y1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &x0, &x1, NULL);
                eval_domain_expr(sc, rb->e2, rb->e3, rb->f2, rb->f3, &y0, &y1, NULL);
                fresh = anim_implicit_plot(s, rb->ref_mob, cb_implicit_fn, x0, x1, y0, y1, rb->u0, rb->u1, rb->cb_ctx);
                anim__replace_from(s, target, fresh, false);
            }
            break;
        case RB_CONTOUR:
            if (rb->cb_ctx)
            {
                float x0, x1, y0, y1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &x0, &x1, NULL);
                eval_domain_expr(sc, rb->e2, rb->e3, rb->f2, rb->f3, &y0, &y1, NULL);
                fresh = anim_contour_plot(s, rb->ref_mob, cb_implicit_fn, x0, x1, y0, y1, rb->u0, rb->u0, rb->levels, rb->u1, rb->c0, rb->c1, rb->cb_ctx);
                anim__replace_from(s, target, fresh, true);
            }
            break;
        case RB_POLAR:
            if (rb->cb_ctx)
            {
                float t0, t1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &t0, &t1, NULL);
                fresh = anim_plot_polar(s, rb->ref_mob, cb_scalar_fn, t0, t1, rb->u0, rb->cb_ctx);
                anim__replace_from(s, target, fresh, false);
            }
            break;
        case RB_PARAM:
            if (rb->cb_ctx)
            {
                float t0, t1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &t0, &t1, NULL);
                fresh = anim_parametric_curve(s, cb_param_fn, t0, t1, rb->u0, rb->cb_ctx);
                anim__replace_from(s, target, fresh, false);
            }
            break;
        case RB_SURFACE:
            if (rb->cb_ctx)
            {
                float u0, u1, v0, v1;
                AnimId fresh;
                eval_domain_expr(sc, rb->e0, rb->e1, rb->f0, rb->f1, &u0, &u1, NULL);
                eval_domain_expr(sc, rb->e2, rb->e3, rb->f2, rb->f3, &v0, &v1, NULL);
                fresh = anim_surface3d(s, cb_surface_fn, NULL, u0, u1, v0, v1, rb->u0, rb->u1, rb->cb_ctx);
                anim__replace_from(s, target, fresh, false);
            }
            break;
        case RB_PHYS_SPRING:
            if (rb->e0 && rb->e1)
                anim_phys_spring_set(s, target, as_vec(sc, eval_expr(sc, rb->e0), ANIM_ORIGIN),
                                     as_vec(sc, eval_expr(sc, rb->e1), ANIM_RIGHT), rb->u0, rb->f0);
            break;
        case RB_PHYS_PENDULUM:
            if (rb->e0 || rb->e1)
                anim_phys_pendulum_set(s, target, rb->e0 ? as_vec(sc, eval_expr(sc, rb->e0), rb->dir) : rb->dir,
                                       rb->f0, rb->e1 ? (float)as_num(sc, eval_expr(sc, rb->e1), rb->f1) : rb->f1);
            break;
        case RB_PHYS_WAVE:
            if (rb->e0 || rb->e1 || rb->e2)
            {
                float amp   = rb->e0 ? (float)as_num(sc, eval_expr(sc, rb->e0), rb->f0) : rb->f0;
                float cyc   = rb->e1 ? (float)as_num(sc, eval_expr(sc, rb->e1), rb->f1) : rb->f1;
                float phase = rb->e2 ? (float)as_num(sc, eval_expr(sc, rb->e2), rb->f2) : rb->f2;
                anim_phys_wave_set(s, target, rb->dir, (AnimVec3){ rb->f3, rb->c0.r, rb->c0.g }, amp, cyc, phase, rb->u0);
            }
            break;
    }
    sc->cur_env = prev;
}

static void attach_react_plot(AnimScript *sc, AnimId target, ReactBindKind kind,
                              AnimId ref_mob, ScriptCbCtx *cb_ctx,
                              const Expr *e0, const Expr *e1, const Expr *e2, const Expr *e3,
                              float f0, float f1, float f2, float f3,
                              uint32_t u0, uint32_t u1, AnimColor c0, AnimColor c1, float *levels)
{
    AnimTrackerId trks[ANIM_MAX_DEPS];
    AnimId        mobs[ANIM_MAX_DEPS];
    uint32_t      tc = 0u, mc = 0u;
    ReactBinding *rb;

    if (cb_ctx && cb_ctx->fn)
    {
        if (cb_ctx->fn->is_lambda && cb_ctx->fn->lambda_body)
            collect_reactive_deps(sc, cb_ctx->fn->lambda_body, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
        else if (cb_ctx->fn->fn_body)
            collect_stmt_deps(sc, cb_ctx->fn->fn_body, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    }
    collect_reactive_deps(sc, e0, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    collect_reactive_deps(sc, e1, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    collect_reactive_deps(sc, e2, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    collect_reactive_deps(sc, e3, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    if (tc == 0u)
        return;

    if (ANIM_ID_VALID(ref_mob) && mc < ANIM_MAX_DEPS)
        mobs[mc++] = ref_mob;

    rb = (ReactBinding *)sc_alloc(sc, sizeof *rb);
    rb->sc      = sc;
    rb->kind    = kind;
    rb->target  = target;
    rb->ref_mob = ref_mob;
    rb->cb_ctx  = cb_ctx;
    rb->e0      = (Expr *)e0;
    rb->e1      = (Expr *)e1;
    rb->e2      = (Expr *)e2;
    rb->e3      = (Expr *)e3;
    rb->f0      = f0;
    rb->f1      = f1;
    rb->f2      = f2;
    rb->f3      = f3;
    rb->u0      = u0;
    rb->u1      = u1;
    rb->c0      = c0;
    rb->c1      = c1;
    rb->levels  = levels;
    anim_add_updater(sc->scene, target, script_reactive_updater, rb, mobs, mc, trks, tc);
}

static void attach_react(AnimScript *sc, AnimId target, ReactBindKind kind,
                         const Expr *e0, const Expr *e1, const Expr *e2,
                         AnimId ref_mob, AnimVec3 dir, float f0, float f1)
{
    AnimTrackerId trks[ANIM_MAX_DEPS];
    AnimId        mobs[ANIM_MAX_DEPS];
    uint32_t      tc = 0u, mc = 0u;
    ReactBinding *rb;

    collect_reactive_deps(sc, e0, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    collect_reactive_deps(sc, e1, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    collect_reactive_deps(sc, e2, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
    if (ANIM_ID_VALID(ref_mob) && mc < ANIM_MAX_DEPS)
        mobs[mc++] = ref_mob;
    if (tc == 0u && !ANIM_ID_VALID(ref_mob))
        return;

    rb = (ReactBinding *)sc_alloc(sc, sizeof *rb);
    rb->sc      = sc;
    rb->kind    = kind;
    rb->target  = target;
    rb->ref_mob = ref_mob;
    rb->e0      = (Expr *)e0;
    rb->e1      = (Expr *)e1;
    rb->e2      = (Expr *)e2;
    rb->dir     = dir;
    rb->f0      = f0;
    rb->f1      = f1;
    anim_add_updater(sc->scene, target, script_reactive_updater, rb, mobs, mc, trks, tc);
}

/* C callback adapters for script functions/lambdas */
static float cb_scalar_fn(float x, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    Val arg = val_num((double)x);
    return (float)as_num(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, &arg, 1u, 0), 0.0);
}

static float cb_implicit_fn(float x, float y, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    Val args[2] = { val_num((double)x), val_num((double)y) };
    return (float)as_num(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 2u, 0), 0.0);
}

static AnimVec3 cb_field_fn(AnimVec3 p, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    if (ctx->fn->param_count == 1u)
    {
        Val arg = val_vec(p);
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, &arg, 1u, 0), ANIM_ORIGIN);
    }
    else
    {
        Val args[3] = { val_num((double)p.x), val_num((double)p.y), val_num((double)p.z) };
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 3u, 0), ANIM_ORIGIN);
    }
}

static AnimVec3 cb_param_fn(float t, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    Val arg = val_num((double)t);
    return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, &arg, 1u, 0), ANIM_ORIGIN);
}

static AnimVec3 cb_surface_fn(float u, float v, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    Val args[2] = { val_num((double)u), val_num((double)v) };
    return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 2u, 0), ANIM_ORIGIN);
}

static AnimVec3 cb_warp_fn(AnimVec3 p, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    if (ctx->fn->param_count == 1u)
    {
        Val arg = val_vec(p);
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, &arg, 1u, 0), p);
    }
    else
    {
        Val args[3] = { val_num((double)p.x), val_num((double)p.y), val_num((double)p.z) };
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 3u, 0), p);
    }
}

static AnimComplex cb_complex_fn(AnimComplex z, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    AnimVec3     v;
    if (ctx->fn->param_count == 1u)
    {
        Val arg = val_vec(ANIM_V3(z.re, z.im, 0.0f));
        v = as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, &arg, 1u, 0), ANIM_V3(z.re, z.im, 0.0f));
    }
    else
    {
        Val args[2] = { val_num((double)z.re), val_num((double)z.im) };
        v = as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 2u, 0), ANIM_V3(z.re, z.im, 0.0f));
    }
    return (AnimComplex){ v.x, v.y };
}

static AnimColor cb_pixel_fn(uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    if (ctx->fn->param_count <= 2u)
    {
        Val args[2] = { val_num((double)x), val_num((double)y) };
        return as_col(invoke_script_fn(ctx->sc, ctx->fn, args, 2u, 0), ANIM_WHITE);
    }
    else
    {
        Val args[4] = { val_num((double)x), val_num((double)y), val_num((double)w), val_num((double)h) };
        return as_col(invoke_script_fn(ctx->sc, ctx->fn, args, 4u, 0), ANIM_WHITE);
    }
}

static AnimVec3 cb_homotopy_fn(AnimVec3 p, float t, void *user)
{
    ScriptCbCtx *ctx = (ScriptCbCtx *)user;
    if (ctx->fn->param_count <= 2u)
    {
        Val args[2] = { val_vec(p), val_num((double)t) };
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 2u, 0), p);
    }
    else
    {
        Val args[4] = { val_num((double)p.x), val_num((double)p.y), val_num((double)p.z), val_num((double)t) };
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->fn, args, 4u, 0), p);
    }
}

typedef struct ScriptOdeCtx {
    AnimScript *sc;
    ScriptFn   *ode_fn;
    ScriptFn   *proj_fn;
} ScriptOdeCtx;

static void cb_ode_fn(float t, const float *y, float *dydt, uint32_t dim, void *user)
{
    ScriptOdeCtx *ctx = (ScriptOdeCtx *)user;
    Val yitems[64];
    Val ylist = { .kind = VAL_LIST };
    Val args[2];
    Val res;
    uint32_t n = MU_MIN(dim, 64u);
    for (uint32_t i = 0u; i < n; ++i) yitems[i] = val_num((double)y[i]);
    ylist.u.list.items = yitems;
    ylist.u.list.count = n;
    args[0] = val_num((double)t);
    args[1] = ylist;
    res = invoke_script_fn(ctx->sc, ctx->ode_fn, args, 2u, 0);
    for (uint32_t i = 0u; i < dim; ++i)
        dydt[i] = (res.kind == VAL_LIST && i < res.u.list.count) ? (float)as_num(ctx->sc, res.u.list.items[i], 0.0) : 0.0f;
}

static AnimVec3 cb_ode_proj_fn(const float *y, uint32_t dim, void *user)
{
    ScriptOdeCtx *ctx = (ScriptOdeCtx *)user;
    if (ctx->proj_fn)
    {
        Val yitems[64];
        Val ylist = { .kind = VAL_LIST };
        uint32_t n = MU_MIN(dim, 64u);
        for (uint32_t i = 0u; i < n; ++i) yitems[i] = val_num((double)y[i]);
        ylist.u.list.items = yitems;
        ylist.u.list.count = n;
        return as_vec(ctx->sc, invoke_script_fn(ctx->sc, ctx->proj_fn, &ylist, 1u, 0), ANIM_ORIGIN);
    }
    return ANIM_V3(dim > 0u ? y[0] : 0.0f, dim > 1u ? y[1] : 0.0f, dim > 2u ? y[2] : 0.0f);
}

static void set_stroke_width_rec(AnimScene *s, AnimId id, float w)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t c;
    if (!h)
        return;
    h->stroke_width = w;
    c = h->first_child;
    while (c != 0u)
    {
        set_stroke_width_rec(s, (AnimId){ c, s->hot[c].gen }, w);
        c = s->hot[c].next_sibling;
    }
}

static ScriptCbCtx *make_cb_ctx(AnimScript *sc, ScriptFn *fn)
{
    ScriptCbCtx *c = (ScriptCbCtx *)sc_alloc(sc, sizeof *c);
    c->sc = sc;
    c->fn = fn;
    return c;
}

static void ensure_in_scene(AnimScript *sc, AnimId id)
{
    const VMobHot *h = anim__hot_const(sc->scene, id);
    if (h && !h->in_scene)
        anim_add(sc->scene, id);
}

static void push_play_spec(AnimScript *sc, AnimSpec sp)
{
    if (sc->in_play && sc->play_count < SC_MAX_SPECS)
        sc->play_specs[sc->play_count++] = sp;
}

static bool str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static bool arg_find_kw(const char *const *arg_names, uint32_t ac, const char *key, uint32_t *out_idx)
{
    uint32_t i;
    if (!arg_names || !key) return false;
    for (i = 0u; i < ac; ++i)
        if (arg_names[i] && strcmp(arg_names[i], key) == 0) { *out_idx = i; return true; }
    return false;
}

static bool arg_get(const char *const *arg_names, const Val *args, const Expr *const *raw_args,
                    uint32_t ac, uint32_t pos_idx, const char *key, Val *out_v, const Expr **out_raw)
{
    uint32_t i, pos = 0u;
    if (key && arg_find_kw(arg_names, ac, key, &i))
    {
        if (out_v)   *out_v   = args[i];
        if (out_raw) *out_raw = raw_args ? raw_args[i] : NULL;
        return true;
    }
    for (i = 0u; i < ac; ++i)
    {
        if (!arg_names || !arg_names[i])
        {
            if (pos == pos_idx)
            {
                if (out_v)   *out_v   = args[i];
                if (out_raw) *out_raw = raw_args ? raw_args[i] : NULL;
                return true;
            }
            pos++;
        }
    }
    return false;
}

static void push_play_spec_ex(AnimScript *sc, AnimSpec sp, const char *const *arg_names, const Val *args, uint32_t ac)
{
    Val vr, vl;
    if (arg_get(arg_names, args, NULL, ac, 99u, "rate", &vr, NULL))
        sp = anim_spec_with_rate(sp, as_rate(vr, ANIM_RATE_SMOOTH));
    if (arg_get(arg_names, args, NULL, ac, 99u, "lag", &vl, NULL))
        sp = anim_spec_with_lag(sp, (float)as_num(sc, vl, -1.0));
    push_play_spec(sc, sp);
}

static Val eval_builtin(AnimScript *sc, const char *name, const Expr *const *raw_args,
                        const char *const *arg_names, const Val *args, uint32_t ac, int line)
{
    AnimScene *s = sc->scene;

    /* ---- Math & Vector / Color Helpers ---- */
    if (str_eq(name, "sin") && ac >= 1)   return val_num(sin(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "cos") && ac >= 1)   return val_num(cos(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "tan") && ac >= 1)   return val_num(tan(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "asin") && ac >= 1)  return val_num(asin(MU_CLAMP(as_num(sc, args[0], 0.0), -1.0, 1.0)));
    if (str_eq(name, "acos") && ac >= 1)  return val_num(acos(MU_CLAMP(as_num(sc, args[0], 0.0), -1.0, 1.0)));
    if (str_eq(name, "atan") && ac >= 1)  return val_num(atan(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "atan2") && ac >= 2) return val_num(atan2(as_num(sc, args[0], 0.0), as_num(sc, args[1], 1.0)));
    if (str_eq(name, "sqrt") && ac >= 1)  return val_num(sqrt(MU_MAX(as_num(sc, args[0], 0.0), 0.0)));
    if (str_eq(name, "abs") && ac >= 1)   return val_num(fabs(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "floor") && ac >= 1) return val_num(floor(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "ceil") && ac >= 1)  return val_num(ceil(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "round") && ac >= 1) return val_num(round(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "exp") && ac >= 1)   return val_num(exp(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "log") && ac >= 1)   return val_num(log(MU_MAX(as_num(sc, args[0], 1.0), 1e-12)));
    if (str_eq(name, "pow") && ac >= 2)   return val_num(pow(as_num(sc, args[0], 0.0), as_num(sc, args[1], 1.0)));
    if (str_eq(name, "min") && ac >= 2)   return val_num(MU_MIN(as_num(sc, args[0], 0.0), as_num(sc, args[1], 0.0)));
    if (str_eq(name, "max") && ac >= 2)   return val_num(MU_MAX(as_num(sc, args[0], 0.0), as_num(sc, args[1], 0.0)));
    if (str_eq(name, "clamp") && ac >= 3) return val_num(MU_CLAMP(as_num(sc, args[0], 0.0), as_num(sc, args[1], 0.0), as_num(sc, args[2], 1.0)));
    if (str_eq(name, "rad") && ac >= 1)   return val_num(as_num(sc, args[0], 0.0) * (double)ANIM_PI / 180.0);
    if (str_eq(name, "deg") && ac >= 1)   return val_num(as_num(sc, args[0], 0.0) * 180.0 / (double)ANIM_PI);
    if (str_eq(name, "lerp") && ac >= 3)
    {
        double t = as_num(sc, args[2], 0.0);
        if (args[0].kind == VAL_VEC || args[1].kind == VAL_VEC)
        {
            AnimVec3 a = as_vec(sc, args[0], ANIM_ORIGIN);
            AnimVec3 b = as_vec(sc, args[1], ANIM_ORIGIN);
            return val_vec(ANIM_V3(a.x + (b.x - a.x) * (float)t, a.y + (b.y - a.y) * (float)t, a.z + (b.z - a.z) * (float)t));
        }
        return val_num(as_num(sc, args[0], 0.0) + (as_num(sc, args[1], 0.0) - as_num(sc, args[0], 0.0)) * t);
    }
    if (str_eq(name, "smoothstep") && ac >= 3)
    {
        double e0 = as_num(sc, args[0], 0.0), e1 = as_num(sc, args[1], 1.0), x = as_num(sc, args[2], 0.0);
        double t  = MU_CLAMP((x - e0) / (fabs(e1 - e0) > 1e-9 ? (e1 - e0) : 1.0), 0.0, 1.0);
        return val_num(t * t * (3.0 - 2.0 * t));
    }
    if (str_eq(name, "erf") && ac >= 1)    return val_num(erf(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "erfc") && ac >= 1)   return val_num(erfc(as_num(sc, args[0], 0.0)));
    if (str_eq(name, "gamma") && ac >= 1)  return val_num(tgamma(as_num(sc, args[0], 1.0)));
    if (str_eq(name, "lgamma") && ac >= 1) return val_num(lgamma(as_num(sc, args[0], 1.0)));
    if (str_eq(name, "norm_pdf") && ac >= 1)
    {
        double x = as_num(sc, args[0], 0.0);
        double mu = ac >= 2 ? as_num(sc, args[1], 0.0) : 0.0;
        double sd = ac >= 3 ? MU_MAX(as_num(sc, args[2], 1.0), 1e-6) : 1.0;
        double z  = (x - mu) / sd;
        return val_num(exp(-0.5 * z * z) / (sd * sqrt(2.0 * (double)ANIM_PI)));
    }
    if (str_eq(name, "norm_cdf") && ac >= 1)
    {
        double x = as_num(sc, args[0], 0.0);
        double mu = ac >= 2 ? as_num(sc, args[1], 0.0) : 0.0;
        double sd = ac >= 3 ? MU_MAX(as_num(sc, args[2], 1.0), 1e-6) : 1.0;
        return val_num(0.5 * (1.0 + erf((x - mu) / (sd * sqrt(2.0)))));
    }
    if (str_eq(name, "seed") && ac >= 1)
    {
        uint64_t z = (uint64_t)(int64_t)as_num(sc, args[0], 1.0) + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        sc->rng_state = z ^ (z >> 31);
        if (sc->rng_state == 0ULL) sc->rng_state = 1ULL;
        return val_nil();
    }
    if (str_eq(name, "rand"))
    {
        uint64_t x = sc->rng_state ? sc->rng_state : 0x9e3779b97f4a7c15ULL;
        double u;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        sc->rng_state = x;
        u = (double)((x * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
        if (ac == 1) return val_num(u * as_num(sc, args[0], 1.0));
        if (ac >= 2) { double lo = as_num(sc, args[0], 0.0), hi = as_num(sc, args[1], 1.0); return val_num(lo + u * (hi - lo)); }
        return val_num(u);
    }
    if (str_eq(name, "randn"))
    {
        double u1 = 0.0, u2 = 0.0, z;
        for (int k = 0; k < 2; ++k)
        {
            uint64_t x = sc->rng_state ? sc->rng_state : 0x9e3779b97f4a7c15ULL;
            double u;
            x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
            sc->rng_state = x;
            u = (double)((x * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
            if (k == 0) u1 = MU_MAX(u, 1e-12);
            else        u2 = u;
        }
        z = sqrt(-2.0 * log(u1)) * cos((double)ANIM_TAU * u2);
        if (ac >= 1)
        {
            double mu = as_num(sc, args[0], 0.0);
            double sd = ac >= 2 ? as_num(sc, args[1], 1.0) : 1.0;
            return val_num(mu + sd * z);
        }
        return val_num(z);
    }
    if (str_eq(name, "length") && ac >= 1)
    {
        AnimVec3 v = as_vec(sc, args[0], ANIM_ORIGIN);
        return val_num(sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z));
    }
    if (str_eq(name, "norm") && ac >= 1)
    {
        AnimVec3 v = as_vec(sc, args[0], ANIM_ORIGIN);
        float    l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        return val_vec(l > 1e-6f ? ANIM_V3(v.x / l, v.y / l, v.z / l) : ANIM_ORIGIN);
    }
    if (str_eq(name, "cross") && ac >= 2)
    {
        AnimVec3 a = as_vec(sc, args[0], ANIM_ORIGIN), b = as_vec(sc, args[1], ANIM_ORIGIN);
        return val_vec(ANIM_V3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x));
    }
    if ((str_eq(name, "rgb") || str_eq(name, "rgba")) && ac >= 3)
    {
        float r = (float)as_num(sc, args[0], 1.0);
        float g = (float)as_num(sc, args[1], 1.0);
        float b = (float)as_num(sc, args[2], 1.0);
        float a = ac >= 4 ? (float)as_num(sc, args[3], 1.0) : 1.0f;
        return val_col(anim_srgb_to_linear(r, g, b, a));
    }
    if (str_eq(name, "lerp_color") && ac >= 3)
        return val_col(anim_color_lerp_oklab(as_col(args[0], ANIM_WHITE), as_col(args[1], ANIM_WHITE), (float)as_num(sc, args[2], 0.5)));

    /* ---- Scene & Layout ---- */
    if (str_eq(name, "show"))
    {
        uint32_t i;
        for (i = 0u; i < ac; ++i)
        {
            if (args[i].kind == VAL_MOB && ANIM_ID_VALID(args[i].u.mob))
            {
                anim_add(s, args[i].u.mob);
                anim_set_visible(s, args[i].u.mob, true, true);
            }
            else if (args[i].kind == VAL_LIST)
            {
                uint32_t j;
                for (j = 0u; j < args[i].u.list.count; ++j)
                {
                    AnimId mid = as_mob(args[i].u.list.items[j]);
                    if (ANIM_ID_VALID(mid)) { anim_add(s, mid); anim_set_visible(s, mid, true, true); }
                }
            }
        }
        return ac > 0 ? args[0] : val_nil();
    }
    if (str_eq(name, "hide"))
    {
        for (uint32_t i = 0u; i < ac; ++i)
            if (ANIM_ID_VALID(as_mob(args[i])))
                anim_set_visible(s, as_mob(args[i]), false, true);
        return val_nil();
    }
    if (str_eq(name, "remove"))
    {
        for (uint32_t i = 0u; i < ac; ++i)
            if (ANIM_ID_VALID(as_mob(args[i])))
                anim_remove(s, as_mob(args[i]));
        return val_nil();
    }
    if (str_eq(name, "clone") && ac >= 1)
        return val_mob(anim_vmob_clone(s, as_mob(args[0])));
    if (str_eq(name, "group"))
    {
        AnimId buf[128];
        uint32_t n = 0u;
        for (uint32_t i = 0u; i < ac && n < 128u; ++i)
        {
            if (args[i].kind == VAL_MOB) buf[n++] = args[i].u.mob;
            else if (args[i].kind == VAL_LIST)
                for (uint32_t j = 0u; j < args[i].u.list.count && n < 128u; ++j)
                    if (args[i].u.list.items[j].kind == VAL_MOB)
                        buf[n++] = args[i].u.list.items[j].u.mob;
        }
        return val_mob(anim_group(s, buf, n));
    }
    if (str_eq(name, "arrange") && ac >= 1)
    {
        AnimId buf[128];
        uint32_t n = 0u;
        AnimVec3 dir = ac >= 2 ? as_vec(sc, args[1], ANIM_RIGHT) : ANIM_RIGHT;
        float buff = ac >= 3 ? (float)as_num(sc, args[2], 0.25) : 0.25f;
        if (args[0].kind == VAL_LIST)
            for (uint32_t j = 0u; j < args[0].u.list.count && n < 128u; ++j)
                buf[n++] = as_mob(args[0].u.list.items[j]);
        else if (args[0].kind == VAL_MOB)
        {
            uint32_t cc = anim_child_count(s, args[0].u.mob);
            for (uint32_t j = 0u; j < cc && n < 128u; ++j)
                buf[n++] = anim_child_at(s, args[0].u.mob, j);
        }
        anim_arrange(s, buf, n, dir, buff);
        return args[0];
    }
    if (str_eq(name, "arrange_grid") && ac >= 3)
    {
        AnimId buf[128];
        uint32_t n = 0u;
        uint32_t rows = (uint32_t)as_num(sc, args[1], 1.0);
        uint32_t cols = (uint32_t)as_num(sc, args[2], 1.0);
        float hb = ac >= 4 ? (float)as_num(sc, args[3], 0.25) : 0.25f;
        float vb = ac >= 5 ? (float)as_num(sc, args[4], 0.25) : 0.25f;
        if (args[0].kind == VAL_LIST)
            for (uint32_t j = 0u; j < args[0].u.list.count && n < 128u; ++j)
                buf[n++] = as_mob(args[0].u.list.items[j]);
        anim_arrange_in_grid(s, buf, n, rows, cols, hb, vb);
        return args[0];
    }
    if (str_eq(name, "camera"))
    {
        AnimCamera cam = anim_scene_camera(s);
        Val f;
        if (ac >= 1 && args[0].kind == VAL_BAG)
            return val_cam(bag_to_camera(sc, args[0], cam));
        if (arg_get(arg_names, args, raw_args, ac, 0u, "pos", &f, NULL))    cam.pos = as_vec(sc, f, cam.pos);
        if (arg_get(arg_names, args, raw_args, ac, 1u, "target", &f, NULL)) cam.target = as_vec(sc, f, cam.target);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "up", &f, NULL))    cam.up = as_vec(sc, f, cam.up);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "fov", &f, NULL))   cam.fov_deg = (float)as_num(sc, f, cam.fov_deg);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "zoom", &f, NULL))  cam.zoom = (float)as_num(sc, f, cam.zoom);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "ortho", &f, NULL)) cam.ortho_blend = (float)as_num(sc, f, cam.ortho_blend);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "frame_height", &f, NULL)) cam.frame_height = (float)as_num(sc, f, cam.frame_height);
        return val_cam(cam);
    }
    if (str_eq(name, "ambient_camera") && ac >= 1)
    {
        anim_scene_begin_ambient_camera_rotation(s, (float)as_num(sc, args[0], 0.2), ac >= 2 ? (float)as_num(sc, args[1], 0.0) : 0.0f);
        return val_nil();
    }
    if (str_eq(name, "stop_ambient_camera"))
    {
        anim_scene_stop_ambient_camera_rotation(s);
        return val_nil();
    }
    if (str_eq(name, "illusion_camera") && ac >= 1)
    {
        anim_scene_begin_illusion_camera_rotation(s, (float)as_num(sc, args[0], 0.4), ac >= 2 ? (float)as_num(sc, args[1], 0.15) : 0.15f);
        return val_nil();
    }
    if (str_eq(name, "next_section"))
    {
        anim_timeline_next_section(sc->tl, (ac >= 1 && as_str(args[0])) ? as_str(args[0]) : "section", ac >= 2 ? as_bool(sc, args[1]) : false);
        return val_nil();
    }
    if (str_eq(name, "clear"))
    {
        anim_scene_clear(s);
        return val_nil();
    }
    if (str_eq(name, "clear_lights"))
    {
        anim_scene_clear_lights(s);
        return val_nil();
    }
    if (str_eq(name, "add_light") && ac >= 3)
    {
        const char *ks = as_str(args[0]) ? as_str(args[0]) : "directional";
        AnimLightKind lk = str_eq(ks, "point") ? ANIM_LIGHT_POINT : ANIM_LIGHT_DIRECTIONAL;
        AnimVec3 pd = as_vec(sc, args[1], ANIM_V3(0.45f, 0.72f, 0.53f));
        AnimColor col = as_col(args[2], ANIM_WHITE);
        float inten = ac >= 4 ? (float)as_num(sc, args[3], 1.0) : 1.0f;
        anim_scene_add_light(s, lk, pd, col, inten);
        return val_nil();
    }
    if (str_eq(name, "ambient_light") && ac >= 1)
    {
        anim_scene_set_ambient_light(s, as_col(args[0], anim_rgba(0.18f, 0.22f, 0.30f, 1.0f)),
                                     ac >= 2 ? (float)as_num(sc, args[1], 1.0) : 1.0f);
        return val_nil();
    }
    if (str_eq(name, "set_fog") && ac >= 3)
    {
        AnimFogDesc fg = {
            .enabled     = true,
            .exponential = ac >= 5 ? as_bool(sc, args[4]) : false,
            .color       = as_col(args[0], anim_rgba(0.05f, 0.08f, 0.14f, 1.0f)),
            .start       = (float)as_num(sc, args[1], 4.0),
            .end         = (float)as_num(sc, args[2], 24.0),
            .density     = ac >= 4 ? (float)as_num(sc, args[3], 0.08) : 0.08f,
        };
        anim_scene_set_fog(s, &fg);
        return val_nil();
    }
    if (str_eq(name, "clear_fog"))
    {
        anim_scene_set_fog(s, NULL);
        return val_nil();
    }
    if (str_eq(name, "camera_save"))
    {
        anim_scene_camera_save(s);
        return val_nil();
    }
    if (str_eq(name, "camera_restore"))
    {
        anim_scene_camera_restore(s);
        return val_nil();
    }
    if (str_eq(name, "camera_frame") && ac >= 1)
    {
        float pad = ac >= 2 ? (float)as_num(sc, args[1], 0.6) : 0.6f;
        float asp = ac >= 3 ? (float)as_num(sc, args[2], 16.0 / 9.0) : (16.0f / 9.0f);
        anim_scene_set_camera(s, anim_camera_frame_mob(s, as_mob(args[0]), pad, asp));
        return val_nil();
    }
    if (str_eq(name, "camera_spherical") && ac >= 4)
    {
        anim_scene_camera_set_spherical(s, as_vec(sc, args[0], ANIM_ORIGIN),
                                        (float)as_num(sc, args[1], 8.0),
                                        (float)as_num(sc, args[2], 1.1),
                                        (float)as_num(sc, args[3], 0.0));
        return val_nil();
    }
    if (str_eq(name, "arc_length") && ac >= 1)
        return val_num((double)anim_vmob_arc_length(s, as_mob(args[0])));
    if (str_eq(name, "point_at") && ac >= 2)
        return val_vec(anim_vmob_point_at(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0)));
    if (str_eq(name, "tangent_at") && ac >= 2)
        return val_vec(anim_vmob_tangent_at(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0)));
    if (str_eq(name, "fixed_dt") && ac >= 1)
    {
        anim_timeline_set_fixed_dt(sc->tl, (float)as_num(sc, args[0], 1.0 / 60.0));
        return val_nil();
    }
    if (str_eq(name, "scene_time"))
        return val_num((double)anim_scene_time(s));
    if (str_eq(name, "viewport"))
    {
        AnimCamera cam = anim_scene_camera(s);
        float rect[4] = { 0.64f, 0.04f, 0.96f, 0.36f };
        AnimColor border = ANIM_YELLOW;
        float bw = 2.0f;
        Val f;
        if (arg_get(arg_names, args, raw_args, ac, 99u, "cam", &f, NULL) && f.kind == VAL_CAM) cam = f.u.cam;
        if (arg_get(arg_names, args, raw_args, ac, 99u, "pos", &f, NULL))    cam.pos = as_vec(sc, f, cam.pos);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "target", &f, NULL)) cam.target = as_vec(sc, f, cam.target);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "up", &f, NULL))     cam.up = as_vec(sc, f, cam.up);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "fov", &f, NULL))    cam.fov_deg = (float)as_num(sc, f, cam.fov_deg);
        if (arg_get(arg_names, args, raw_args, ac, 0u,  "rect", &f, NULL) && f.kind == VAL_LIST && f.u.list.count >= 4u)
            for (int k = 0; k < 4; ++k) rect[k] = (float)as_num(sc, f.u.list.items[k], rect[k]);
        if (arg_get(arg_names, args, raw_args, ac, 1u,  "border", &f, NULL)) border = as_col(f, border);
        if (arg_get(arg_names, args, raw_args, ac, 2u,  "width", &f, NULL))  bw = (float)as_num(sc, f, bw);
        return val_num((double)anim_scene_add_viewport(s, cam, rect, border, bw));
    }

    /* ---- 2D Geometry Builders ---- */
    if (str_eq(name, "circle"))
        return val_mob(anim_circle(s, ac >= 1 ? (float)as_num(sc, args[0], 1.0) : 1.0f));
    if (str_eq(name, "arc") && ac >= 3)
    {
        AnimId id = anim_arc(s, (float)as_num(sc, args[0], 1.0), (float)as_num(sc, args[1], 0.0), (float)as_num(sc, args[2], ANIM_PI));
        if (expr_is_reactive(sc, raw_args[0]) || expr_is_reactive(sc, raw_args[1]) || expr_is_reactive(sc, raw_args[2]))
            attach_react(sc, id, RB_ARC, raw_args[0], raw_args[1], raw_args[2], ANIM_ID_NIL, ANIM_ORIGIN, 0.0f, 0.0f);
        return val_mob(id);
    }
    if (str_eq(name, "annulus") && ac >= 2)
        return val_mob(anim_annulus(s, (float)as_num(sc, args[0], 0.5), (float)as_num(sc, args[1], 1.0)));
    if (str_eq(name, "sector") && ac >= 4)
        return val_mob(anim_sector(s, (float)as_num(sc, args[0], 0.0), (float)as_num(sc, args[1], 1.0),
                                   (float)as_num(sc, args[2], 0.0), (float)as_num(sc, args[3], ANIM_PI * 0.5)));
    if (str_eq(name, "ellipse") && ac >= 2)
        return val_mob(anim_ellipse(s, (float)as_num(sc, args[0], 2.0), (float)as_num(sc, args[1], 1.0)));
    if (str_eq(name, "rect") && ac >= 2)
        return val_mob(anim_rect(s, (float)as_num(sc, args[0], 2.0), (float)as_num(sc, args[1], 1.0)));
    if (str_eq(name, "round_rect") && ac >= 2)
        return val_mob(anim_rounded_rect(s, (float)as_num(sc, args[0], 2.0), (float)as_num(sc, args[1], 1.0),
                                         ac >= 3 ? (float)as_num(sc, args[2], 0.2) : 0.2f));
    if (str_eq(name, "square"))
        return val_mob(anim_square(s, ac >= 1 ? (float)as_num(sc, args[0], 1.5) : 1.5f));
    if (str_eq(name, "dot"))
    {
        Val pv = {0}, rv = {0}, v3d = {0};
        const Expr *praw = NULL;
        /* Vector dot product: dot(v1, v2) when both are vectors */
        if (ac == 2u && !arg_names[0] && !arg_names[1] && args[0].kind == VAL_VEC && args[1].kind == VAL_VEC)
        {
            AnimVec3 a = args[0].u.vec, b = args[1].u.vec;
            return val_num((double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z);
        }
        float rad = 0.08f;
        AnimVec3 p = ANIM_ORIGIN;
        bool is3d = arg_get(arg_names, args, raw_args, ac, 99u, "3d", &v3d, NULL) && as_bool(sc, v3d);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "r", &rv, NULL))
            rad = (float)as_num(sc, rv, 0.08);
        else if (ac >= 1u && args[0].kind == VAL_NUM)
            rad = (float)as_num(sc, args[0], 0.08);
        else if (ac >= 2u && args[1].kind == VAL_NUM)
            rad = (float)as_num(sc, args[1], 0.08);

        if (arg_get(arg_names, args, raw_args, ac, 99u, "pos", &pv, &praw))
            p = as_vec(sc, pv, ANIM_ORIGIN);
        else if (ac >= 1u && args[0].kind == VAL_VEC)
            { p = as_vec(sc, args[0], ANIM_ORIGIN); praw = raw_args[0]; }
        else if (ac >= 2u && args[1].kind == VAL_VEC)
            { p = as_vec(sc, args[1], ANIM_ORIGIN); praw = raw_args[1]; }

        if (is3d)
            return val_mob(anim_dot3d(s, p, rad));
        {
            AnimId id = anim_dot(s, p, rad);
            if (praw && expr_is_reactive(sc, praw))
                attach_react(sc, id, RB_MOVE_TO, praw, NULL, NULL, ANIM_ID_NIL, ANIM_ORIGIN, 0.0f, 0.0f);
            return val_mob(id);
        }
    }
    if (str_eq(name, "line") && ac >= 2)
    {
        Val va, vb, vd, vg, vr;
        const Expr *ra = NULL, *rb = NULL;
        arg_get(arg_names, args, raw_args, ac, 0u, "a", &va, &ra);
        arg_get(arg_names, args, raw_args, ac, 1u, "b", &vb, &rb);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "r", &vr, NULL))
            return val_mob(anim_line3d(s, as_vec(sc, va, ANIM_LEFT), as_vec(sc, vb, ANIM_RIGHT), (float)as_num(sc, vr, 0.03), 14u));
        if (arg_get(arg_names, args, raw_args, ac, 99u, "dash", &vd, NULL))
        {
            float gap = arg_get(arg_names, args, raw_args, ac, 99u, "gap", &vg, NULL) ? (float)as_num(sc, vg, 0.10) : 0.10f;
            return val_mob(anim_dashed_line(s, as_vec(sc, va, ANIM_LEFT), as_vec(sc, vb, ANIM_RIGHT), (float)as_num(sc, vd, 0.15), gap));
        }
        {
            AnimId id = anim_line(s, as_vec(sc, va, ANIM_LEFT), as_vec(sc, vb, ANIM_RIGHT));
            if ((ra && expr_is_reactive(sc, ra)) || (rb && expr_is_reactive(sc, rb)))
                attach_react(sc, id, RB_LINE, ra, rb, NULL, ANIM_ID_NIL, ANIM_ORIGIN, 0.0f, 0.0f);
            return val_mob(id);
        }
    }
    if (str_eq(name, "arrow") && ac >= 2)
    {
        Val va, vb, vtl, vtw, vboth, vr;
        const Expr *ra = NULL, *rb = NULL;
        float tl = 0.0f, tw = 0.0f;
        arg_get(arg_names, args, raw_args, ac, 0u, "a", &va, &ra);
        arg_get(arg_names, args, raw_args, ac, 1u, "b", &vb, &rb);
        if (arg_get(arg_names, args, raw_args, ac, 2u, "tip_len", &vtl, NULL))   tl = (float)as_num(sc, vtl, 0.0);
        if (arg_get(arg_names, args, raw_args, ac, 3u, "tip_width", &vtw, NULL)) tw = (float)as_num(sc, vtw, 0.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "r", &vr, NULL))
            return val_mob(anim_arrow3d(s, as_vec(sc, va, ANIM_ORIGIN), as_vec(sc, vb, ANIM_UP),
                                        (float)as_num(sc, vr, 0.04), tl > 0.0f ? tl : 0.12f, tw > 0.0f ? tw : 0.30f, 14u));
        if (arg_get(arg_names, args, raw_args, ac, 99u, "both", &vboth, NULL) && as_bool(sc, vboth))
            return val_mob(anim_double_arrow(s, as_vec(sc, va, ANIM_LEFT), as_vec(sc, vb, ANIM_RIGHT), tl, tw));
        {
            AnimId id = anim_arrow(s, as_vec(sc, va, ANIM_ORIGIN), as_vec(sc, vb, ANIM_RIGHT), tl, tw);
            if ((ra && expr_is_reactive(sc, ra)) || (rb && expr_is_reactive(sc, rb)))
                attach_react(sc, id, RB_ARROW, ra, rb, NULL, ANIM_ID_NIL, ANIM_ORIGIN, tl, tw);
            return val_mob(id);
        }
    }
    if (str_eq(name, "vector") && ac >= 1)
        return val_mob(anim_vector(s, as_vec(sc, args[0], ANIM_RIGHT)));
    if (str_eq(name, "polygon") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        AnimVec3 pts[128];
        uint32_t n = MU_MIN(args[0].u.list.count, 128u);
        for (uint32_t i = 0u; i < n; ++i)
            pts[i] = as_vec(sc, args[0].u.list.items[i], ANIM_ORIGIN);
        return val_mob(anim_polygon(s, pts, n));
    }
    if (str_eq(name, "reg_poly") && ac >= 1)
        return val_mob(anim_regular_polygon(s, (uint32_t)as_num(sc, args[0], 6.0),
                                            ac >= 2 ? (float)as_num(sc, args[1], 1.0) : 1.0f));
    if (str_eq(name, "star") && ac >= 1)
        return val_mob(anim_star(s, (uint32_t)as_num(sc, args[0], 5.0),
                                 ac >= 2 ? (float)as_num(sc, args[1], 1.2) : 1.2f,
                                 ac >= 3 ? (float)as_num(sc, args[2], 0.5) : 0.5f));
    if (str_eq(name, "brace") && ac >= 1)
        return val_mob(anim_brace(s, as_mob(args[0]), ac >= 2 ? as_vec(sc, args[1], ANIM_DOWN) : ANIM_DOWN,
                                  ac >= 3 ? (float)as_num(sc, args[2], 0.2) : 0.2f));
    if ((str_eq(name, "bezier") || str_eq(name, "cubic_bezier")) && ac >= 4)
        return val_mob(anim_cubic_bezier(s, as_vec(sc, args[0], ANIM_ORIGIN), as_vec(sc, args[1], ANIM_ORIGIN),
                                         as_vec(sc, args[2], ANIM_ORIGIN), as_vec(sc, args[3], ANIM_ORIGIN)));
    if (str_eq(name, "arc_between") && ac >= 2)
    {
        Val va, vb, vang;
        arg_get(arg_names, args, raw_args, ac, 0u, "a", &va, NULL);
        arg_get(arg_names, args, raw_args, ac, 1u, "b", &vb, NULL);
        float ang = arg_get(arg_names, args, raw_args, ac, 2u, "angle", &vang, NULL) ? (float)as_num(sc, vang, ANIM_PI * 0.5) : ANIM_PI * 0.5f;
        return val_mob(anim_arc_between_points(s, as_vec(sc, va, ANIM_LEFT), as_vec(sc, vb, ANIM_RIGHT), ang));
    }
    if (str_eq(name, "vmob"))
        return val_mob(anim_vmob_create(s));
    if (str_eq(name, "begin_path") && ac >= 2)
        { anim_vmob_begin_subpath(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN)); return args[0]; }
    if (str_eq(name, "line_to") && ac >= 2)
        { anim_vmob_line_to(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN)); return args[0]; }
    if (str_eq(name, "quad_to") && ac >= 3)
        { anim_vmob_quad_to(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN), as_vec(sc, args[2], ANIM_ORIGIN)); return args[0]; }
    if (str_eq(name, "cubic_to") && ac >= 4)
        { anim_vmob_cubic_to(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN), as_vec(sc, args[2], ANIM_ORIGIN), as_vec(sc, args[3], ANIM_ORIGIN)); return args[0]; }
    if (str_eq(name, "close_path") && ac >= 1)
        { anim_vmob_close_subpath(s, as_mob(args[0])); return args[0]; }
    if (str_eq(name, "subdivide") && ac >= 2)
        { anim_vmob_subdivide(s, as_mob(args[0]), (uint32_t)MU_MAX(as_num(sc, args[1], 1.0), 1.0)); return args[0]; }
    if (str_eq(name, "point_at") && ac >= 2)
        return val_vec(anim_vmob_point_at(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0)));
    if (str_eq(name, "add_child") && ac >= 2)
        { anim_add_child(s, as_mob(args[0]), as_mob(args[1])); return args[0]; }
    if (str_eq(name, "remove") && ac >= 1)
        { for (uint32_t i = 0u; i < ac; ++i) anim_remove(s, as_mob(args[i])); return val_nil(); }
    if (str_eq(name, "find") && ac >= 1)
        return val_mob(anim_find_by_tag(s, ac >= 2 ? as_mob(args[1]) : ANIM_ID_NIL, as_str(args[0]) ? as_str(args[0]) : ""));
    if (str_eq(name, "bbox") && ac >= 1)
    {
        AnimVec3 mn, mx;
        Val v = { .kind = VAL_LIST };
        anim_bbox_world(s, as_mob(args[0]), &mn, &mx);
        v.u.list.items = (Val *)sc_alloc(sc, 2u * sizeof(Val));
        v.u.list.count = 2u;
        v.u.list.items[0] = val_vec(mn);
        v.u.list.items[1] = val_vec(mx);
        return v;
    }
    if (str_eq(name, "angle") && ac >= 3)
        return val_mob(anim_angle(s, as_vec(sc, args[0], ANIM_RIGHT), as_vec(sc, args[1], ANIM_ORIGIN),
                                  as_vec(sc, args[2], ANIM_UP), ac >= 4 ? (float)as_num(sc, args[3], 0.5) : 0.5f));
    if (str_eq(name, "right_angle") && ac >= 3)
    {
        float len = ac >= 4 ? (float)as_num(sc, args[3], 0.25) : 0.25f;
        AnimId id = anim_right_angle(s, as_vec(sc, args[0], ANIM_RIGHT), as_vec(sc, args[1], ANIM_ORIGIN),
                                     as_vec(sc, args[2], ANIM_UP), len);
        if (expr_is_reactive(sc, raw_args[0]) || expr_is_reactive(sc, raw_args[1]) || expr_is_reactive(sc, raw_args[2]))
            attach_react(sc, id, RB_RIGHT_ANGLE, raw_args[0], raw_args[1], raw_args[2], ANIM_ID_NIL, ANIM_ORIGIN, len, 0.0f);
        return val_mob(id);
    }
    if (str_eq(name, "surrounding_rect") && ac >= 1)
        return val_mob(anim_surrounding_rect(s, as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 0.18) : 0.18f,
                                             ac >= 3 ? (float)as_num(sc, args[2], 0.08) : 0.08f));
    if (str_eq(name, "bg_rect") && ac >= 1)
        return val_mob(anim_background_rect(s, as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 0.15) : 0.15f,
                                            ac >= 3 ? as_col(args[2], anim_rgba(0,0,0,0.85f)) : anim_rgba(0,0,0,0.85f)));
    if (str_eq(name, "underline") && ac >= 1)
        return val_mob(anim_underline(s, as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 0.12) : 0.12f));
    if (str_eq(name, "cross_mark") && ac >= 1)
        return val_mob(anim_cross(s, as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 4.0) : 4.0f));
    if (str_eq(name, "traced_path") && ac >= 1)
        return val_mob(anim_traced_path(s, as_mob(args[0]), ac >= 2 ? as_col(args[1], ANIM_YELLOW) : ANIM_YELLOW,
                                        ac >= 3 ? (float)as_num(sc, args[2], 3.0) : 3.0f,
                                        ac >= 4 ? (uint32_t)as_num(sc, args[3], 64.0) : 64u));
    /* ---- Graphing, Fields & Plots ---- */
    if (str_eq(name, "number_line") && ac >= 2)
        return val_mob(anim_number_line(s, (float)as_num(sc, args[0], -5.0), (float)as_num(sc, args[1], 5.0),
                                        ac >= 3 ? (float)as_num(sc, args[2], 1.0) : 1.0f,
                                        ac >= 4 ? (float)as_num(sc, args[3], 8.0) : 8.0f,
                                        ac >= 5 ? as_bool(sc, args[4]) : true));
    if (str_eq(name, "axes"))
    {
        AnimAxesDesc desc = { -5.0f, 5.0f, 1.0f, -3.0f, 3.0f, 1.0f, 7.5f, 4.5f, true, true, false, false, NULL };
        Val fx, fy, fz, fsz, fticks, ftips, fgrid, fnum;
        bool has_z = false;
        if (arg_get(arg_names, args, raw_args, ac, 0u, "x", &fx, NULL) && fx.kind == VAL_RANGE)
            { desc.x_min = (float)fx.u.range.lo; desc.x_max = (float)fx.u.range.hi; desc.x_step = fx.u.range.has_step ? (float)fx.u.range.step : 1.0f; }
        if (arg_get(arg_names, args, raw_args, ac, 1u, "y", &fy, NULL) && fy.kind == VAL_RANGE)
            { desc.y_min = (float)fy.u.range.lo; desc.y_max = (float)fy.u.range.hi; desc.y_step = fy.u.range.has_step ? (float)fy.u.range.step : 1.0f; }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "z", &fz, NULL)) has_z = true;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "size", &fsz, NULL))    { AnimVec3 sz = as_vec(sc, fsz, ANIM_V3(7.5f, 4.5f, 0.0f)); desc.x_length = sz.x; desc.y_length = sz.y; }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "ticks", &fticks, NULL)) desc.include_ticks = as_bool(sc, fticks);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "tips", &ftips, NULL))   desc.include_tips = as_bool(sc, ftips);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "grid", &fgrid, NULL))   desc.include_grid = as_bool(sc, fgrid);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "numbers", &fnum, NULL)) desc.include_numbers = as_bool(sc, fnum);
        if (has_z)
        {
            float xr = MU_MAX(fabsf(desc.x_max), fabsf(desc.x_min));
            float yr = MU_MAX(fabsf(desc.y_max), fabsf(desc.y_min));
            float zr = (fz.kind == VAL_RANGE) ? (float)MU_MAX(fabs(fz.u.range.hi), fabs(fz.u.range.lo))
                                              : (float)as_num(sc, fz, 3.0);
            return val_mob(anim_axes3d(s, xr, yr, zr));
        }
        return val_mob(anim_axes(s, &desc));
    }
    if (str_eq(name, "c2p") && ac >= 3)
        return val_vec(anim_axes_c2p(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0), (float)as_num(sc, args[2], 0.0)));
    if (str_eq(name, "p2c") && ac >= 2)
    {
        float cx = 0.0f, cy = 0.0f;
        anim_axes_p2c(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN), &cx, &cy);
        return val_vec(ANIM_V3(cx, cy, 0.0f));
    }
    if (str_eq(name, "plot") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float x0 = -4.0f, x1 = 4.0f; uint32_t samp = 64u;
        Val vr, v1, vs;
        const Expr *rx0 = NULL, *rx1 = NULL;
        AnimId ax_id = as_mob(args[0]), pid;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vr, &rx0))
        {
            if (vr.kind == VAL_RANGE) { x0 = (float)vr.u.range.lo; x1 = (float)vr.u.range.hi; }
            else
            {
                x0 = (float)as_num(sc, vr, -4.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &v1, &rx1)) x1 = (float)as_num(sc, v1, 4.0);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 64.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 64.0);
        pid = anim_plot(s, ax_id, cb_scalar_fn, x0, x1, samp, ctx);
        attach_react_plot(sc, pid, RB_PLOT, ax_id, ctx, rx0, rx1, NULL, NULL, x0, x1, 0.0f, 0.0f, samp, 0u, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(pid);
    }
    if (str_eq(name, "area") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float x0 = -3.0f, x1 = 3.0f; uint32_t samp = 48u;
        Val vr, v1, vs;
        const Expr *rx0 = NULL, *rx1 = NULL;
        AnimId ax_id = as_mob(args[0]), pid;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vr, &rx0))
        {
            if (vr.kind == VAL_RANGE) { x0 = (float)vr.u.range.lo; x1 = (float)vr.u.range.hi; }
            else
            {
                x0 = (float)as_num(sc, vr, -3.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &v1, &rx1)) x1 = (float)as_num(sc, v1, 3.0);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 48.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 48.0);
        pid = anim_area_under_curve(s, ax_id, cb_scalar_fn, x0, x1, samp, ctx);
        attach_react_plot(sc, pid, RB_AREA, ax_id, ctx, rx0, rx1, NULL, NULL, x0, x1, 0.0f, 0.0f, samp, 0u, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(pid);
    }
    if (str_eq(name, "riemann") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float x0 = -3.0f, x1 = 3.0f, dx = 0.4f;
        AnimColor c0 = ANIM_BLUE_D, c1 = ANIM_TEAL;
        Val vr, v1, vdx, vc0, vc1;
        const Expr *rx0 = NULL, *rx1 = NULL, *rdx = NULL;
        AnimId ax_id = as_mob(args[0]), pid;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vr, &rx0))
        {
            if (vr.kind == VAL_RANGE)
            {
                x0 = (float)vr.u.range.lo; x1 = (float)vr.u.range.hi;
                if (vr.u.range.has_step) dx = (float)vr.u.range.step;
            }
            else
            {
                x0 = (float)as_num(sc, vr, -3.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &v1, &rx1)) x1 = (float)as_num(sc, v1, 3.0);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "dx", &vdx, &rdx)) dx = (float)as_num(sc, vdx, 0.4);
                if (arg_get(arg_names, args, raw_args, ac, 5u, "c0", &vc0, NULL)) c0 = as_col(vc0, ANIM_BLUE_D);
                if (arg_get(arg_names, args, raw_args, ac, 6u, "c1", &vc1, NULL)) c1 = as_col(vc1, ANIM_TEAL);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "dx", &vdx, &rdx)) dx = (float)as_num(sc, vdx, dx);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "c_left", &vc0, NULL)) c0 = as_col(vc0, c0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "c_right", &vc1, NULL)) c1 = as_col(vc1, c1);
        pid = anim_riemann_rectangles(s, ax_id, cb_scalar_fn, x0, x1, dx, ctx, c0, c1);
        attach_react_plot(sc, pid, RB_RIEMANN, ax_id, ctx, rx0, rx1, rdx, NULL, x0, x1, dx, 0.0f, 0u, 0u, c0, c1, NULL);
        return val_mob(pid);
    }
    if (str_eq(name, "implicit_plot") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float x0 = -3.0f, x1 = 3.0f, y0 = -3.0f, y1 = 3.0f;
        uint32_t nx = 48u, ny = 48u;
        Val vx, vx1, vy, vy1, vnx, vny;
        const Expr *rx0 = NULL, *rx1 = NULL, *ry0 = NULL, *ry1 = NULL;
        AnimId ax_id = as_mob(args[0]), pid;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vx, &rx0))
        {
            if (vx.kind == VAL_RANGE)
            {
                x0 = (float)vx.u.range.lo; x1 = (float)vx.u.range.hi;
                if (arg_get(arg_names, args, raw_args, ac, 3u, "y", &vy, &ry0) && vy.kind == VAL_RANGE)
                    { y0 = (float)vy.u.range.lo; y1 = (float)vy.u.range.hi; }
            }
            else
            {
                x0 = (float)as_num(sc, vx, -3.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &vx1, &rx1)) x1 = (float)as_num(sc, vx1, 3.0);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "y",  &vy,  &ry0)) y0 = (float)as_num(sc, vy, -3.0);
                if (arg_get(arg_names, args, raw_args, ac, 5u, "y1", &vy1, &ry1)) y1 = (float)as_num(sc, vy1, 3.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "nx", &vnx, NULL)) nx = (uint32_t)as_num(sc, vnx, 48.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "ny", &vny, NULL)) ny = (uint32_t)as_num(sc, vny, 48.0);
        pid = anim_implicit_plot(s, ax_id, cb_implicit_fn, x0, x1, y0, y1, nx, ny, ctx);
        attach_react_plot(sc, pid, RB_IMPLICIT, ax_id, ctx, rx0, rx1, ry0, ry1, x0, x1, y0, y1, nx, ny, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(pid);
    }
    if (str_eq(name, "contour_plot") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float x0 = -3.0f, x1 = 3.0f, y0 = -3.0f, y1 = 3.0f;
        uint32_t nx = 48u, ny = 48u, nlev = 8u;
        AnimColor c_low = ANIM_BLUE, c_high = ANIM_YELLOW;
        float *levels = NULL;
        Val vx, vx1, vy, vy1, vnx, vny, vl, vn, vc0, vc1;
        const Expr *rx0 = NULL, *rx1 = NULL, *ry0 = NULL, *ry1 = NULL;
        AnimId ax_id = as_mob(args[0]), pid;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vx, &rx0))
        {
            if (vx.kind == VAL_RANGE)
            {
                x0 = (float)vx.u.range.lo; x1 = (float)vx.u.range.hi;
                if (arg_get(arg_names, args, raw_args, ac, 3u, "y", &vy, &ry0) && vy.kind == VAL_RANGE)
                    { y0 = (float)vy.u.range.lo; y1 = (float)vy.u.range.hi; }
            }
            else
            {
                x0 = (float)as_num(sc, vx, -3.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &vx1, &rx1)) x1 = (float)as_num(sc, vx1, 3.0);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "y",  &vy,  &ry0)) y0 = (float)as_num(sc, vy, -3.0);
                if (arg_get(arg_names, args, raw_args, ac, 5u, "y1", &vy1, &ry1)) y1 = (float)as_num(sc, vy1, 3.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "nx", &vnx, NULL)) nx = (uint32_t)as_num(sc, vnx, 48.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "ny", &vny, NULL)) ny = (uint32_t)as_num(sc, vny, 48.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "n",  &vn,  NULL)) nlev = (uint32_t)MU_MAX(as_num(sc, vn, 8.0), 1.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "levels", &vl, NULL) && vl.kind == VAL_LIST && vl.u.list.count > 0u)
        {
            nlev = vl.u.list.count;
            levels = (float *)sc_alloc(sc, nlev * sizeof *levels);
            for (uint32_t i = 0u; i < nlev; ++i) levels[i] = (float)as_num(sc, vl.u.list.items[i], 0.0);
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "c_low", &vc0, NULL))  c_low = as_col(vc0, ANIM_BLUE);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "c_high", &vc1, NULL)) c_high = as_col(vc1, ANIM_YELLOW);
        pid = anim_contour_plot(s, ax_id, cb_implicit_fn, x0, x1, y0, y1, nx, ny, levels, nlev, c_low, c_high, ctx);
        attach_react_plot(sc, pid, RB_CONTOUR, ax_id, ctx, rx0, rx1, ry0, ry1, x0, x1, y0, y1, nx, nlev, c_low, c_high, levels);
        return val_mob(pid);
    }
    if (str_eq(name, "vector_field") && ac >= 1)
    {
        if (args[0].kind == VAL_FN)
        {
            ScriptCbCtx *ctx = make_cb_ctx(sc, args[0].u.fn);
            Val vb0, vb1, vn, vml, vc0, vc1;
            AnimVec3 bmin = arg_get(arg_names, args, raw_args, ac, 1u, "min", &vb0, NULL) ? as_vec(sc, vb0, ANIM_V3(-1.5f,-1.5f,-1.5f)) : ANIM_V3(-1.5f,-1.5f,-1.5f);
            AnimVec3 bmax = arg_get(arg_names, args, raw_args, ac, 2u, "max", &vb1, NULL) ? as_vec(sc, vb1, ANIM_V3( 1.5f, 1.5f, 1.5f)) : ANIM_V3( 1.5f, 1.5f, 1.5f);
            uint32_t n    = arg_get(arg_names, args, raw_args, ac, 3u, "n",   &vn,  NULL) ? (uint32_t)as_num(sc, vn, 4.0) : 4u;
            float    ml   = arg_get(arg_names, args, raw_args, ac, 4u, "max_len", &vml, NULL) ? (float)as_num(sc, vml, 0.65) : 0.65f;
            AnimColor c0  = arg_get(arg_names, args, raw_args, ac, 5u, "c_lo", &vc0, NULL) ? as_col(vc0, ANIM_BLUE_D) : ANIM_BLUE_D;
            AnimColor c1  = arg_get(arg_names, args, raw_args, ac, 6u, "c_hi", &vc1, NULL) ? as_col(vc1, ANIM_YELLOW) : ANIM_YELLOW;
            return val_mob(anim_vector_field3d(s, cb_field_fn, bmin, bmax, n, n, n, ml, c0, c1, ctx));
        }
        if (ac >= 2 && args[1].kind == VAL_FN)
        {
            ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
            Val vdx, vdy, vml;
            float dx = arg_get(arg_names, args, raw_args, ac, 2u, "dx", &vdx, NULL) ? (float)as_num(sc, vdx, 0.7) : 0.7f;
            float dy = arg_get(arg_names, args, raw_args, ac, 3u, "dy", &vdy, NULL) ? (float)as_num(sc, vdy, 0.7) : 0.7f;
            float ml = arg_get(arg_names, args, raw_args, ac, 4u, "max_len", &vml, NULL) ? (float)as_num(sc, vml, 0.5) : 0.5f;
            return val_mob(anim_vector_field(s, as_mob(args[0]), cb_field_fn, dx, dy, ml, ctx));
        }
    }
    if (str_eq(name, "stream_lines") && ac >= 1)
    {
        if (args[0].kind == VAL_FN)
        {
            ScriptCbCtx *ctx = make_cb_ctx(sc, args[0].u.fn);
            Val vb0, vb1, vn, vst, vms, vc0, vc1;
            AnimVec3 bmin = arg_get(arg_names, args, raw_args, ac, 1u, "min", &vb0, NULL) ? as_vec(sc, vb0, ANIM_V3(-1.5f,-1.5f,-1.5f)) : ANIM_V3(-1.5f,-1.5f,-1.5f);
            AnimVec3 bmax = arg_get(arg_names, args, raw_args, ac, 2u, "max", &vb1, NULL) ? as_vec(sc, vb1, ANIM_V3( 1.5f, 1.5f, 1.5f)) : ANIM_V3( 1.5f, 1.5f, 1.5f);
            uint32_t n    = arg_get(arg_names, args, raw_args, ac, 3u, "n",   &vn,  NULL) ? (uint32_t)as_num(sc, vn, 3.0) : 3u;
            float    step = arg_get(arg_names, args, raw_args, ac, 4u, "step",&vst, NULL) ? (float)as_num(sc, vst, 0.10) : 0.10f;
            uint32_t mstp = arg_get(arg_names, args, raw_args, ac, 5u, "max_steps", &vms, NULL) ? (uint32_t)as_num(sc, vms, 36.0) : 36u;
            AnimColor c0  = arg_get(arg_names, args, raw_args, ac, 6u, "c_lo", &vc0, NULL) ? as_col(vc0, ANIM_TEAL) : ANIM_TEAL;
            AnimColor c1  = arg_get(arg_names, args, raw_args, ac, 7u, "c_hi", &vc1, NULL) ? as_col(vc1, ANIM_GOLD) : ANIM_GOLD;
            return val_mob(anim_stream_lines3d(s, cb_field_fn, bmin, bmax, n, n, n, step, mstp, c0, c1, ctx));
        }
        if (ac >= 2 && args[1].kind == VAL_FN)
        {
            ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
            float x0 = -3.0f, x1 = 3.0f, y0 = -2.5f, y1 = 2.5f, dx = 0.7f, dy = 0.7f;
            uint32_t steps = 32u;
            Val vx0, vx1, vy0, vy1, vdx, vdy, vms;
            if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vx0, NULL))
            {
                if (vx0.kind == VAL_RANGE) { x0 = (float)vx0.u.range.lo; x1 = (float)vx0.u.range.hi; }
                else { x0 = (float)as_num(sc, vx0, -3.0); if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &vx1, NULL)) x1 = (float)as_num(sc, vx1, 3.0); }
            }
            if (arg_get(arg_names, args, raw_args, ac, 4u, "y", &vy0, NULL))
            {
                if (vy0.kind == VAL_RANGE) { y0 = (float)vy0.u.range.lo; y1 = (float)vy0.u.range.hi; }
                else { y0 = (float)as_num(sc, vy0, -2.5); if (arg_get(arg_names, args, raw_args, ac, 5u, "y1", &vy1, NULL)) y1 = (float)as_num(sc, vy1, 2.5); }
            }
            if (arg_get(arg_names, args, raw_args, ac, 99u, "dx", &vdx, NULL)) dx = (float)as_num(sc, vdx, 0.7);
            if (arg_get(arg_names, args, raw_args, ac, 99u, "dy", &vdy, NULL)) dy = (float)as_num(sc, vdy, 0.7);
            if (arg_get(arg_names, args, raw_args, ac, 99u, "max_len", &vms, NULL)) steps = (uint32_t)MU_MAX(as_num(sc, vms, 0.4) * 80.0, 4.0);
            if (arg_get(arg_names, args, raw_args, ac, 99u, "max_steps", &vms, NULL)) steps = (uint32_t)MU_MAX(as_num(sc, vms, 32.0), 4.0);
            return val_mob(anim_stream_lines(s, as_mob(args[0]), cb_field_fn, x0, x1, y0, y1, dx, dy, steps, ctx));
        }
    }
    if (str_eq(name, "animated_stream_lines") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float x0 = -3.0f, x1 = 3.0f, y0 = -2.5f, y1 = 2.5f, dx = 0.7f, dy = 0.7f, spd = 1.0f;
        uint32_t steps = 32u;
        Val vx0, vx1, vy0, vy1, vdx, vdy, vms, vspd;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "x", &vx0, NULL))
        {
            if (vx0.kind == VAL_RANGE) { x0 = (float)vx0.u.range.lo; x1 = (float)vx0.u.range.hi; }
            else { x0 = (float)as_num(sc, vx0, -3.0); if (arg_get(arg_names, args, raw_args, ac, 3u, "x1", &vx1, NULL)) x1 = (float)as_num(sc, vx1, 3.0); }
        }
        if (arg_get(arg_names, args, raw_args, ac, 4u, "y", &vy0, NULL))
        {
            if (vy0.kind == VAL_RANGE) { y0 = (float)vy0.u.range.lo; y1 = (float)vy0.u.range.hi; }
            else { y0 = (float)as_num(sc, vy0, -2.5); if (arg_get(arg_names, args, raw_args, ac, 5u, "y1", &vy1, NULL)) y1 = (float)as_num(sc, vy1, 2.5); }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "dx", &vdx, NULL)) dx = (float)as_num(sc, vdx, 0.7);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "dy", &vdy, NULL)) dy = (float)as_num(sc, vdy, 0.7);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "max_steps", &vms, NULL)) steps = (uint32_t)MU_MAX(as_num(sc, vms, 32.0), 4.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "flow_speed", &vspd, NULL)) spd = (float)as_num(sc, vspd, 1.0);
        return val_mob(anim_animated_stream_lines(s, as_mob(args[0]), cb_field_fn, x0, x1, y0, y1, dx, dy, steps, spd, ctx));
    }
    if (str_eq(name, "polar_plane"))
        return val_mob(anim_polar_plane(s, ac >= 1 ? (float)as_num(sc, args[0], 3.0) : 3.0f,
                                        ac >= 2 ? (float)as_num(sc, args[1], 1.0) : 1.0f,
                                        ac >= 3 ? (uint32_t)as_num(sc, args[2], 12.0) : 12u,
                                        ac >= 4 ? (float)as_num(sc, args[3], 5.0) : 5.0f));
    if (str_eq(name, "plot_polar") && ac >= 2 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        float t0 = 0.0f, t1 = ANIM_TAU; uint32_t samp = 72u;
        Val vt, vt1, vs;
        const Expr *rt0 = NULL, *rt1 = NULL;
        AnimId pl_id = as_mob(args[0]), pid;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "t", &vt, &rt0))
        {
            if (vt.kind == VAL_RANGE) { t0 = (float)vt.u.range.lo; t1 = (float)vt.u.range.hi; }
            else
            {
                t0 = (float)as_num(sc, vt, 0.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "t1", &vt1, &rt1)) t1 = (float)as_num(sc, vt1, ANIM_TAU);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 72.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 72.0);
        pid = anim_plot_polar(s, pl_id, cb_scalar_fn, t0, t1, samp, ctx);
        attach_react_plot(sc, pid, RB_POLAR, pl_id, ctx, rt0, rt1, NULL, NULL, t0, t1, 0.0f, 0.0f, samp, 0u, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(pid);
    }
    if (str_eq(name, "parametric_curve") && ac >= 1 && args[0].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[0].u.fn);
        float t0 = 0.0f, t1 = ANIM_TAU; uint32_t samp = 72u;
        Val vt, vt1, vs;
        const Expr *rt0 = NULL, *rt1 = NULL;
        AnimId pid;
        if (arg_get(arg_names, args, raw_args, ac, 1u, "t", &vt, &rt0))
        {
            if (vt.kind == VAL_RANGE) { t0 = (float)vt.u.range.lo; t1 = (float)vt.u.range.hi; }
            else
            {
                t0 = (float)as_num(sc, vt, 0.0);
                if (arg_get(arg_names, args, raw_args, ac, 2u, "t1", &vt1, &rt1)) t1 = (float)as_num(sc, vt1, ANIM_TAU);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 72.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "samples", &vs, NULL)) samp = (uint32_t)as_num(sc, vs, 72.0);
        pid = anim_parametric_curve(s, cb_param_fn, t0, t1, samp, ctx);
        attach_react_plot(sc, pid, RB_PARAM, ANIM_ID_NIL, ctx, rt0, rt1, NULL, NULL, t0, t1, 0.0f, 0.0f, samp, 0u, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(pid);
    }
    if (str_eq(name, "scatter_plot") && ac >= 3 && args[1].kind == VAL_LIST && args[2].kind == VAL_LIST)
    {
        uint32_t n = MU_MIN(args[1].u.list.count, args[2].u.list.count);
        float *xs = (float *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *xs);
        float *ys = (float *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *ys);
        for (uint32_t i = 0u; i < n; ++i)
        {
            xs[i] = (float)as_num(sc, args[1].u.list.items[i], 0.0);
            ys[i] = (float)as_num(sc, args[2].u.list.items[i], 0.0);
        }
        return val_mob(anim_scatter_plot(s, as_mob(args[0]), xs, ys, NULL, NULL, n,
                                         ac >= 4 ? (float)as_num(sc, args[3], 0.08) : 0.08f));
    }
    if (str_eq(name, "bar_plot") && ac >= 3 && args[1].kind == VAL_LIST)
    {
        uint32_t tot = args[1].u.list.count;
        uint32_t ser = (uint32_t)MU_MAX(as_num(sc, args[2], 1.0), 1.0);
        uint32_t cat = tot / ser;
        float *vals = (float *)sc_alloc(sc, MU_MAX(tot, 1u) * sizeof *vals);
        for (uint32_t i = 0u; i < tot; ++i) vals[i] = (float)as_num(sc, args[1].u.list.items[i], 0.0);
        return val_mob(anim_bar_plot(s, as_mob(args[0]), vals, cat, ser,
                                     ac >= 4 ? as_bool(sc, args[3]) : false, NULL,
                                     ac >= 5 ? (float)as_num(sc, args[4], 0.72) : 0.72f));
    }
    if (str_eq(name, "heatmap") && ac >= 4 && args[1].kind == VAL_LIST)
    {
        uint32_t cols = (uint32_t)MU_MAX(as_num(sc, args[2], 1.0), 1.0);
        uint32_t rows = (uint32_t)MU_MAX(as_num(sc, args[3], 1.0), 1.0);
        uint32_t tot  = cols * rows;
        float   *vals = (float *)sc_alloc(sc, MU_MAX(tot, 1u) * sizeof *vals);
        for (uint32_t i = 0u; i < tot && i < args[1].u.list.count; ++i)
            vals[i] = (float)as_num(sc, args[1].u.list.items[i], 0.0);
        return val_mob(anim_heatmap(s, as_mob(args[0]), vals, cols, rows,
                                    ac >= 5 ? (float)as_num(sc, args[4], 0.0) : 0.0f,
                                    ac >= 6 ? (float)as_num(sc, args[5], 1.0) : 1.0f,
                                    ac >= 7 ? as_col(args[6], ANIM_BLUE_D) : ANIM_BLUE_D,
                                    ac >= 8 ? as_col(args[7], ANIM_TEAL) : ANIM_TEAL,
                                    ac >= 9 ? as_col(args[8], ANIM_YELLOW) : ANIM_YELLOW));
    }
    if (str_eq(name, "heatmap_set") && ac >= 4 && args[1].kind == VAL_LIST)
    {
        uint32_t cols = (uint32_t)MU_MAX(as_num(sc, args[2], 1.0), 1.0);
        uint32_t rows = (uint32_t)MU_MAX(as_num(sc, args[3], 1.0), 1.0);
        uint32_t tot  = cols * rows;
        float   *vals = (float *)sc_alloc(sc, MU_MAX(tot, 1u) * sizeof *vals);
        for (uint32_t i = 0u; i < tot && i < args[1].u.list.count; ++i)
            vals[i] = (float)as_num(sc, args[1].u.list.items[i], 0.0);
        anim_heatmap_set(s, as_mob(args[0]), vals, cols, rows,
                         ac >= 5 ? (float)as_num(sc, args[4], 0.0) : 0.0f,
                         ac >= 6 ? (float)as_num(sc, args[5], 1.0) : 1.0f,
                         ac >= 7 ? as_col(args[6], ANIM_BLUE_D) : ANIM_BLUE_D,
                         ac >= 8 ? as_col(args[7], ANIM_TEAL) : ANIM_TEAL,
                         ac >= 9 ? as_col(args[8], ANIM_YELLOW) : ANIM_YELLOW);
        return args[0];
    }
    if (str_eq(name, "frustum3d"))
    {
        Val vfov, vasp, vn, vf, vp, vt;
        float fov  = arg_get(arg_names, args, raw_args, ac, 0u, "fov",    &vfov, NULL) ? (float)as_num(sc, vfov, 60.0) : 60.0f;
        float asp  = arg_get(arg_names, args, raw_args, ac, 1u, "aspect", &vasp, NULL) ? (float)as_num(sc, vasp, 16.0 / 9.0) : (16.0f / 9.0f);
        float nz   = arg_get(arg_names, args, raw_args, ac, 2u, "near",   &vn,   NULL) ? (float)as_num(sc, vn, 0.5) : 0.5f;
        float fz   = arg_get(arg_names, args, raw_args, ac, 3u, "far",    &vf,   NULL) ? (float)as_num(sc, vf, 4.0) : 4.0f;
        AnimVec3 p = arg_get(arg_names, args, raw_args, ac, 4u, "pos",    &vp,   NULL) ? as_vec(sc, vp, ANIM_V3(0, 0, 4)) : ANIM_V3(0, 0, 4);
        AnimVec3 t = arg_get(arg_names, args, raw_args, ac, 5u, "target", &vt,   NULL) ? as_vec(sc, vt, ANIM_ORIGIN) : ANIM_ORIGIN;
        return val_mob(anim_frustum3d(s, fov, asp, nz, fz, p, t));
    }
    if (str_eq(name, "pixel_grid") && ac >= 2)
    {
        Val vc, vr, vcs, vcen;
        uint32_t cols = (uint32_t)MU_MAX(as_num(sc, arg_get(arg_names, args, raw_args, ac, 0u, "cols", &vc, NULL) ? vc : args[0], 8.0), 1.0);
        uint32_t rows = (uint32_t)MU_MAX(as_num(sc, arg_get(arg_names, args, raw_args, ac, 1u, "rows", &vr, NULL) ? vr : args[1], 8.0), 1.0);
        float    csz  = arg_get(arg_names, args, raw_args, ac, 2u, "cell",    &vcs,  NULL) ? (float)as_num(sc, vcs, 0.35) : 0.35f;
        bool     cen  = arg_get(arg_names, args, raw_args, ac, 3u, "centers", &vcen, NULL) ? as_bool(sc, vcen) : false;
        return val_mob(anim_pixel_grid(s, cols, rows, csz, cen, NULL));
    }
    if (str_eq(name, "pixel_array") && ac >= 1)
    {
        if (args[0].kind == VAL_LIST && args[0].u.list.count > 0u)
        {
            uint32_t rows = args[0].u.list.count;
            uint32_t cols = (args[0].u.list.items[0].kind == VAL_LIST) ? args[0].u.list.items[0].u.list.count : rows;
            if (args[0].u.list.items[0].kind == VAL_LIST)
            {
                AnimColor *px = (AnimColor *)sc_alloc(sc, (size_t)rows * (size_t)MU_MAX(cols, 1u) * sizeof *px);
                for (uint32_t r = 0u; r < rows; ++r)
                {
                    Val row = args[0].u.list.items[r];
                    for (uint32_t c = 0u; c < cols; ++c)
                        px[r * cols + c] = (row.kind == VAL_LIST && c < row.u.list.count) ? as_col(row.u.list.items[c], ANIM_BLACK) : ANIM_BLACK;
                }
                float cs = ac >= 2 ? (float)as_num(sc, args[1], 0.35) : 0.35f;
                float gp = ac >= 3 ? (float)as_num(sc, args[2], 0.0)  : 0.0f;
                return val_mob(anim_pixel_array_grid(s, px, cols, rows, cs, gp));
            }
            else
            {
                uint32_t nc = (uint32_t)MU_MAX(ac >= 2 ? as_num(sc, args[1], (double)rows) : (double)rows, 1.0);
                uint32_t nr = (rows + nc - 1u) / nc;
                AnimColor *px = (AnimColor *)sc_alloc(sc, (size_t)nr * (size_t)nc * sizeof *px);
                for (uint32_t i = 0u; i < nr * nc; ++i)
                    px[i] = (i < rows) ? as_col(args[0].u.list.items[i], ANIM_BLACK) : ANIM_BLACK;
                float cs = ac >= 3 ? (float)as_num(sc, args[2], 0.35) : 0.35f;
                return val_mob(anim_pixel_array_grid(s, px, nc, nr, cs, 0.0f));
            }
        }
        else if (ac >= 3 && args[2].kind == VAL_FN)
        {
            uint32_t w = (uint32_t)MU_MAX(as_num(sc, args[0], 32.0), 1.0);
            uint32_t h = (uint32_t)MU_MAX(as_num(sc, args[1], 32.0), 1.0);
            Val vh;
            float wh = arg_get(arg_names, args, raw_args, ac, 3u, "height", &vh, NULL) ? (float)as_num(sc, vh, 3.5) : 3.5f;
            return val_mob(anim_pixel_array_fn(s, w, h, wh, cb_pixel_fn, make_cb_ctx(sc, args[2].u.fn)));
        }
    }
    if (str_eq(name, "callout") && ac >= 3)
    {
        AnimVec3 tp = as_vec(sc, args[0], ANIM_ORIGIN);
        AnimVec3 bp = as_vec(sc, args[1], ANIM_V3(2.0f, 1.0f, 0.0f));
        const char *lbl = as_str(args[2]) ? as_str(args[2]) : "note";
        Val vcol, vfp;
        AnimColor col = arg_get(arg_names, args, raw_args, ac, 3u, "color", &vcol, NULL) ? as_col(vcol, ANIM_YELLOW) : ANIM_YELLOW;
        const char *fp = (arg_get(arg_names, args, raw_args, ac, 4u, "font", &vfp, NULL) && as_str(vfp)) ? as_str(vfp) : NULL;
        return val_mob(anim_callout(s, tp, bp, lbl, fp, col));
    }
    if (str_eq(name, "dimension_line") && ac >= 2)
    {
        AnimVec3 a = as_vec(sc, args[0], ANIM_ORIGIN);
        AnimVec3 b = as_vec(sc, args[1], ANIM_RIGHT);
        Val voff, vlbl, vcol, vfp;
        float off = arg_get(arg_names, args, raw_args, ac, 2u, "offset", &voff, NULL) ? (float)as_num(sc, voff, 0.45) : 0.45f;
        const char *lbl = (arg_get(arg_names, args, raw_args, ac, 3u, "label", &vlbl, NULL) && as_str(vlbl)) ? as_str(vlbl) : NULL;
        AnimColor col = arg_get(arg_names, args, raw_args, ac, 4u, "color", &vcol, NULL) ? as_col(vcol, ANIM_TEAL) : ANIM_TEAL;
        const char *fp = (arg_get(arg_names, args, raw_args, ac, 5u, "font", &vfp, NULL) && as_str(vfp)) ? as_str(vfp) : NULL;
        return val_mob(anim_dimension_line(s, a, b, off, lbl, fp, col));
    }
    /* ---- 3D Solids ---- */
    if (str_eq(name, "sphere"))
        return val_mob(anim_sphere(s, ac >= 1 ? (float)as_num(sc, args[0], 1.0) : 1.0f,
                                   ac >= 2 ? (uint32_t)as_num(sc, args[1], 28.0) : 28u,
                                   ac >= 3 ? (uint32_t)as_num(sc, args[2], 18.0) : 18u));
    if (str_eq(name, "torus"))
        return val_mob(anim_torus(s, ac >= 1 ? (float)as_num(sc, args[0], 1.2) : 1.2f,
                                  ac >= 2 ? (float)as_num(sc, args[1], 0.4) : 0.4f,
                                  ac >= 3 ? (uint32_t)as_num(sc, args[2], 32.0) : 32u,
                                  ac >= 4 ? (uint32_t)as_num(sc, args[3], 18.0) : 18u));
    if (str_eq(name, "cube"))
        return val_mob(anim_cube(s, ac >= 1 ? (float)as_num(sc, args[0], 1.5) : 1.5f));
    if (str_eq(name, "prism"))
        return val_mob(anim_prism(s, ac >= 1 ? as_vec(sc, args[0], ANIM_V3(1.5f, 1.0f, 1.0f)) : ANIM_V3(1.5f, 1.0f, 1.0f)));
    if (str_eq(name, "cylinder"))
        return val_mob(anim_cylinder(s, ac >= 1 ? (float)as_num(sc, args[0], 0.6) : 0.6f,
                                     ac >= 2 ? (float)as_num(sc, args[1], 1.8) : 1.8f,
                                     ac >= 3 ? (uint32_t)as_num(sc, args[2], 24.0) : 24u));
    if (str_eq(name, "cone"))
        return val_mob(anim_cone(s, ac >= 1 ? (float)as_num(sc, args[0], 0.7) : 0.7f,
                                 ac >= 2 ? (float)as_num(sc, args[1], 1.8) : 1.8f,
                                 ac >= 3 ? (uint32_t)as_num(sc, args[2], 24.0) : 24u));
    if (str_eq(name, "polyhedron"))
        return val_mob(anim_polyhedron(s, ac >= 1 ? (AnimPolyhedronKind)(int)as_num(sc, args[0], 4.0) : ANIM_POLY_ICOSAHEDRON,
                                       ac >= 2 ? (float)as_num(sc, args[1], 1.0) : 1.0f));
    if (str_eq(name, "surface3d") && ac >= 1 && args[0].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[0].u.fn);
        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        uint32_t nu = 28u, nv = 28u;
        Val vu, vv, vu1, vv1, vnu, vnv;
        const Expr *ru0 = NULL, *ru1 = NULL, *rv0 = NULL, *rv1 = NULL;
        AnimId pid;
        if (arg_get(arg_names, args, raw_args, ac, 1u, "u", &vu, &ru0))
        {
            if (vu.kind == VAL_RANGE)
            {
                u0 = (float)vu.u.range.lo; u1 = (float)vu.u.range.hi;
                if (arg_get(arg_names, args, raw_args, ac, 2u, "v", &vv, &rv0) && vv.kind == VAL_RANGE)
                    { v0 = (float)vv.u.range.lo; v1 = (float)vv.u.range.hi; }
            }
            else
            {
                u0 = (float)as_num(sc, vu, 0.0);
                if (arg_get(arg_names, args, raw_args, ac, 2u, "u1", &vu1, &ru1)) u1 = (float)as_num(sc, vu1, 1.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "v",  &vv,  &rv0)) v0 = (float)as_num(sc, vv,  0.0);
                if (arg_get(arg_names, args, raw_args, ac, 4u, "v1", &vv1, &rv1)) v1 = (float)as_num(sc, vv1, 1.0);
                if (arg_get(arg_names, args, raw_args, ac, 5u, "nu", &vnu, NULL)) nu = (uint32_t)as_num(sc, vnu, 28.0);
                if (arg_get(arg_names, args, raw_args, ac, 6u, "nv", &vnv, NULL)) nv = (uint32_t)as_num(sc, vnv, 28.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "nu", &vnu, NULL)) nu = (uint32_t)as_num(sc, vnu, 28.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "nv", &vnv, NULL)) nv = (uint32_t)as_num(sc, vnv, 28.0);
        pid = anim_surface3d(s, cb_surface_fn, NULL, u0, u1, v0, v1, nu, nv, ctx);
        attach_react_plot(sc, pid, RB_SURFACE, ANIM_ID_NIL, ctx, ru0, ru1, rv0, rv1, u0, u1, v0, v1, nu, nv, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(pid);
    }

    /* ---- Data Viz, Geometry & Diagrams ---- */
    if (str_eq(name, "box_plot") && ac >= 2 && args[1].kind == VAL_LIST)
    {
        uint32_t n = args[1].u.list.count;
        AnimBoxStats *bs = (AnimBoxStats *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *bs);
        AnimColor *cols = NULL;
        Val vw, vc;
        float bw = arg_get(arg_names, args, raw_args, ac, 2u, "width", &vw, NULL) ? (float)as_num(sc, vw, 0.5) : 0.5f;
        for (uint32_t i = 0u; i < n; ++i)
        {
            Val item = args[1].u.list.items[i];
            if (item.kind == VAL_LIST && item.u.list.count >= 5u)
            {
                bs[i].min_v  = (float)as_num(sc, item.u.list.items[0], 0.0);
                bs[i].q1     = (float)as_num(sc, item.u.list.items[1], 0.0);
                bs[i].median = (float)as_num(sc, item.u.list.items[2], 0.0);
                bs[i].q3     = (float)as_num(sc, item.u.list.items[3], 0.0);
                bs[i].max_v  = (float)as_num(sc, item.u.list.items[4], 0.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 3u, "colors", &vc, NULL) && vc.kind == VAL_LIST && vc.u.list.count >= n)
        {
            cols = (AnimColor *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *cols);
            for (uint32_t i = 0u; i < n; ++i) cols[i] = as_col(vc.u.list.items[i], ANIM_BLUE);
        }
        return val_mob(anim_box_plot(s, as_mob(args[0]), bs, n, bw, cols));
    }
    if (str_eq(name, "convex_hull") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        AnimVec3 *pts = (AnimVec3 *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *pts);
        for (uint32_t i = 0u; i < n; ++i) pts[i] = as_vec(sc, args[0].u.list.items[i], ANIM_ORIGIN);
        return val_mob(anim_convex_hull(s, pts, n));
    }
    if (str_eq(name, "number_plane"))
    {
        float x0 = ac >= 1 ? (float)as_num(sc, args[0], -5.0) : -5.0f;
        float x1 = ac >= 2 ? (float)as_num(sc, args[1],  5.0) :  5.0f;
        float y0 = ac >= 3 ? (float)as_num(sc, args[2], -3.0) : -3.0f;
        float y1 = ac >= 4 ? (float)as_num(sc, args[3],  3.0) :  3.0f;
        return val_mob(anim_number_plane(s, x0, x1, 1.0f, y0, y1, 1.0f, 10.0f, 6.0f, true));
    }
    if (str_eq(name, "complex_plane"))
    {
        float x0 = ac >= 1 ? (float)as_num(sc, args[0], -4.0) : -4.0f;
        float x1 = ac >= 2 ? (float)as_num(sc, args[1],  4.0) :  4.0f;
        float y0 = ac >= 3 ? (float)as_num(sc, args[2], -3.0) : -3.0f;
        float y1 = ac >= 4 ? (float)as_num(sc, args[3],  3.0) :  3.0f;
        return val_mob(anim_complex_plane(s, x0, x1, 1.0f, y0, y1, 1.0f, 8.0f, 6.0f, true, NULL));
    }
    if (str_eq(name, "prepare_nonlinear") && ac >= 1)
    {
        anim_number_plane_prepare_nonlinear(s, as_mob(args[0]), ac >= 2 ? (uint32_t)as_num(sc, args[1], 4.0) : 4u);
        return args[0];
    }
    if (str_eq(name, "apply_complex") && !sc->in_play && ac >= 2 && args[1].kind == VAL_FN)
    {
        anim_vmob_apply_complex_fn(s, as_mob(args[0]), cb_complex_fn, make_cb_ctx(sc, args[1].u.fn),
                                   ac >= 3 ? (uint32_t)as_num(sc, args[2], 4.0) : 4u);
        return args[0];
    }
    if (str_eq(name, "save_state") && ac >= 1)
        { anim_save_state(s, as_mob(args[0])); return args[0]; }
    if (str_eq(name, "cutout") && ac >= 2)
    {
        AnimId holes[16];
        uint32_t hn = 0u;
        if (args[1].kind == VAL_LIST)
            for (uint32_t k = 0u; k < args[1].u.list.count && hn < 16u; ++k) holes[hn++] = as_mob(args[1].u.list.items[k]);
        else
            for (uint32_t k = 1u; k < ac && hn < 16u; ++k) holes[hn++] = as_mob(args[k]);
        return val_mob(anim_cutout(s, as_mob(args[0]), holes, hn));
    }
    if (str_eq(name, "rounded_polygon") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        AnimVec3 *pts = (AnimVec3 *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *pts);
        for (uint32_t k = 0u; k < n; ++k) pts[k] = as_vec(sc, args[0].u.list.items[k], ANIM_ORIGIN);
        return val_mob(anim_rounded_polygon(s, pts, n, ac >= 2 ? (float)as_num(sc, args[1], 0.18) : 0.18f));
    }
    if (str_eq(name, "elbow"))
        return val_mob(anim_elbow(s, ac >= 1 ? (float)as_num(sc, args[0], 0.25) : 0.25f, ac >= 2 ? (float)as_num(sc, args[1], 0.0) : 0.0f));
    if (str_eq(name, "arc_polygon") && ac >= 2 && args[0].kind == VAL_LIST && args[1].kind == VAL_LIST)
    {
        uint32_t n = MU_MIN(args[0].u.list.count, args[1].u.list.count);
        AnimVec3 *pts = (AnimVec3 *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *pts);
        float    *ang = (float *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *ang);
        for (uint32_t k = 0u; k < n; ++k)
        {
            pts[k] = as_vec(sc, args[0].u.list.items[k], ANIM_ORIGIN);
            ang[k] = (float)as_num(sc, args[1].u.list.items[k], 0.0);
        }
        return val_mob(anim_arc_polygon(s, pts, ang, n));
    }
    if (str_eq(name, "dashed") && ac >= 1)
        return val_mob(anim_dashed_vmobject(s, as_mob(args[0]), ac >= 2 ? (uint32_t)as_num(sc, args[1], 15.0) : 15u,
                                            ac >= 3 ? (float)as_num(sc, args[2], 0.5) : 0.5f));
    if (str_eq(name, "labeled_dot") && ac >= 1)
        return val_mob(anim_labeled_dot(s, as_str(args[0]) ? as_str(args[0]) : "A", NULL,
                                        ac >= 2 ? (float)as_num(sc, args[1], 0.35) : 0.35f,
                                        ac >= 3 ? as_col(args[2], ANIM_BLUE) : ANIM_BLUE,
                                        ac >= 4 ? as_col(args[3], ANIM_WHITE) : ANIM_WHITE));
    if (str_eq(name, "curved_arrow") && ac >= 2)
        return val_mob(anim_curved_arrow(s, as_vec(sc, args[0], ANIM_LEFT), as_vec(sc, args[1], ANIM_RIGHT),
                                         ac >= 3 ? (float)as_num(sc, args[2], ANIM_PI * 0.5) : ANIM_PI * 0.5f,
                                         ac >= 4 ? (float)as_num(sc, args[3], 0.22) : 0.22f));
    if (str_eq(name, "curved_double_arrow") && ac >= 2)
        return val_mob(anim_curved_double_arrow(s, as_vec(sc, args[0], ANIM_LEFT), as_vec(sc, args[1], ANIM_RIGHT),
                                                ac >= 3 ? (float)as_num(sc, args[2], ANIM_PI * 0.5) : ANIM_PI * 0.5f,
                                                ac >= 4 ? (float)as_num(sc, args[3], 0.22) : 0.22f));
    if (str_eq(name, "integer") && ac >= 1)
        return val_mob(anim_integer(s, (int)as_num(sc, args[0], 0.0), NULL, ac >= 2 ? (float)as_num(sc, args[1], 0.45) : 0.45f));
    if (str_eq(name, "surface_of_revolution") && ac >= 3 && args[0].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[0].u.fn);
        float t0 = (float)as_num(sc, args[1], -1.0);
        float t1 = (float)as_num(sc, args[2],  1.0);
        float th = ac >= 4 ? (float)as_num(sc, args[3], ANIM_TAU) : ANIM_TAU;
        return val_mob(anim_surface_of_revolution(s, cb_param_fn, t0, t1, th, 24u, 32u, ctx));
    }
    if (str_eq(name, "extrude") && ac >= 1)
        return val_mob(anim_extrude_vmob(s, as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 0.6) : 0.6f,
                                         ac >= 3 ? as_col(args[2], ANIM_BLUE) : ANIM_BLUE));
    if (str_eq(name, "voronoi") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        AnimVec3 *sites = (AnimVec3 *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *sites);
        AnimColor *cols = NULL;
        Val vb0, vb1, vc;
        AnimVec3 bmin = arg_get(arg_names, args, raw_args, ac, 1u, "min", &vb0, NULL) ? as_vec(sc, vb0, ANIM_V3(-4.0f, -3.0f, 0.0f)) : ANIM_V3(-4.0f, -3.0f, 0.0f);
        AnimVec3 bmax = arg_get(arg_names, args, raw_args, ac, 2u, "max", &vb1, NULL) ? as_vec(sc, vb1, ANIM_V3( 4.0f,  3.0f, 0.0f)) : ANIM_V3( 4.0f,  3.0f, 0.0f);
        for (uint32_t i = 0u; i < n; ++i) sites[i] = as_vec(sc, args[0].u.list.items[i], ANIM_ORIGIN);
        if (arg_get(arg_names, args, raw_args, ac, 3u, "colors", &vc, NULL) && vc.kind == VAL_LIST && vc.u.list.count >= n)
        {
            cols = (AnimColor *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *cols);
            for (uint32_t i = 0u; i < n; ++i) cols[i] = as_col(vc.u.list.items[i], ANIM_BLUE);
        }
        return val_mob(anim_voronoi(s, sites, n, bmin, bmax, cols));
    }
    if (str_eq(name, "particle_system") && ac >= 1)
    {
        Val vo, vv0, vv1, vg, vl, vc0, vc1, vr;
        uint32_t cnt  = (uint32_t)MU_MAX(as_num(sc, args[0], 32.0), 1.0);
        AnimVec3 orig = arg_get(arg_names, args, raw_args, ac, 1u, "origin",  &vo,  NULL) ? as_vec(sc, vo, ANIM_ORIGIN) : ANIM_ORIGIN;
        AnimVec3 vmin = arg_get(arg_names, args, raw_args, ac, 2u, "vel_min", &vv0, NULL) ? as_vec(sc, vv0, ANIM_V3(-1.0f,  1.0f, 0.0f)) : ANIM_V3(-1.0f, 1.0f, 0.0f);
        AnimVec3 vmax = arg_get(arg_names, args, raw_args, ac, 3u, "vel_max", &vv1, NULL) ? as_vec(sc, vv1, ANIM_V3( 1.0f,  3.0f, 0.0f)) : ANIM_V3( 1.0f, 3.0f, 0.0f);
        AnimVec3 grav = arg_get(arg_names, args, raw_args, ac, 4u, "gravity", &vg,  NULL) ? as_vec(sc, vg, ANIM_V3( 0.0f, -2.0f, 0.0f)) : ANIM_V3( 0.0f, -2.0f, 0.0f);
        float    life = arg_get(arg_names, args, raw_args, ac, 5u, "life",    &vl,  NULL) ? (float)as_num(sc, vl, 2.0) : 2.0f;
        AnimColor c0  = arg_get(arg_names, args, raw_args, ac, 6u, "c_start", &vc0, NULL) ? as_col(vc0, ANIM_GOLD) : ANIM_GOLD;
        AnimColor c1  = arg_get(arg_names, args, raw_args, ac, 7u, "c_end",   &vc1, NULL) ? as_col(vc1, ANIM_RED)  : ANIM_RED;
        float    rad  = arg_get(arg_names, args, raw_args, ac, 8u, "r",       &vr,  NULL) ? (float)as_num(sc, vr, 0.05) : 0.05f;
        return val_mob(anim_particle_system(s, cnt, orig, vmin, vmax, grav, life, c0, c1, rad));
    }
    if (str_eq(name, "matrix") && ac >= 1 && args[0].kind == VAL_LIST && args[0].u.list.count > 0u)
    {
        uint32_t rows = 1u, cols = 1u, dec = 1u;
        float ch = 0.42f;
        float *vals = NULL;
        Val vr, vc, vd, vsz;
        if (args[0].u.list.items[0].kind == VAL_LIST)
        {
            rows = args[0].u.list.count;
            cols = args[0].u.list.items[0].u.list.count;
            vals = (float *)sc_alloc(sc, MU_MAX(rows * cols, 1u) * sizeof *vals);
            for (uint32_t r = 0u; r < rows; ++r)
            {
                Val row = args[0].u.list.items[r];
                for (uint32_t c = 0u; c < cols; ++c)
                    vals[r * cols + c] = (row.kind == VAL_LIST && c < row.u.list.count) ? (float)as_num(sc, row.u.list.items[c], 0.0) : 0.0f;
            }
        }
        else
        {
            uint32_t tot = args[0].u.list.count;
            rows = arg_get(arg_names, args, raw_args, ac, 1u, "rows", &vr, NULL) ? (uint32_t)MU_MAX(as_num(sc, vr, 1.0), 1.0) : 1u;
            cols = arg_get(arg_names, args, raw_args, ac, 2u, "cols", &vc, NULL) ? (uint32_t)MU_MAX(as_num(sc, vc, (double)tot), 1.0) : (tot / rows);
            vals = (float *)sc_alloc(sc, MU_MAX(rows * cols, 1u) * sizeof *vals);
            for (uint32_t i = 0u; i < rows * cols && i < tot; ++i) vals[i] = (float)as_num(sc, args[0].u.list.items[i], 0.0);
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "decimals", &vd, NULL)) dec = (uint32_t)as_num(sc, vd, 1.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "size", &vsz, NULL))    ch  = (float)as_num(sc, vsz, 0.42);
        return val_mob(anim_matrix(s, vals, rows, cols, dec, NULL, ch));
    }
    if (str_eq(name, "table") && ac >= 1 && args[0].kind == VAL_LIST && args[0].u.list.count > 0u)
    {
        uint32_t rows = 1u, cols = 1u;
        float ch = 0.34f;
        bool hdr = true;
        const char **cells = NULL;
        Val vr, vc, vsz, vhdr;
        if (args[0].u.list.items[0].kind == VAL_LIST)
        {
            rows = args[0].u.list.count;
            cols = args[0].u.list.items[0].u.list.count;
            cells = (const char **)sc_alloc(sc, MU_MAX(rows * cols, 1u) * sizeof *cells);
            for (uint32_t r = 0u; r < rows; ++r)
            {
                Val row = args[0].u.list.items[r];
                for (uint32_t c = 0u; c < cols; ++c)
                {
                    const char *str = (row.kind == VAL_LIST && c < row.u.list.count && as_str(row.u.list.items[c])) ? as_str(row.u.list.items[c]) : "";
                    cells[r * cols + c] = str;
                }
            }
        }
        else
        {
            uint32_t tot = args[0].u.list.count;
            rows = arg_get(arg_names, args, raw_args, ac, 1u, "rows", &vr, NULL) ? (uint32_t)MU_MAX(as_num(sc, vr, 1.0), 1.0) : 1u;
            cols = arg_get(arg_names, args, raw_args, ac, 2u, "cols", &vc, NULL) ? (uint32_t)MU_MAX(as_num(sc, vc, (double)tot), 1.0) : (tot / rows);
            cells = (const char **)sc_alloc(sc, MU_MAX(rows * cols, 1u) * sizeof *cells);
            for (uint32_t i = 0u; i < rows * cols && i < tot; ++i) cells[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "size", &vsz, NULL))   ch  = (float)as_num(sc, vsz, 0.34);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "header", &vhdr, NULL)) hdr = as_bool(sc, vhdr);
        return val_mob(anim_table(s, cells, rows, cols, NULL, ch, hdr));
    }
    if (str_eq(name, "graph_network") && ac >= 2 && args[0].kind == VAL_LIST && args[1].kind == VAL_LIST)
    {
        uint32_t nc = args[0].u.list.count, ec = args[1].u.list.count;
        AnimVec3 *pos = (AnimVec3 *)sc_alloc(sc, MU_MAX(nc, 1u) * sizeof *pos);
        uint32_t *ef  = (uint32_t *)sc_alloc(sc, MU_MAX(ec, 1u) * sizeof *ef);
        uint32_t *et  = (uint32_t *)sc_alloc(sc, MU_MAX(ec, 1u) * sizeof *et);
        const char **lbls = NULL;
        bool dir = false;
        Val vlbl, vdir;
        for (uint32_t i = 0u; i < nc; ++i) pos[i] = as_vec(sc, args[0].u.list.items[i], ANIM_ORIGIN);
        for (uint32_t i = 0u; i < ec; ++i)
        {
            Val e = args[1].u.list.items[i];
            if (e.kind == VAL_LIST && e.u.list.count >= 2u)
                { ef[i] = (uint32_t)as_num(sc, e.u.list.items[0], 0.0); et[i] = (uint32_t)as_num(sc, e.u.list.items[1], 0.0); }
        }
        if (arg_get(arg_names, args, raw_args, ac, 2u, "labels", &vlbl, NULL) && vlbl.kind == VAL_LIST && vlbl.u.list.count > 0u)
        {
            lbls = (const char **)sc_alloc(sc, MU_MAX(nc, 1u) * sizeof *lbls);
            for (uint32_t i = 0u; i < nc; ++i) lbls[i] = (i < vlbl.u.list.count && as_str(vlbl.u.list.items[i])) ? as_str(vlbl.u.list.items[i]) : "";
        }
        if (arg_get(arg_names, args, raw_args, ac, 3u, "directed", &vdir, NULL)) dir = as_bool(sc, vdir);
        return val_mob(anim_graph_network(s, nc, pos, lbls, ec, ef, et, dir, NULL));
    }
    if (str_eq(name, "graph_layout") && ac >= 2 && args[1].kind == VAL_LIST)
    {
        uint32_t nc = (uint32_t)MU_MAX(as_num(sc, args[0], 4.0), 1.0);
        uint32_t ec = args[1].u.list.count;
        uint32_t *ef = (uint32_t *)sc_alloc(sc, MU_MAX(ec, 1u) * sizeof *ef);
        uint32_t *et = (uint32_t *)sc_alloc(sc, MU_MAX(ec, 1u) * sizeof *et);
        const char **lbls = NULL;
        bool dir = false;
        AnimGraphLayoutKind lk = ANIM_GRAPH_LAYOUT_SPRING;
        float w = 7.0f, h = 5.0f;
        Val vlay, vlbl, vdir, vw, vh;
        for (uint32_t i = 0u; i < ec; ++i)
        {
            Val e = args[1].u.list.items[i];
            if (e.kind == VAL_LIST && e.u.list.count >= 2u)
                { ef[i] = (uint32_t)as_num(sc, e.u.list.items[0], 0.0); et[i] = (uint32_t)as_num(sc, e.u.list.items[1], 0.0); }
        }
        if (arg_get(arg_names, args, raw_args, ac, 2u, "layout", &vlay, NULL) && as_str(vlay))
        {
            const char *ls = as_str(vlay);
            if (str_eq(ls, "circular"))       lk = ANIM_GRAPH_LAYOUT_CIRCULAR;
            else if (str_eq(ls, "grid"))      lk = ANIM_GRAPH_LAYOUT_GRID;
            else if (str_eq(ls, "tree"))      lk = ANIM_GRAPH_LAYOUT_TREE;
            else if (str_eq(ls, "bipartite")) lk = ANIM_GRAPH_LAYOUT_BIPARTITE;
            else                              lk = ANIM_GRAPH_LAYOUT_SPRING;
        }
        if (arg_get(arg_names, args, raw_args, ac, 3u, "labels", &vlbl, NULL) && vlbl.kind == VAL_LIST && vlbl.u.list.count > 0u)
        {
            lbls = (const char **)sc_alloc(sc, MU_MAX(nc, 1u) * sizeof *lbls);
            for (uint32_t i = 0u; i < nc; ++i) lbls[i] = (i < vlbl.u.list.count && as_str(vlbl.u.list.items[i])) ? as_str(vlbl.u.list.items[i]) : "";
        }
        if (arg_get(arg_names, args, raw_args, ac, 4u, "directed", &vdir, NULL)) dir = as_bool(sc, vdir);
        if (arg_get(arg_names, args, raw_args, ac, 5u, "width",  &vw, NULL)) w = (float)as_num(sc, vw, 7.0);
        if (arg_get(arg_names, args, raw_args, ac, 6u, "height", &vh, NULL)) h = (float)as_num(sc, vh, 5.0);
        return val_mob(anim_graph_with_layout(s, nc, lbls, ec, ef, et, dir, lk, w, h, NULL));
    }

    /* ---- Text, Math, Images & UI ---- */
    if (str_eq(name, "text") && ac >= 1)
    {
        const char *txt = as_str(args[0]) ? as_str(args[0]) : "";
        Val vsz, vfp;
        float wh = arg_get(arg_names, args, raw_args, ac, 1u, "size", &vsz, NULL) ? (float)as_num(sc, vsz, 0.5) : 0.5f;
        const char *fp = (arg_get(arg_names, args, raw_args, ac, 2u, "font", &vfp, NULL) && as_str(vfp)) ? as_str(vfp) : NULL;
        return val_mob(anim_text(s, fp, txt, wh));
    }
    if (str_eq(name, "markup") && ac >= 1)
    {
        const char *txt = as_str(args[0]) ? as_str(args[0]) : "";
        Val vsz, vfp;
        float wh = arg_get(arg_names, args, raw_args, ac, 1u, "size", &vsz, NULL) ? (float)as_num(sc, vsz, 0.5) : 0.5f;
        const char *fp = (arg_get(arg_names, args, raw_args, ac, 2u, "font", &vfp, NULL) && as_str(vfp)) ? as_str(vfp) : NULL;
        return val_mob(anim_markup(s, fp, txt, wh));
    }
    if (str_eq(name, "t2c") && ac >= 4)
    {
        anim_text_set_t2c(s, as_mob(args[0]), as_str(args[1]) ? as_str(args[1]) : "",
                          as_str(args[2]) ? as_str(args[2]) : "", as_col(args[3], ANIM_GOLD));
        return args[0];
    }
    if (str_eq(name, "tangent_line") && ac >= 1)
        return val_mob(anim_tangent_line(s, as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 0.5) : 0.5f,
                                         ac >= 3 ? (float)as_num(sc, args[2], 2.5) : 2.5f));
    if (str_eq(name, "secant_slope") && ac >= 4 && args[1].kind == VAL_FN)
    {
        ScriptCbCtx *ctx = make_cb_ctx(sc, args[1].u.fn);
        return val_mob(anim_secant_slope_group(s, as_mob(args[0]), cb_scalar_fn,
                                               (float)as_num(sc, args[2], 0.0),
                                               (float)as_num(sc, args[3], 1.0),
                                               ac >= 5 ? (float)as_num(sc, args[4], 3.0) : 3.0f,
                                               ctx, ANIM_YELLOW, ANIM_BLUE, ANIM_GREEN));
    }
    if (str_eq(name, "tex") && ac >= 1)
    {
        Val vsz, vbk, val;
        float wh = arg_get(arg_names, args, raw_args, ac, 1u, "size", &vsz, NULL) ? (float)as_num(sc, vsz, 0.65) : 0.65f;
        AnimTexBackend bk = ANIM_TEX_AUTO;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "backend", &vbk, NULL) && as_str(vbk))
        {
            if (str_eq(as_str(vbk), "typst")) bk = ANIM_TEX_TYPST;
            else if (str_eq(as_str(vbk), "latex")) bk = ANIM_TEX_LATEX;
        }
        if (args[0].kind == VAL_LIST)
        {
            const char *pts[64];
            uint32_t n = MU_MIN(args[0].u.list.count, 64u);
            for (uint32_t i = 0u; i < n; ++i)
                pts[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
            if (arg_get(arg_names, args, raw_args, ac, 99u, "align", &val, NULL) && as_bool(sc, val))
            {
                Val vgap;
                float gap = arg_get(arg_names, args, raw_args, ac, 2u, "gap", &vgap, NULL) ? (float)as_num(sc, vgap, 0.25) : 0.25f;
                return val_mob(anim_tex_align(s, bk, pts, n, wh, gap));
            }
            return val_mob(anim_tex_parts(s, bk, pts, n, wh));
        }
        {
            const char *expr = as_str(args[0]) ? as_str(args[0]) : "";
            return val_mob(anim_tex(s, bk, expr, wh));
        }
    }
    if (str_eq(name, "paragraph") && ac >= 1)
        return val_mob(anim_paragraph(s, NULL, as_str(args[0]) ? as_str(args[0]) : "",
                                      ac >= 2 ? (float)as_num(sc, args[1], 0.35) : 0.35f,
                                      ac >= 3 ? (float)as_num(sc, args[2], 0.20) : 0.20f,
                                      ANIM_ALIGN_LEFT));
    if (str_eq(name, "code") && ac >= 1)
        return val_mob(anim_code(s, NULL, as_str(args[0]) ? as_str(args[0]) : "",
                                 ac >= 2 ? (float)as_num(sc, args[1], 0.28) : 0.28f,
                                 ac >= 3 ? as_bool(sc, args[2]) : true));
    if (str_eq(name, "svg") && ac >= 1)
        return val_mob(anim_svg_from_file(s, as_str(args[0]) ? as_str(args[0]) : "",
                                          ac >= 2 ? (float)as_num(sc, args[1], 2.0) : 2.0f));
    if (str_eq(name, "svg_str") && ac >= 1)
    {
        const char *xml = as_str(args[0]) ? as_str(args[0]) : "";
        return val_mob(anim_svg_from_memory(s, xml, strlen(xml), ac >= 2 ? (float)as_num(sc, args[1], 2.0) : 2.0f));
    }
    if (str_eq(name, "table_cell") && ac >= 3)
        return val_mob(anim_table_cell(s, as_mob(args[0]), (uint32_t)as_num(sc, args[1], 0.0), (uint32_t)as_num(sc, args[2], 0.0)));
    if (str_eq(name, "highlight_line") && ac >= 2)
    {
        Val vcol;
        AnimColor col = arg_get(arg_names, args, raw_args, ac, 2u, "color", &vcol, NULL) ? as_col(vcol, ANIM_YELLOW) : ANIM_YELLOW;
        return val_mob(anim_code_highlight_line(s, as_mob(args[0]), (uint32_t)MU_MAX(as_num(sc, args[1], 1.0), 1.0), col));
    }
    if (str_eq(name, "image") && ac >= 1)
        return val_mob(anim_image(s, as_str(args[0]) ? as_str(args[0]) : "",
                                  ac >= 2 ? (float)as_num(sc, args[1], 2.5) : 2.5f));
    if (str_eq(name, "decimal") && ac >= 1)
    {
        uint32_t dec = ac >= 2 ? (uint32_t)as_num(sc, args[1], 2.0) : 2u;
        float    wh  = ac >= 3 ? (float)as_num(sc, args[2], 0.45) : 0.45f;
        AnimId   id  = anim_decimal_number(s, (float)as_num(sc, args[0], 0.0), dec, NULL, wh);
        if (expr_is_reactive(sc, raw_args[0]))
            attach_react(sc, id, RB_DECIMAL, raw_args[0], NULL, NULL, ANIM_ID_NIL, ANIM_ORIGIN, 0.0f, 0.0f);
        return val_mob(id);
    }
    if (str_eq(name, "variable") && ac >= 2 && args[1].kind == VAL_TRACKER)
        return val_mob(anim_variable(s, as_str(args[0]) ? as_str(args[0]) : "x", args[1].u.tracker,
                                     ac >= 3 ? (uint32_t)as_num(sc, args[2], 2.0) : 2u, NULL,
                                     ac >= 4 ? (float)as_num(sc, args[3], 0.45) : 0.45f));
    if (str_eq(name, "tex_diff") && ac >= 2 && args[0].kind == VAL_LIST && args[1].kind == VAL_LIST)
    {
        const char *pa[64], *pb[64];
        uint32_t na = MU_MIN(args[0].u.list.count, 64u);
        uint32_t nb = MU_MIN(args[1].u.list.count, 64u);
        Val vsz, vdel, vadd;
        float wh = arg_get(arg_names, args, raw_args, ac, 2u, "size", &vsz, NULL) ? (float)as_num(sc, vsz, 0.65) : 0.65f;
        AnimColor cdel = arg_get(arg_names, args, raw_args, ac, 3u, "del_color", &vdel, NULL) ? as_col(vdel, ANIM_RED) : ANIM_RED;
        AnimColor cadd = arg_get(arg_names, args, raw_args, ac, 4u, "add_color", &vadd, NULL) ? as_col(vadd, ANIM_GREEN) : ANIM_GREEN;
        for (uint32_t i = 0u; i < na; ++i) pa[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
        for (uint32_t i = 0u; i < nb; ++i) pb[i] = as_str(args[1].u.list.items[i]) ? as_str(args[1].u.list.items[i]) : "";
        return val_mob(anim_tex_diff(s, ANIM_TEX_AUTO, pa, na, pb, nb, wh, cdel, cadd, NULL, NULL));
    }
    if (str_eq(name, "bulleted_list") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        const char *items[64];
        uint32_t n = MU_MIN(args[0].u.list.count, 64u);
        Val vsz, vsp;
        float th = arg_get(arg_names, args, raw_args, ac, 1u, "size", &vsz, NULL) ? (float)as_num(sc, vsz, 0.35) : 0.35f;
        float sp = arg_get(arg_names, args, raw_args, ac, 2u, "spacing", &vsp, NULL) ? (float)as_num(sc, vsp, 0.20) : 0.20f;
        for (uint32_t i = 0u; i < n; ++i) items[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
        return val_mob(anim_bulleted_list(s, NULL, items, n, th, sp));
    }

    /* ---- Physics & CS Toolkit ---- */
    if (str_eq(name, "phys_spring") && ac >= 2)
    {
        Val va, vb, vco, vw;
        const Expr *ra = NULL, *rb = NULL;
        arg_get(arg_names, args, raw_args, ac, 0u, "a", &va, &ra);
        arg_get(arg_names, args, raw_args, ac, 1u, "b", &vb, &rb);
        uint32_t coils = arg_get(arg_names, args, raw_args, ac, 2u, "coils", &vco, NULL) ? (uint32_t)MU_MAX(as_num(sc, vco, 8.0), 1.0) : 8u;
        float width    = arg_get(arg_names, args, raw_args, ac, 3u, "width", &vw,  NULL) ? (float)as_num(sc, vw, 0.28) : 0.28f;
        AnimId id = anim_phys_spring(s, as_vec(sc, va, ANIM_ORIGIN), as_vec(sc, vb, ANIM_RIGHT), coils, width);
        if ((ra && expr_is_reactive(sc, ra)) || (rb && expr_is_reactive(sc, rb)))
            attach_react_plot(sc, id, RB_PHYS_SPRING, ANIM_ID_NIL, NULL, ra, rb, NULL, NULL, width, 0.0f, 0.0f, 0.0f, coils, 0u, ANIM_WHITE, ANIM_WHITE, NULL);
        return val_mob(id);
    }
    if (str_eq(name, "phys_pendulum") && ac >= 1)
    {
        Val vp, vl, va, vr;
        const Expr *rp = NULL, *ra = NULL;
        arg_get(arg_names, args, raw_args, ac, 0u, "pivot", &vp, &rp);
        float len = arg_get(arg_names, args, raw_args, ac, 1u, "len",   &vl, NULL) ? (float)as_num(sc, vl, 2.0) : 2.0f;
        float ang = arg_get(arg_names, args, raw_args, ac, 2u, "angle", &va, &ra)  ? (float)as_num(sc, va, 0.4) : 0.4f;
        float rad = arg_get(arg_names, args, raw_args, ac, 3u, "r",     &vr, NULL) ? (float)as_num(sc, vr, 0.16) : 0.16f;
        AnimVec3 piv = as_vec(sc, vp, ANIM_UP);
        AnimId id = anim_phys_pendulum(s, piv, len, ang, rad);
        if ((rp && expr_is_reactive(sc, rp)) || (ra && expr_is_reactive(sc, ra)))
        {
            ReactBinding *rb = (ReactBinding *)sc_alloc(sc, sizeof *rb);
            AnimTrackerId trks[ANIM_MAX_DEPS]; AnimId mobs[ANIM_MAX_DEPS]; uint32_t tc = 0u, mc = 0u;
            collect_reactive_deps(sc, rp, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
            collect_reactive_deps(sc, ra, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
            if (tc > 0u)
            {
                rb->sc = sc; rb->kind = RB_PHYS_PENDULUM; rb->target = id;
                rb->e0 = (Expr *)rp; rb->e1 = (Expr *)ra; rb->dir = piv; rb->f0 = len; rb->f1 = ang;
                anim_add_updater(s, id, script_reactive_updater, rb, mobs, mc, trks, tc);
            }
        }
        return val_mob(id);
    }
    if (str_eq(name, "phys_free_body") && ac >= 1)
    {
        Val vc, vw, vh, vf, vl, vco;
        arg_get(arg_names, args, raw_args, ac, 0u, "center", &vc, NULL);
        float bw = arg_get(arg_names, args, raw_args, ac, 1u, "w", &vw, NULL) ? (float)as_num(sc, vw, 1.2) : 1.2f;
        float bh = arg_get(arg_names, args, raw_args, ac, 2u, "h", &vh, NULL) ? (float)as_num(sc, vh, 0.8) : 0.8f;
        uint32_t fc = 0u;
        AnimVec3 *fvec = NULL; const char **lbls = NULL; AnimColor *cols = NULL;
        if (arg_get(arg_names, args, raw_args, ac, 3u, "forces", &vf, NULL) && vf.kind == VAL_LIST && vf.u.list.count > 0u)
        {
            fc = vf.u.list.count;
            fvec = (AnimVec3 *)sc_alloc(sc, fc * sizeof *fvec);
            for (uint32_t i = 0u; i < fc; ++i) fvec[i] = as_vec(sc, vf.u.list.items[i], ANIM_ORIGIN);
            if (arg_get(arg_names, args, raw_args, ac, 4u, "labels", &vl, NULL) && vl.kind == VAL_LIST)
            {
                lbls = (const char **)sc_alloc(sc, fc * sizeof *lbls);
                for (uint32_t i = 0u; i < fc; ++i) lbls[i] = (i < vl.u.list.count && as_str(vl.u.list.items[i])) ? as_str(vl.u.list.items[i]) : "";
            }
            if (arg_get(arg_names, args, raw_args, ac, 5u, "colors", &vco, NULL) && vco.kind == VAL_LIST)
            {
                cols = (AnimColor *)sc_alloc(sc, fc * sizeof *cols);
                for (uint32_t i = 0u; i < fc; ++i) cols[i] = (i < vco.u.list.count) ? as_col(vco.u.list.items[i], ANIM_YELLOW) : ANIM_YELLOW;
            }
        }
        return val_mob(anim_phys_free_body(s, as_vec(sc, vc, ANIM_ORIGIN), bw, bh, fvec, lbls, cols, fc, NULL));
    }
    if (str_eq(name, "phys_wave") && ac >= 2)
    {
        Val va, vb, vamp, vcyc, vph, vsmp;
        const Expr *ramp = NULL, *rcyc = NULL, *rph = NULL;
        arg_get(arg_names, args, raw_args, ac, 0u, "a", &va, NULL);
        arg_get(arg_names, args, raw_args, ac, 1u, "b", &vb, NULL);
        float amp   = arg_get(arg_names, args, raw_args, ac, 2u, "amp",     &vamp, &ramp) ? (float)as_num(sc, vamp, 0.4) : 0.4f;
        float cyc   = arg_get(arg_names, args, raw_args, ac, 3u, "cycles",  &vcyc, &rcyc) ? (float)as_num(sc, vcyc, 3.0) : 3.0f;
        float phase = arg_get(arg_names, args, raw_args, ac, 4u, "phase",   &vph,  &rph)  ? (float)as_num(sc, vph,  0.0) : 0.0f;
        uint32_t smp = arg_get(arg_names, args, raw_args, ac, 5u, "samples", &vsmp, NULL) ? (uint32_t)MU_MAX(as_num(sc, vsmp, 64.0), 4.0) : 64u;
        AnimVec3 pa = as_vec(sc, va, ANIM_V3(-3.0f, 0.0f, 0.0f));
        AnimVec3 pb = as_vec(sc, vb, ANIM_V3( 3.0f, 0.0f, 0.0f));
        AnimId id = anim_phys_wave(s, pa, pb, amp, cyc, phase, smp);
        if ((ramp && expr_is_reactive(sc, ramp)) || (rcyc && expr_is_reactive(sc, rcyc)) || (rph && expr_is_reactive(sc, rph)))
        {
            AnimTrackerId trks[ANIM_MAX_DEPS]; AnimId mobs[ANIM_MAX_DEPS]; uint32_t tc = 0u, mc = 0u;
            collect_reactive_deps(sc, ramp, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
            collect_reactive_deps(sc, rcyc, trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
            collect_reactive_deps(sc, rph,  trks, &tc, ANIM_MAX_DEPS, mobs, &mc, ANIM_MAX_DEPS, 0);
            if (tc > 0u)
            {
                ReactBinding *rb = (ReactBinding *)sc_alloc(sc, sizeof *rb);
                rb->sc = sc; rb->kind = RB_PHYS_WAVE; rb->target = id;
                rb->e0 = (Expr *)ramp; rb->e1 = (Expr *)rcyc; rb->e2 = (Expr *)rph;
                rb->dir = pa; rb->f3 = pb.x; rb->c0.r = pb.y; rb->c0.g = pb.z;
                rb->f0 = amp; rb->f1 = cyc; rb->f2 = phase; rb->u0 = smp;
                anim_add_updater(s, id, script_reactive_updater, rb, mobs, mc, trks, tc);
            }
        }
        return val_mob(id);
    }
    if (str_eq(name, "phys_charge") && ac >= 1)
    {
        Val vp, vr, vs;
        arg_get(arg_names, args, raw_args, ac, 0u, "pos", &vp, NULL);
        float rad = arg_get(arg_names, args, raw_args, ac, 1u, "r",    &vr, NULL) ? (float)as_num(sc, vr, 0.22) : 0.22f;
        int   sgn = arg_get(arg_names, args, raw_args, ac, 2u, "sign", &vs, NULL) ? (int)as_num(sc, vs, 1.0) : 1;
        return val_mob(anim_phys_charge(s, as_vec(sc, vp, ANIM_ORIGIN), rad, sgn));
    }
    if (str_eq(name, "phys_field_lines") && ac >= 2 && args[0].kind == VAL_LIST && args[1].kind == VAL_LIST)
    {
        uint32_t nc = MU_MIN(args[0].u.list.count, args[1].u.list.count);
        AnimVec3 *cpos = (AnimVec3 *)sc_alloc(sc, MU_MAX(nc, 1u) * sizeof *cpos);
        float    *cval = (float *)sc_alloc(sc, MU_MAX(nc, 1u) * sizeof *cval);
        Val vb0, vb1, vln;
        AnimVec3 bmin = arg_get(arg_names, args, raw_args, ac, 2u, "min",   &vb0, NULL) ? as_vec(sc, vb0, ANIM_V3(-4.0f, -3.0f, 0.0f)) : ANIM_V3(-4.0f, -3.0f, 0.0f);
        AnimVec3 bmax = arg_get(arg_names, args, raw_args, ac, 3u, "max",   &vb1, NULL) ? as_vec(sc, vb1, ANIM_V3( 4.0f,  3.0f, 0.0f)) : ANIM_V3( 4.0f,  3.0f, 0.0f);
        uint32_t lpc  = arg_get(arg_names, args, raw_args, ac, 4u, "lines", &vln, NULL) ? (uint32_t)MU_MAX(as_num(sc, vln, 12.0), 1.0) : 12u;
        for (uint32_t i = 0u; i < nc; ++i)
        {
            cpos[i] = as_vec(sc, args[0].u.list.items[i], ANIM_ORIGIN);
            cval[i] = (float)as_num(sc, args[1].u.list.items[i], 1.0);
        }
        return val_mob(anim_phys_field_lines(s, cpos, cval, nc, bmin, bmax, lpc));
    }
    if (str_eq(name, "phys_lens"))
    {
        Val vc, vh, vt, vd;
        AnimVec3 ctr = arg_get(arg_names, args, raw_args, ac, 0u, "center",    &vc, NULL) ? as_vec(sc, vc, ANIM_ORIGIN) : ANIM_ORIGIN;
        float    h   = arg_get(arg_names, args, raw_args, ac, 1u, "height",    &vh, NULL) ? (float)as_num(sc, vh, 2.4) : 2.4f;
        float    th  = arg_get(arg_names, args, raw_args, ac, 2u, "thickness", &vt, NULL) ? (float)as_num(sc, vt, 0.28) : 0.28f;
        bool     div = arg_get(arg_names, args, raw_args, ac, 3u, "diverging", &vd, NULL) ? as_bool(sc, vd) : false;
        return val_mob(anim_phys_lens(s, ctr, h, th, div));
    }
    if (str_eq(name, "phys_ray_bundle"))
    {
        Val vc, vf, vh, vo, vx0, vx1;
        AnimVec3 ctr = arg_get(arg_names, args, raw_args, ac, 0u, "center",  &vc,  NULL) ? as_vec(sc, vc, ANIM_ORIGIN) : ANIM_ORIGIN;
        float    foc = arg_get(arg_names, args, raw_args, ac, 1u, "focal",   &vf,  NULL) ? (float)as_num(sc, vf, 2.0) : 2.0f;
        float    h   = arg_get(arg_names, args, raw_args, ac, 2u, "height",  &vh,  NULL) ? (float)as_num(sc, vh, 2.4) : 2.4f;
        float    x0  = arg_get(arg_names, args, raw_args, ac, 4u, "x0",      &vx0, NULL) ? (float)as_num(sc, vx0, -4.0) : -4.0f;
        float    x1  = arg_get(arg_names, args, raw_args, ac, 5u, "x1",      &vx1, NULL) ? (float)as_num(sc, vx1,  4.0) :  4.0f;
        float   *offs = NULL;
        uint32_t rc = 5u;
        if (arg_get(arg_names, args, raw_args, ac, 3u, "offsets", &vo, NULL) && vo.kind == VAL_LIST && vo.u.list.count > 0u)
        {
            rc = vo.u.list.count;
            offs = (float *)sc_alloc(sc, rc * sizeof *offs);
            for (uint32_t i = 0u; i < rc; ++i) offs[i] = (float)as_num(sc, vo.u.list.items[i], 0.0);
        }
        return val_mob(anim_phys_ray_bundle(s, ctr, foc, h, offs, rc, x0, x1));
    }
    if (str_eq(name, "cs_neural_net") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t nl = args[0].u.list.count;
        uint32_t *ls = (uint32_t *)sc_alloc(sc, MU_MAX(nl, 1u) * sizeof *ls);
        Val vw, vh, vr, vc0, vc1;
        float w = arg_get(arg_names, args, raw_args, ac, 1u, "width",  &vw,  NULL) ? (float)as_num(sc, vw, 5.5) : 5.5f;
        float h = arg_get(arg_names, args, raw_args, ac, 2u, "height", &vh,  NULL) ? (float)as_num(sc, vh, 3.8) : 3.8f;
        float r = arg_get(arg_names, args, raw_args, ac, 3u, "r",      &vr,  NULL) ? (float)as_num(sc, vr, 0.16) : 0.16f;
        AnimColor c0 = arg_get(arg_names, args, raw_args, ac, 4u, "c_in",  &vc0, NULL) ? as_col(vc0, ANIM_BLUE) : ANIM_BLUE;
        AnimColor c1 = arg_get(arg_names, args, raw_args, ac, 5u, "c_out", &vc1, NULL) ? as_col(vc1, ANIM_GOLD) : ANIM_GOLD;
        for (uint32_t i = 0u; i < nl; ++i) ls[i] = (uint32_t)MU_MAX(as_num(sc, args[0].u.list.items[i], 1.0), 1.0);
        return val_mob(anim_cs_neural_net(s, ls, nl, w, h, r, c0, c1));
    }
    if (str_eq(name, "cs_array") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        const char **lbls = (const char **)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *lbls);
        Val vw, vh, vi;
        float cw = arg_get(arg_names, args, raw_args, ac, 1u, "w",       &vw, NULL) ? (float)as_num(sc, vw, 0.9) : 0.9f;
        float ch = arg_get(arg_names, args, raw_args, ac, 2u, "h",       &vh, NULL) ? (float)as_num(sc, vh, 0.7) : 0.7f;
        bool idx = arg_get(arg_names, args, raw_args, ac, 3u, "indices", &vi, NULL) ? as_bool(sc, vi) : true;
        for (uint32_t i = 0u; i < n; ++i) lbls[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
        return val_mob(anim_cs_array(s, lbls, n, cw, ch, idx, NULL, NULL));
    }
    if (str_eq(name, "cs_bar_chart") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        float *vals = (float *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *vals);
        const char **lbls = NULL;
        Val vl, vw, vh, vc0, vc1;
        float w = arg_get(arg_names, args, raw_args, ac, 2u, "width",  &vw,  NULL) ? (float)as_num(sc, vw, 5.0) : 5.0f;
        float h = arg_get(arg_names, args, raw_args, ac, 3u, "height", &vh,  NULL) ? (float)as_num(sc, vh, 3.0) : 3.0f;
        AnimColor c0 = arg_get(arg_names, args, raw_args, ac, 4u, "c_low",  &vc0, NULL) ? as_col(vc0, ANIM_BLUE) : ANIM_BLUE;
        AnimColor c1 = arg_get(arg_names, args, raw_args, ac, 5u, "c_high", &vc1, NULL) ? as_col(vc1, ANIM_GOLD) : ANIM_GOLD;
        for (uint32_t i = 0u; i < n; ++i) vals[i] = (float)as_num(sc, args[0].u.list.items[i], 0.0);
        if (arg_get(arg_names, args, raw_args, ac, 1u, "labels", &vl, NULL) && vl.kind == VAL_LIST)
        {
            lbls = (const char **)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *lbls);
            for (uint32_t i = 0u; i < n; ++i) lbls[i] = (i < vl.u.list.count && as_str(vl.u.list.items[i])) ? as_str(vl.u.list.items[i]) : "";
        }
        return val_mob(anim_cs_bar_chart(s, vals, lbls, n, w, h, c0, c1, NULL, NULL));
    }
    if (str_eq(name, "cs_linked_list") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        const char **lbls = (const char **)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *lbls);
        Val vw, vh, vs;
        float nw = arg_get(arg_names, args, raw_args, ac, 1u, "w",       &vw, NULL) ? (float)as_num(sc, vw, 0.9) : 0.9f;
        float nh = arg_get(arg_names, args, raw_args, ac, 2u, "h",       &vh, NULL) ? (float)as_num(sc, vh, 0.6) : 0.6f;
        float sp = arg_get(arg_names, args, raw_args, ac, 3u, "spacing", &vs, NULL) ? (float)as_num(sc, vs, 0.5) : 0.5f;
        for (uint32_t i = 0u; i < n; ++i) lbls[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
        return val_mob(anim_cs_linked_list(s, lbls, n, nw, nh, sp, NULL, NULL));
    }
    if (str_eq(name, "cs_stack_queue") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        const char **lbls = (const char **)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *lbls);
        Val vq, vw, vh;
        bool q   = arg_get(arg_names, args, raw_args, ac, 1u, "queue", &vq, NULL) ? as_bool(sc, vq) : false;
        float iw = arg_get(arg_names, args, raw_args, ac, 2u, "w",     &vw, NULL) ? (float)as_num(sc, vw, 1.1) : 1.1f;
        float ih = arg_get(arg_names, args, raw_args, ac, 3u, "h",     &vh, NULL) ? (float)as_num(sc, vh, 0.55) : 0.55f;
        for (uint32_t i = 0u; i < n; ++i) lbls[i] = as_str(args[0].u.list.items[i]) ? as_str(args[0].u.list.items[i]) : "";
        return val_mob(anim_cs_stack_queue(s, lbls, n, q, iw, ih, NULL));
    }
    if (str_eq(name, "cs_binary_tree") && ac >= 1 && args[0].kind == VAL_LIST)
    {
        uint32_t n = args[0].u.list.count;
        const char **lbls = (const char **)sc_alloc(sc, MU_MAX(n, 1u) * sizeof *lbls);
        Val vdy, vsp, vr;
        float dy = arg_get(arg_names, args, raw_args, ac, 1u, "dy",     &vdy, NULL) ? (float)as_num(sc, vdy, 1.1) : 1.1f;
        float sp = arg_get(arg_names, args, raw_args, ac, 2u, "spread", &vsp, NULL) ? (float)as_num(sc, vsp, 2.6) : 2.6f;
        float r  = arg_get(arg_names, args, raw_args, ac, 3u, "r",      &vr,  NULL) ? (float)as_num(sc, vr, 0.28) : 0.28f;
        for (uint32_t i = 0u; i < n; ++i) lbls[i] = as_str(args[0].u.list.items[i]);
        return val_mob(anim_cs_binary_tree(s, lbls, n, dy, sp, r, NULL, NULL));
    }
    if (str_eq(name, "cs_fsm") && ac >= 3 && args[0].kind == VAL_LIST && args[1].kind == VAL_LIST && args[2].kind == VAL_LIST)
    {
        uint32_t scnt = MU_MIN(args[0].u.list.count, args[1].u.list.count);
        uint32_t ecnt = args[2].u.list.count;
        AnimVec3 *pos = (AnimVec3 *)sc_alloc(sc, MU_MAX(scnt, 1u) * sizeof *pos);
        const char **slbls = (const char **)sc_alloc(sc, MU_MAX(scnt, 1u) * sizeof *slbls);
        bool *acc = (bool *)sc_alloc(sc, MU_MAX(scnt, 1u) * sizeof *acc);
        uint32_t *ef = (uint32_t *)sc_alloc(sc, MU_MAX(ecnt, 1u) * sizeof *ef);
        uint32_t *et = (uint32_t *)sc_alloc(sc, MU_MAX(ecnt, 1u) * sizeof *et);
        const char **elbls = (const char **)sc_alloc(sc, MU_MAX(ecnt, 1u) * sizeof *elbls);
        Val vacc, vst, vr;
        uint32_t start = arg_get(arg_names, args, raw_args, ac, 99u, "start", &vst, NULL) ? (uint32_t)as_num(sc, vst, 0.0) : 0u;
        float nr       = arg_get(arg_names, args, raw_args, ac, 99u, "r",     &vr,  NULL) ? (float)as_num(sc, vr, 0.36) : 0.36f;
        for (uint32_t i = 0u; i < scnt; ++i)
        {
            pos[i]   = as_vec(sc, args[0].u.list.items[i], ANIM_ORIGIN);
            slbls[i] = as_str(args[1].u.list.items[i]) ? as_str(args[1].u.list.items[i]) : "";
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "accept", &vacc, NULL) && vacc.kind == VAL_LIST)
        {
            for (uint32_t i = 0u; i < vacc.u.list.count; ++i)
            {
                if (vacc.u.list.items[i].kind == VAL_BOOL && i < scnt) acc[i] = vacc.u.list.items[i].u.b;
                else if (vacc.u.list.items[i].kind == VAL_NUM)
                {
                    uint32_t idx = (uint32_t)vacc.u.list.items[i].u.num;
                    if (idx < scnt) acc[idx] = true;
                }
            }
        }
        for (uint32_t i = 0u; i < ecnt; ++i)
        {
            Val e = args[2].u.list.items[i];
            if (e.kind == VAL_LIST && e.u.list.count >= 2u)
            {
                ef[i] = (uint32_t)as_num(sc, e.u.list.items[0], 0.0);
                et[i] = (uint32_t)as_num(sc, e.u.list.items[1], 0.0);
                elbls[i] = (e.u.list.count >= 3u && as_str(e.u.list.items[2])) ? as_str(e.u.list.items[2]) : "";
            }
        }
        return val_mob(anim_cs_fsm(s, scnt, pos, slbls, acc, start, ecnt, ef, et, elbls, nr, NULL));
    }
    if (str_eq(name, "cs_logic_gate") && ac >= 1)
    {
        AnimLogicGateKind gk = ANIM_GATE_AND;
        if (args[0].kind == VAL_STR)
        {
            const char *ks = args[0].u.str;
            if (str_eq(ks, "or")) gk = ANIM_GATE_OR;
            else if (str_eq(ks, "xor"))  gk = ANIM_GATE_XOR;
            else if (str_eq(ks, "not"))  gk = ANIM_GATE_NOT;
            else if (str_eq(ks, "nand")) gk = ANIM_GATE_NAND;
            else if (str_eq(ks, "nor"))  gk = ANIM_GATE_NOR;
        }
        else
            gk = (AnimLogicGateKind)(int)as_num(sc, args[0], 0.0);
        Val vp, vsc, vl;
        AnimVec3 pos = arg_get(arg_names, args, raw_args, ac, 1u, "pos",   &vp,  NULL) ? as_vec(sc, vp, ANIM_ORIGIN) : ANIM_ORIGIN;
        float    scl = arg_get(arg_names, args, raw_args, ac, 2u, "scale", &vsc, NULL) ? (float)as_num(sc, vsc, 1.0) : 1.0f;
        const char *lb = (arg_get(arg_names, args, raw_args, ac, 3u, "label", &vl, NULL) && as_str(vl)) ? as_str(vl) : NULL;
        return val_mob(anim_cs_logic_gate(s, gk, pos, scl, lb, NULL));
    }
    if (str_eq(name, "cs_bit_register") && ac >= 1)
    {
        Val vb, vw, vi;
        uint64_t val = (uint64_t)MU_MAX(as_num(sc, args[0], 0.0), 0.0);
        uint32_t bits = arg_get(arg_names, args, raw_args, ac, 1u, "bits",    &vb, NULL) ? (uint32_t)MU_MAX(as_num(sc, vb, 8.0), 1.0) : 8u;
        float    bw   = arg_get(arg_names, args, raw_args, ac, 2u, "w",       &vw, NULL) ? (float)as_num(sc, vw, 0.55) : 0.55f;
        bool     idx  = arg_get(arg_names, args, raw_args, ac, 3u, "indices", &vi, NULL) ? as_bool(sc, vi) : true;
        return val_mob(anim_cs_bit_register(s, val, bits, bw, idx, NULL, NULL));
    }
    if (str_eq(name, "cs_swap") && ac >= 2)
    {
        Val varc, vdur;
        float arc = arg_get(arg_names, args, raw_args, ac, 2u, "arc", &varc, NULL) ? (float)as_num(sc, varc, 0.6) : 0.6f;
        float dur = arg_get(arg_names, args, raw_args, ac, 3u, "dur", &vdur, NULL) ? (float)as_num(sc, vdur, 0.6) : 0.6f;
        anim_cs_swap_mobs(sc->tl, s, as_mob(args[0]), as_mob(args[1]), arc, dur);
        return val_nil();
    }
    if (str_eq(name, "phys_rk4") && ac >= 4 && args[0].kind == VAL_LIST && args[1].kind == VAL_FN)
    {
        uint32_t dim = MU_MIN(args[0].u.list.count, 64u);
        float y[64];
        ScriptOdeCtx ctx = { .sc = sc, .ode_fn = args[1].u.fn, .proj_fn = NULL };
        Val res = { .kind = VAL_LIST };
        for (uint32_t i = 0u; i < dim; ++i) y[i] = (float)as_num(sc, args[0].u.list.items[i], 0.0);
        anim_phys_step_rk4(y, dim, (float)as_num(sc, args[2], 0.0), (float)as_num(sc, args[3], 0.01), cb_ode_fn, &ctx);
        res.u.list.items = (Val *)sc_alloc(sc, MU_MAX(dim, 1u) * sizeof(Val));
        res.u.list.count = dim;
        for (uint32_t i = 0u; i < dim; ++i) res.u.list.items[i] = val_num((double)y[i]);
        return res;
    }
    if (str_eq(name, "phys_trajectory") && ac >= 2 && args[0].kind == VAL_LIST && args[1].kind == VAL_FN)
    {
        uint32_t dim = MU_MIN(args[0].u.list.count, 64u);
        float y0[64];
        float t0 = 0.0f, t1 = 10.0f;
        uint32_t steps = 200u;
        ScriptOdeCtx *ctx = (ScriptOdeCtx *)sc_alloc(sc, sizeof *ctx);
        Val vt, vt1, vst, vpr;
        ctx->sc = sc; ctx->ode_fn = args[1].u.fn; ctx->proj_fn = NULL;
        for (uint32_t i = 0u; i < dim; ++i) y0[i] = (float)as_num(sc, args[0].u.list.items[i], 0.0);
        if (arg_get(arg_names, args, raw_args, ac, 2u, "t", &vt, NULL))
        {
            if (vt.kind == VAL_RANGE) { t0 = (float)vt.u.range.lo; t1 = (float)vt.u.range.hi; }
            else
            {
                t0 = (float)as_num(sc, vt, 0.0);
                if (arg_get(arg_names, args, raw_args, ac, 3u, "t1", &vt1, NULL)) t1 = (float)as_num(sc, vt1, 10.0);
            }
        }
        if (arg_get(arg_names, args, raw_args, ac, 99u, "steps", &vst, NULL)) steps = (uint32_t)MU_MAX(as_num(sc, vst, 200.0), 4.0);
        if (arg_get(arg_names, args, raw_args, ac, 99u, "proj", &vpr, NULL) && vpr.kind == VAL_FN) ctx->proj_fn = vpr.u.fn;
        return val_mob(anim_phys_trajectory(s, y0, dim, t0, t1, steps, cb_ode_fn, cb_ode_proj_fn, ctx));
    }
    if (!sc->in_play && str_eq(name, "zoom_inset") && ac >= 1)
    {
        bool en = as_bool(sc, args[0]);
        Val vcam, vrect, vcol;
        AnimCamera cam = arg_get(arg_names, args, raw_args, ac, 1u, "cam", &vcam, NULL) ? bag_to_camera(sc, vcam, anim_scene_camera(s)) : anim_scene_camera(s);
        float rect[4] = { 0.65f, 0.05f, 0.96f, 0.36f };
        bool has_rect = false;
        AnimColor bcol = arg_get(arg_names, args, raw_args, ac, 3u, "border", &vcol, NULL) ? as_col(vcol, ANIM_GOLD) : ANIM_GOLD;
        if (arg_get(arg_names, args, raw_args, ac, 2u, "rect", &vrect, NULL) && vrect.kind == VAL_LIST && vrect.u.list.count >= 4u)
        {
            for (int k = 0; k < 4; ++k) rect[k] = (float)as_num(sc, vrect.u.list.items[k], rect[k]);
            has_rect = true;
        }
        anim_scene_set_zoom_inset(s, en, cam, has_rect ? rect : NULL, bcol);
        return val_nil();
    }

    /* ---- Method & Layout Modifiers (first arg = target) ---- */
    if (str_eq(name, "pos") && ac >= 2)
    {
        AnimVec3 p = (ac >= 3)
            ? ANIM_V3((float)as_num(sc, args[1], 0.0), (float)as_num(sc, args[2], 0.0), ac >= 4 ? (float)as_num(sc, args[3], 0.0) : 0.0f)
            : as_vec(sc, args[1], ANIM_ORIGIN);
        AnimId id = as_mob(args[0]);
        anim_move_to(s, id, p);
        if (ac == 2u && expr_is_reactive(sc, raw_args[1]))
            attach_react(sc, id, RB_MOVE_TO, raw_args[1], NULL, NULL, ANIM_ID_NIL, ANIM_ORIGIN, 0.0f, 0.0f);
        return args[0];
    }
    if (str_eq(name, "shift") && ac >= 2)
    {
        if (sc->in_play) { push_play_spec(sc, anim_anim_shift(as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN))); return args[0]; }
        anim_shift(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN));
        return args[0];
    }
    if (str_eq(name, "scale") && ac >= 2)
    {
        if (ac >= 3)
        {
            AnimVec3 sv = ANIM_V3((float)as_num(sc, args[1], 1.0), (float)as_num(sc, args[2], 1.0), ac >= 4 ? (float)as_num(sc, args[3], 1.0) : 1.0f);
            anim_scale_vec(s, as_mob(args[0]), sv);
            return args[0];
        }
        if (sc->in_play) { push_play_spec(sc, anim_anim_scale(as_mob(args[0]), (float)as_num(sc, args[1], 1.0))); return args[0]; }
        anim_scale(s, as_mob(args[0]), (float)as_num(sc, args[1], 1.0));
        return args[0];
    }
    if (str_eq(name, "width") && ac == 1)  return val_num((double)anim_width(s, as_mob(args[0])));
    if (str_eq(name, "width") && ac >= 2)  { anim_scale_to_width(s, as_mob(args[0]), (float)as_num(sc, args[1], 1.0)); return args[0]; }
    if (str_eq(name, "height") && ac == 1) return val_num((double)anim_height(s, as_mob(args[0])));
    if (str_eq(name, "height") && ac >= 2) { anim_scale_to_height(s, as_mob(args[0]), (float)as_num(sc, args[1], 1.0)); return args[0]; }
    if (str_eq(name, "rot") && ac >= 2)
    {
        AnimVec3 ax = ac >= 3 ? as_vec(sc, args[2], ANIM_OUT) : ANIM_OUT;
        if (sc->in_play) { push_play_spec(sc, anim_anim_rotate(as_mob(args[0]), (float)as_num(sc, args[1], 0.0), ax)); return args[0]; }
        anim_rotate(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0), ax);
        return args[0];
    }
    if (str_eq(name, "next_to") && ac >= 2)
    {
        AnimId id  = as_mob(args[0]), ref = as_mob(args[1]);
        AnimVec3 dir = ac >= 3 ? as_vec(sc, args[2], ANIM_RIGHT) : ANIM_RIGHT;
        float buff   = ac >= 4 ? (float)as_num(sc, args[3], 0.25) : 0.25f;
        anim_next_to(s, id, ref, dir, buff);
        attach_react(sc, id, RB_NEXT_TO, raw_args[1], NULL, NULL, ref, dir, buff, 0.0f);
        return args[0];
    }
    if (str_eq(name, "align_to") && ac >= 3) { anim_align_to(s, as_mob(args[0]), as_mob(args[1]), as_vec(sc, args[2], ANIM_LEFT)); return args[0]; }
    if (str_eq(name, "to_edge") && ac >= 2)  { anim_to_edge(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_UP), ac >= 3 ? (float)as_num(sc, args[2], 0.4) : 0.4f); return args[0]; }
    if (str_eq(name, "to_corner") && ac >= 2){ anim_to_corner(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_UL), ac >= 3 ? (float)as_num(sc, args[2], 0.4) : 0.4f); return args[0]; }
    if (str_eq(name, "fill") && ac >= 2)
    {
        AnimColor c = as_col(args[1], ANIM_WHITE);
        if (ac >= 3) c.a *= (float)as_num(sc, args[2], 1.0);
        anim_set_fill(s, as_mob(args[0]), c, true);
        return args[0];
    }
    if (str_eq(name, "stroke") && ac >= 2)
    {
        if (args[1].kind == VAL_NUM)
            set_stroke_width_rec(s, as_mob(args[0]), (float)args[1].u.num);
        else
        {
            AnimColor c = as_col(args[1], ANIM_WHITE);
            float w = ac >= 3 ? (float)as_num(sc, args[2], 3.0) : 3.0f;
            if (ac >= 4) c.a *= (float)as_num(sc, args[3], 1.0);
            anim_set_stroke(s, as_mob(args[0]), c, w, true);
        }
        return args[0];
    }
    if (str_eq(name, "color") && ac >= 2)    { anim_set_color(s, as_mob(args[0]), as_col(args[1], ANIM_WHITE), true); return args[0]; }
    if (str_eq(name, "opacity") && ac >= 2)  { anim_set_opacity(s, as_mob(args[0]), (float)as_num(sc, args[1], 1.0), true); return args[0]; }
    if (str_eq(name, "glow") && ac >= 2)     { anim_set_glow(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.5), true); return args[0]; }
    if (str_eq(name, "sheen") && ac >= 2)    { anim_set_sheen(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.4), ac >= 3 ? (float)as_num(sc, args[2], 0.5) : 0.5f, true); return args[0]; }
    if (str_eq(name, "sheen_color") && ac >= 4)
        { anim_set_sheen_color(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.5), (float)as_num(sc, args[2], 0.5), as_col(args[3], ANIM_WHITE), true); return args[0]; }
    if ((str_eq(name, "bg_stroke") || str_eq(name, "background_stroke")) && ac >= 2)
        { anim_set_background_stroke(s, as_mob(args[0]), as_col(args[1], ANIM_BLACK), ac >= 3 ? (float)as_num(sc, args[2], 4.0) : 4.0f, true); return args[0]; }
    if (str_eq(name, "depth_test") && ac >= 1)
        { anim_set_depth_test(s, as_mob(args[0]), ac >= 2 ? as_bool(sc, args[1]) : true, true); return args[0]; }
    if (str_eq(name, "fixed_in_frame") && ac >= 1)
        { anim_set_fixed_in_frame(s, as_mob(args[0]), ac >= 2 ? as_bool(sc, args[1]) : true, true); return args[0]; }
    if (str_eq(name, "shadow") && ac >= 2)   { anim_set_shadow(s, as_mob(args[0]), (float)as_num(sc, args[1], 8.0), true); return args[0]; }
    if (str_eq(name, "dash") && ac >= 2)     { anim_set_dash(s, as_mob(args[0]), (float)as_num(sc, args[1], 10.0), ac >= 3 ? (float)as_num(sc, args[2], 8.0) : 8.0f, ac >= 4 ? (float)as_num(sc, args[3], 0.0) : 0.0f, true); return args[0]; }
    if (str_eq(name, "z") && ac >= 2)        { anim_set_z_index(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0), true); return args[0]; }
    if (str_eq(name, "billboard") && ac >= 1){ anim_set_billboard(s, as_mob(args[0]), ac >= 2 ? as_bool(sc, args[1]) : true, ac >= 3 ? as_bool(sc, args[2]) : false); return args[0]; }
    if (str_eq(name, "wireframe") && ac >= 2){ anim_surface_set_wireframe(s, as_mob(args[0]), (float)as_num(sc, args[1], 10.0), ac >= 3 ? (float)as_num(sc, args[2], 0.4) : 0.4f, true); return args[0]; }
    if (str_eq(name, "roughness") && ac >= 2){ anim_surface_set_roughness(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.25), true); return args[0]; }
    if (str_eq(name, "checkerboard") && ac >= 3)
        { anim_surface_set_checkerboard(s, as_mob(args[0]), as_col(args[1], ANIM_BLUE_D), as_col(args[2], ANIM_BLUE_E), ac >= 4 ? (uint32_t)as_num(sc, args[3], 8.0) : 8u, ac >= 5 ? (uint32_t)as_num(sc, args[4], 8.0) : 8u, true); return args[0]; }
    if (str_eq(name, "texture") && ac >= 2)  { anim_surface_set_texture(s, as_mob(args[0]), as_str(args[1]) ? as_str(args[1]) : "", ac >= 3 ? as_bool(sc, args[2]) : true, true); return args[0]; }
    if (str_eq(name, "lit") && ac >= 2)      { anim_image_set_lit(s, as_mob(args[0]), as_bool(sc, args[1]), true); return args[0]; }
    if (str_eq(name, "tag") && ac >= 2)      { anim_set_tag(s, as_mob(args[0]), as_str(args[1]) ? as_str(args[1]) : ""); return args[0]; }
    if (str_eq(name, "gradient_stops") && ac >= 2 && args[1].kind == VAL_LIST && args[1].u.list.count >= 2u)
    {
        AnimColor cols[8];
        uint32_t  cnt = MU_MIN(args[1].u.list.count, 8u);
        Val vp0, vp1, vrad, vr1;
        bool radial = arg_get(arg_names, args, raw_args, ac, 99u, "radial", &vrad, NULL) && as_bool(sc, vrad);
        AnimVec3 p0 = arg_get(arg_names, args, raw_args, ac, 2u, radial ? "center" : "p0", &vp0, NULL) ? as_vec(sc, vp0, ANIM_V3(-1.0f, 0.0f, 0.0f)) : ANIM_V3(-1.0f, 0.0f, 0.0f);
        AnimVec3 p1 = arg_get(arg_names, args, raw_args, ac, 3u, "p1", &vp1, NULL) ? as_vec(sc, vp1, ANIM_V3(1.0f, 0.0f, 0.0f)) : ANIM_V3(1.0f, 0.0f, 0.0f);
        float rad = arg_get(arg_names, args, raw_args, ac, 4u, "r", &vr1, NULL) ? (float)as_num(sc, vr1, 1.5) : 1.5f;
        for (uint32_t k = 0u; k < cnt; ++k) cols[k] = as_col(args[1].u.list.items[k], ANIM_WHITE);
        anim_set_fill_gradient_stops(s, as_mob(args[0]), cols, cnt, p0, p1, radial, rad, true);
        return args[0];
    }
    if (str_eq(name, "gradient_fill") && ac >= 3)
    {
        Val vp0, vp1, vrad, vr1;
        bool radial = arg_get(arg_names, args, raw_args, ac, 99u, "radial", &vrad, NULL) && as_bool(sc, vrad);
        AnimColor c0 = as_col(args[1], ANIM_BLUE), c1 = as_col(args[2], ANIM_GOLD);
        if (radial)
        {
            AnimVec3 ctr = arg_get(arg_names, args, raw_args, ac, 3u, "center", &vp0, NULL) ? as_vec(sc, vp0, anim_center(s, as_mob(args[0]))) : anim_center(s, as_mob(args[0]));
            float r1 = arg_get(arg_names, args, raw_args, ac, 4u, "r1", &vr1, NULL) ? (float)as_num(sc, vr1, 1.5)
                     : (arg_get(arg_names, args, raw_args, ac, 99u, "r", &vr1, NULL) ? (float)as_num(sc, vr1, 1.5) : 1.5f);
            anim_set_fill_gradient_radial(s, as_mob(args[0]), c0, c1, ctr, r1, true);
        }
        else
        {
            AnimVec3 p0 = arg_get(arg_names, args, raw_args, ac, 3u, "p0", &vp0, NULL) ? as_vec(sc, vp0, ANIM_V3(-1.0f, 0.0f, 0.0f)) : ANIM_V3(-1.0f, 0.0f, 0.0f);
            AnimVec3 p1 = arg_get(arg_names, args, raw_args, ac, 4u, "p1", &vp1, NULL) ? as_vec(sc, vp1, ANIM_V3( 1.0f, 0.0f, 0.0f)) : ANIM_V3( 1.0f, 0.0f, 0.0f);
            anim_set_fill_gradient_linear(s, as_mob(args[0]), c0, c1, p0, p1, true);
        }
        return args[0];
    }
    if (str_eq(name, "clear_gradient") && ac >= 1)
        { anim_clear_fill_gradient(s, as_mob(args[0]), true); return args[0]; }
    if (str_eq(name, "stroke_gradient") && ac >= 3)
        { anim_vmob_set_stroke_gradient(s, as_mob(args[0]), as_col(args[1], ANIM_BLUE), as_col(args[2], ANIM_GOLD)); return args[0]; }
    if (str_eq(name, "trim") && ac >= 3)
        { anim_set_trim(s, as_mob(args[0]), (float)as_num(sc, args[1], 0.0), (float)as_num(sc, args[2], 1.0), true); return args[0]; }
    if (str_eq(name, "clip_rect") && ac >= 3)
        { anim_set_clip_rect(s, as_mob(args[0]), as_vec(sc, args[1], ANIM_V3(-7,-4,0)), as_vec(sc, args[2], ANIM_V3(7,4,0)), true); return args[0]; }
    if (str_eq(name, "clear_clip") && ac >= 1)
        { anim_clear_clip(s, as_mob(args[0]), true); return args[0]; }
    if (str_eq(name, "fill_rule") && ac >= 2)
    {
        AnimFillRule fr = (args[1].kind == VAL_STR && str_eq(args[1].u.str, "nonzero")) ? ANIM_FILL_NONZERO : ANIM_FILL_EVEN_ODD;
        anim_set_fill_rule(s, as_mob(args[0]), fr, true);
        return args[0];
    }
    if (str_eq(name, "cap_join") && ac >= 3)
    {
        AnimCapStyle  cap  = ANIM_CAP_ROUND;
        AnimJoinStyle join = ANIM_JOIN_ROUND;
        if (args[1].kind == VAL_STR)
        {
            if (str_eq(args[1].u.str, "butt")) cap = ANIM_CAP_BUTT;
            else if (str_eq(args[1].u.str, "square")) cap = ANIM_CAP_SQUARE;
        }
        if (args[2].kind == VAL_STR)
        {
            if (str_eq(args[2].u.str, "miter")) join = ANIM_JOIN_MITER;
            else if (str_eq(args[2].u.str, "bevel")) join = ANIM_JOIN_BEVEL;
        }
        anim_set_cap_join(s, as_mob(args[0]), cap, join, true);
        return args[0];
    }

    /* ---- Timeline Animations (when inside `play` / `lagged` / `sequence` / `at`) ---- */
    if (sc->in_play)
    {
        if (str_eq(name, "create"))   { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_create(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "uncreate")) { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_uncreate(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "write"))    { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_write(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "unwrite"))  { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_unwrite(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "type_with_cursor") && ac >= 1) { push_play_spec_ex(sc, anim_anim_type_with_cursor(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? as_col(args[1], ANIM_WHITE) : ANIM_WHITE), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "show_subsets") && ac >= 1)     { push_play_spec_ex(sc, anim_anim_show_increasing_subsets(as_mob(args[0])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "show_one_by_one") && ac >= 1)  { push_play_spec_ex(sc, anim_anim_show_submobjects_one_by_one(as_mob(args[0])), arg_names, args, ac); return val_nil(); }
        if ((str_eq(name, "add_letters") || str_eq(name, "add_text_letter_by_letter")) && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_add_text_letter_by_letter(as_mob(args[0])), arg_names, args, ac); return val_nil(); }
        if ((str_eq(name, "remove_letters") || str_eq(name, "remove_text_letter_by_letter")) && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_remove_text_letter_by_letter(as_mob(args[0])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "transform_matching_shapes") && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_transform_matching_shapes(as_mob(args[0]), as_mob(args[1])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "draw_border_then_fill")) { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_draw_border_then_fill(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "fade_in"))
        {
            if (ac >= 1 && (args[0].kind == VAL_MOB || args[0].kind == VAL_LIST))
            {
                bool has_shift = (ac >= 2 && !arg_names[1] && args[1].kind == VAL_VEC);
                if (has_shift)
                    push_play_spec_ex(sc, anim_anim_fade_in(as_mob(args[0]), args[1].u.vec, (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 1.0) : 1.0f), arg_names, args, ac);
                else
                    for (uint32_t i = 0u; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_fade_in(as_mob(args[i]), ANIM_ORIGIN, 1.0f), arg_names, args, ac);
            }
            return val_nil();
        }
        if (str_eq(name, "fade_out"))
        {
            if (ac >= 2 && !arg_names[1] && args[1].kind == VAL_VEC)
                push_play_spec_ex(sc, anim_anim_fade_out(as_mob(args[0]), args[1].u.vec, (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 1.0) : 1.0f), arg_names, args, ac);
            else
                for (uint32_t i = 0u; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_fade_out(as_mob(args[i]), ANIM_ORIGIN, 1.0f), arg_names, args, ac);
            return val_nil();
        }
        if (str_eq(name, "grow"))       { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_grow_from_center(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "grow_arrow")) { for (uint32_t i = 0; i < ac; ++i) if (!arg_names[i]) push_play_spec_ex(sc, anim_anim_grow_arrow(as_mob(args[i])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "grow_from_edge") && ac >= 2) { push_play_spec_ex(sc, anim_anim_grow_from_edge(as_mob(args[0]), as_vec(sc, args[1], ANIM_LEFT)), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "spin_in") && ac >= 1)    { push_play_spec_ex(sc, anim_anim_spin_in_from_nothing(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? (float)as_num(sc, args[1], ANIM_TAU) : ANIM_TAU), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "shrink") && ac >= 1)     { push_play_spec_ex(sc, anim_anim_shrink_to_center(as_mob(args[0])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "indicate") && ac >= 1)   { push_play_spec_ex(sc, anim_anim_indicate(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? (float)as_num(sc, args[1], 1.2) : 1.2f, (ac >= 3 && !arg_names[2]) ? as_col(args[2], ANIM_YELLOW) : ANIM_YELLOW), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "circumscribe") && ac >= 1) { push_play_spec_ex(sc, anim_anim_circumscribe(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? as_col(args[1], ANIM_YELLOW) : ANIM_YELLOW, (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 0.14) : 0.14f), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "passing_flash") && ac >= 1) { push_play_spec_ex(sc, anim_anim_show_passing_flash(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? (float)as_num(sc, args[1], 0.25) : 0.25f), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "wiggle") && ac >= 1)     { push_play_spec_ex(sc, anim_anim_wiggle(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? (float)as_num(sc, args[1], 1.12) : 1.12f, (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 0.08) : 0.08f), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "transform") && ac >= 2)  { push_play_spec_ex(sc, anim_anim_transform(as_mob(args[0]), as_mob(args[1])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "replace") && ac >= 2)    { push_play_spec_ex(sc, anim_anim_replacement_transform(as_mob(args[0]), as_mob(args[1])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "transform_matching_tex") && ac >= 2) { push_play_spec_ex(sc, anim_anim_transform_matching_tex(as_mob(args[0]), as_mob(args[1])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "move_arc") && ac >= 2)   { push_play_spec_ex(sc, anim_anim_move_arc(as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN), (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 1.0) : 1.0f), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "follow_path") && ac >= 2){ push_play_spec_ex(sc, anim_anim_follow_path(as_mob(args[0]), as_mob(args[1]), (ac >= 3 && !arg_names[2]) ? as_bool(sc, args[2]) : false), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "flash") && ac >= 1)      { push_play_spec(sc, anim_anim_flash(as_vec(sc, args[0], ANIM_ORIGIN), ac >= 2 ? as_col(args[1], ANIM_YELLOW) : ANIM_YELLOW, ac >= 3 ? (float)as_num(sc, args[2], 0.25) : 0.25f, 12u, ac >= 4 ? (float)as_num(sc, args[3], 0.55) : 0.55f)); return val_nil(); }
        if (str_eq(name, "focus_on") && ac >= 1)   { push_play_spec(sc, anim_anim_focus_on(as_vec(sc, args[0], ANIM_ORIGIN), ac >= 2 ? as_col(args[1], ANIM_YELLOW) : ANIM_YELLOW, ac >= 3 ? (float)as_num(sc, args[2], 0.35) : 0.35f)); return val_nil(); }
        if (str_eq(name, "broadcast") && ac >= 1)  { push_play_spec(sc, anim_anim_broadcast(as_vec(sc, args[0], ANIM_ORIGIN), ac >= 2 ? as_col(args[1], ANIM_BLUE) : ANIM_BLUE, 3u, ac >= 3 ? (float)as_num(sc, args[2], 2.5) : 2.5f)); return val_nil(); }
        if (str_eq(name, "apply_wave") && ac >= 1) { push_play_spec(sc, anim_anim_apply_wave(as_mob(args[0]), ac >= 2 ? as_vec(sc, args[1], ANIM_UP) : ANIM_UP, ac >= 3 ? (float)as_num(sc, args[2], 0.25) : 0.25f)); return val_nil(); }
        if (str_eq(name, "apply_warp") && ac >= 2 && args[1].kind == VAL_FN)
            { push_play_spec(sc, anim_anim_apply_warp(as_mob(args[0]), cb_warp_fn, make_cb_ctx(sc, args[1].u.fn))); return val_nil(); }
        if (str_eq(name, "apply_matrix") && ac >= 2 && args[1].kind == VAL_LIST && args[1].u.list.count >= 16u)
        {
            float m[16];
            for (uint32_t i = 0u; i < 16u; ++i) m[i] = (float)as_num(sc, args[1].u.list.items[i], (i % 5u == 0u) ? 1.0 : 0.0);
            push_play_spec(sc, anim_anim_apply_matrix(as_mob(args[0]), m));
            return val_nil();
        }
        if (str_eq(name, "apply_matrix3") && ac >= 2 && args[1].kind == VAL_LIST && args[1].u.list.count >= 9u)
        {
            float m[9];
            for (uint32_t i = 0u; i < 9u; ++i) m[i] = (float)as_num(sc, args[1].u.list.items[i], (i % 4u == 0u) ? 1.0 : 0.0);
            push_play_spec(sc, anim_anim_apply_matrix3(as_mob(args[0]), m));
            return val_nil();
        }
        if (str_eq(name, "homotopy") && ac >= 2 && args[1].kind == VAL_FN)
            { push_play_spec(sc, anim_anim_homotopy(as_mob(args[0]), cb_homotopy_fn, make_cb_ctx(sc, args[1].u.fn))); return val_nil(); }
        if (str_eq(name, "apply_complex") && ac >= 2 && args[1].kind == VAL_FN)
            { push_play_spec_ex(sc, anim_anim_apply_complex_fn(as_mob(args[0]), cb_complex_fn, make_cb_ctx(sc, args[1].u.fn)), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "restore") && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_restore(as_mob(args[0])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "rotating") && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_rotating(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? (float)as_num(sc, args[1], ANIM_TAU) : ANIM_TAU, (ac >= 3 && !arg_names[2]) ? as_vec(sc, args[2], ANIM_OUT) : ANIM_OUT, (ac >= 4 && !arg_names[3]) ? as_vec(sc, args[3], anim_center(s, as_mob(args[0]))) : anim_center(s, as_mob(args[0]))), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "swap") && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_swap(as_mob(args[0]), as_mob(args[1]), (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 0.9) : 0.9f), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "fade_transform") && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_fade_transform(as_mob(args[0]), as_mob(args[1])), arg_names, args, ac); return val_nil(); }
        if ((str_eq(name, "ccw_transform") || str_eq(name, "counterclockwise_transform")) && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_counterclockwise_transform(as_mob(args[0]), as_mob(args[1]), (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], ANIM_PI) : ANIM_PI), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "phase_flow") && ac >= 2 && args[1].kind == VAL_FN)
            { push_play_spec_ex(sc, anim_anim_phase_flow(as_mob(args[0]), cb_field_fn, (ac >= 3 && !arg_names[2]) ? (float)as_num(sc, args[2], 1.0) : 1.0f, make_cb_ctx(sc, args[1].u.fn)), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "blink") && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_blink(as_mob(args[0]), (ac >= 2 && !arg_names[1]) ? (uint32_t)as_num(sc, args[1], 2.0) : 2u), arg_names, args, ac); return val_nil(); }

        /* 3D Camera & 3D Transitions */
        if (str_eq(name, "camera_to") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_to(bag_to_camera(sc, args[0], anim_scene_camera(s)))); return val_nil(); }
        if (str_eq(name, "camera_fly_to") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_fly_to(bag_to_camera(sc, args[0], anim_scene_camera(s)), ac >= 2 ? (float)as_num(sc, args[1], 1.0) : 1.0f)); return val_nil(); }
        if (str_eq(name, "camera_orbit") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_orbit((float)as_num(sc, args[0], 0.0), ac >= 2 ? (float)as_num(sc, args[1], 0.0) : 0.0f, ac >= 3 ? (float)as_num(sc, args[2], 1.0) : 1.0f)); return val_nil(); }
        if (str_eq(name, "camera_orbit_axis") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_orbit_axis((float)as_num(sc, args[0], ANIM_PI), ac >= 2 ? as_vec(sc, args[1], ANIM_UP) : ANIM_UP, ac >= 3 ? (float)as_num(sc, args[2], 1.0) : 1.0f)); return val_nil(); }
        if (str_eq(name, "dolly_zoom") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_dolly_zoom((float)as_num(sc, args[0], 65.0))); return val_nil(); }
        if (str_eq(name, "camera_track") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_track(as_mob(args[0]), ac >= 2 ? as_vec(sc, args[1], ANIM_ORIGIN) : ANIM_ORIGIN, ac >= 3 ? as_bool(sc, args[2]) : false)); return val_nil(); }
        if (str_eq(name, "camera_shake") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_shake((float)as_num(sc, args[0], 0.12), ac >= 2 ? (float)as_num(sc, args[1], 8.0) : 8.0f)); return val_nil(); }
        if (str_eq(name, "camera_path") && ac >= 1)
            { push_play_spec(sc, anim_anim_camera_path(as_mob(args[0]), ac >= 2 ? as_vec(sc, args[1], ANIM_ORIGIN) : ANIM_ORIGIN, ac >= 3 ? as_bool(sc, args[2]) : false)); return val_nil(); }
        if (str_eq(name, "camera_frame") && ac >= 1)
        {
            float pad = ac >= 2 ? (float)as_num(sc, args[1], 0.6) : 0.6f;
            float asp = ac >= 3 ? (float)as_num(sc, args[2], 16.0 / 9.0) : (16.0f / 9.0f);
            push_play_spec_ex(sc, anim_anim_camera_frame(s, as_mob(args[0]), pad, asp), arg_names, args, ac);
            return val_nil();
        }
        if (str_eq(name, "flip_3d") && ac >= 1)
        {
            AnimId tid = as_mob(args[0]);
            ensure_in_scene(sc, tid);
            push_play_spec(sc, anim_anim_flip_3d(tid, ac >= 2 ? as_vec(sc, args[1], ANIM_UP) : ANIM_UP, ac >= 3 ? (float)as_num(sc, args[2], 1.0) : 1.0f));
            return val_nil();
        }
        if (str_eq(name, "flip_replace_3d") && ac >= 2)
            { push_play_spec(sc, anim_anim_flip_replace_3d(as_mob(args[0]), as_mob(args[1]), ac >= 3 ? as_vec(sc, args[2], ANIM_UP) : ANIM_UP)); return val_nil(); }
        if (str_eq(name, "tumble_in") && ac >= 1)
            { push_play_spec(sc, anim_anim_tumble_in(as_mob(args[0]), ac >= 2 ? as_vec(sc, args[1], ANIM_V3(-2, 2, -3)) : ANIM_V3(-2, 2, -3), ac >= 3 ? (float)as_num(sc, args[2], ANIM_TAU) : ANIM_TAU, ac >= 4 ? as_vec(sc, args[3], ANIM_UR) : ANIM_UR)); return val_nil(); }
        if (str_eq(name, "tumble_out") && ac >= 1)
            { push_play_spec(sc, anim_anim_tumble_out(as_mob(args[0]), ac >= 2 ? as_vec(sc, args[1], ANIM_V3(2, -2, -3)) : ANIM_V3(2, -2, -3), ac >= 3 ? (float)as_num(sc, args[2], ANIM_PI) : ANIM_PI, ac >= 4 ? as_vec(sc, args[3], ANIM_UP) : ANIM_UP)); return val_nil(); }
        if (str_eq(name, "explode_3d") && ac >= 1)
            { push_play_spec(sc, anim_anim_explode_3d(as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 1.0) : 1.0f, ac >= 3 ? (float)as_num(sc, args[2], ANIM_PI) : ANIM_PI)); return val_nil(); }
        if (str_eq(name, "swirl_3d") && ac >= 1)
            { push_play_spec(sc, anim_anim_swirl_3d(as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 1.5) : 1.5f, ac >= 3 ? as_vec(sc, args[2], ANIM_UP) : ANIM_UP)); return val_nil(); }
        if (str_eq(name, "rotate_around") && ac >= 3)
            { push_play_spec(sc, anim_anim_rotate_around(as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN), (float)as_num(sc, args[2], ANIM_TAU), ac >= 4 ? as_vec(sc, args[3], ANIM_UP) : ANIM_UP, ac >= 5 ? as_bool(sc, args[4]) : true)); return val_nil(); }
        if (str_eq(name, "pulse_glow") && ac >= 1)
            { push_play_spec(sc, anim_anim_pulse_glow(as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], 0.8) : 0.8f, ac >= 3 ? (float)as_num(sc, args[2], 1.15) : 1.15f)); return val_nil(); }
        if (str_eq(name, "spring_move") && ac >= 2)
            { push_play_spec(sc, anim_anim_spring_move(as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN), ac >= 3 ? (float)as_num(sc, args[2], 0.5) : 0.5f, ac >= 4 ? (float)as_num(sc, args[3], 14.0) : 14.0f)); return val_nil(); }
        if (str_eq(name, "viewport_to") && ac >= 3)
        {
            uint32_t vp_i = (uint32_t)as_num(sc, args[0], 0.0);
            bool act = as_bool(sc, args[1]);
            AnimCamera cam = bag_to_camera(sc, args[2], anim_scene_camera(s));
            float rect[4] = { 0.64f, 0.04f, 0.96f, 0.36f };
            bool has_rect = false;
            if (ac >= 4 && args[3].kind == VAL_LIST && args[3].u.list.count >= 4u)
            {
                for (int k = 0; k < 4; ++k) rect[k] = (float)as_num(sc, args[3].u.list.items[k], rect[k]);
                has_rect = true;
            }
            push_play_spec(sc, anim_anim_viewport_to(vp_i, act, cam, has_rect ? rect : NULL));
            return val_nil();
        }
        if (str_eq(name, "set_color") && ac >= 2)
            { push_play_spec(sc, anim_anim_set_color(as_mob(args[0]), as_col(args[1], ANIM_WHITE))); return val_nil(); }
        if (str_eq(name, "set_opacity") && ac >= 2)
            { push_play_spec(sc, anim_anim_set_opacity(as_mob(args[0]), (float)as_num(sc, args[1], 1.0))); return val_nil(); }
        if (str_eq(name, "dash_flow") && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_dash_flow(as_mob(args[0]), (float)as_num(sc, args[1], 24.0)), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "grow_from_point") && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_grow_from_point(as_mob(args[0]), as_vec(sc, args[1], ANIM_ORIGIN)), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "spiral_in") && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_spiral_in(as_mob(args[0]), ac >= 2 ? (float)as_num(sc, args[1], ANIM_TAU) : ANIM_TAU), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "transform_copy") && ac >= 2)
            { push_play_spec_ex(sc, anim_anim_transform_from_copy(as_mob(args[0]), as_mob(args[1])), arg_names, args, ac); return val_nil(); }
        if (str_eq(name, "zoom_inset") && ac >= 1)
        {
            bool en = as_bool(sc, args[0]);
            Val vcam, vrect;
            AnimCamera cam = arg_get(arg_names, args, raw_args, ac, 1u, "cam", &vcam, NULL) ? bag_to_camera(sc, vcam, anim_scene_camera(s)) : anim_scene_camera(s);
            float rect[4] = { 0.65f, 0.05f, 0.96f, 0.36f };
            bool has_rect = false;
            if (arg_get(arg_names, args, raw_args, ac, 2u, "rect", &vrect, NULL) && vrect.kind == VAL_LIST && vrect.u.list.count >= 4u)
            {
                for (int k = 0; k < 4; ++k) rect[k] = (float)as_num(sc, vrect.u.list.items[k], rect[k]);
                has_rect = true;
            }
            push_play_spec_ex(sc, anim_anim_zoom_inset_to(en, cam, has_rect ? rect : NULL), arg_names, args, ac);
            return val_nil();
        }
        if (str_eq(name, "frame") && ac >= 1)
            { push_play_spec_ex(sc, anim_anim_circumscribe(as_mob(args[0]), ac >= 2 ? as_col(args[1], ANIM_YELLOW) : ANIM_YELLOW, ac >= 3 ? (float)as_num(sc, args[2], 0.14) : 0.14f), arg_names, args, ac); return val_nil(); }
    }

    sc_error(sc, line, "unknown or misplaced function '%s'", name);
    return val_nil();
}

static Val eval_expr(AnimScript *sc, const Expr *e)
{
    if (!e || sc->has_error)
        return val_nil();

    switch (e->kind)
    {
        case EX_NUM:  return val_num(e->u.num.val);
        case EX_BOOL: return val_bool(e->u.boolean.val);
        case EX_STR:  return val_str(e->u.str.str);
        case EX_TEX:  return val_mob(anim_tex(sc->scene, ANIM_TEX_AUTO, e->u.str.str, 0.7f));
        case EX_HEX:
            return val_col(e->u.hex.has_alpha
                ? anim_hex_a(e->u.hex.rgb, (float)e->u.hex.alpha / 255.0f)
                : anim_hex(e->u.hex.rgb));
        case EX_IDENT:
        {
            VarSlot *vs = env_find(sc->cur_env, e->u.ident.name);
            if (vs)
            {
                if (vs->is_reactive && vs->react_expr)
                    return eval_expr(sc, vs->react_expr);
                return vs->val;
            }
            sc_error(sc, e->line, "undefined identifier '%s'", e->u.ident.name);
            return val_nil();
        }
        case EX_LIST:
        {
            Val      v    = { .kind = VAL_LIST };
            uint32_t n    = e->u.list.count;
            v.u.list.items = (Val *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof(Val));
            v.u.list.count = n;
            for (uint32_t i = 0u; i < n; ++i)
                v.u.list.items[i] = eval_expr(sc, e->u.list.items[i]);
            return v;
        }
        case EX_VEC:
        {
            Val      v       = { .kind = VAL_LIST };
            uint32_t n       = e->u.list.count;
            bool     all_num = (n == 2u || n == 3u);
            v.u.list.items = (Val *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof(Val));
            v.u.list.count = n;
            for (uint32_t i = 0u; i < n; ++i)
            {
                v.u.list.items[i] = eval_expr(sc, e->u.list.items[i]);
                if (v.u.list.items[i].kind != VAL_NUM && v.u.list.items[i].kind != VAL_TRACKER)
                    all_num = false;
            }
            if (all_num)
            {
                float x = (float)as_num(sc, v.u.list.items[0], 0.0);
                float y = (float)as_num(sc, v.u.list.items[1], 0.0);
                float z = (n == 3u) ? (float)as_num(sc, v.u.list.items[2], 0.0) : 0.0f;
                return val_vec(ANIM_V3(x, y, z));
            }
            return v;
        }
        case EX_RANGE:
        {
            Val v = { .kind = VAL_RANGE };
            v.u.range.lo       = as_num(sc, eval_expr(sc, e->u.range.lo), 0.0);
            v.u.range.hi       = as_num(sc, eval_expr(sc, e->u.range.hi), 1.0);
            v.u.range.has_step = (e->u.range.step != NULL);
            v.u.range.step     = e->u.range.step ? as_num(sc, eval_expr(sc, e->u.range.step), 1.0) : 1.0;
            return v;
        }
        case EX_BAG:
        {
            Val      v = { .kind = VAL_BAG };
            uint32_t n = e->u.bag.count;
            v.u.bag.keys  = (const char **)sc_alloc(sc, MU_MAX(n, 1u) * sizeof(const char *));
            v.u.bag.vals  = (Val *)sc_alloc(sc, MU_MAX(n, 1u) * sizeof(Val));
            v.u.bag.count = n;
            for (uint32_t i = 0u; i < n; ++i)
            {
                v.u.bag.keys[i] = e->u.bag.fields[i].key;
                v.u.bag.vals[i] = eval_expr(sc, e->u.bag.fields[i].val);
            }
            return v;
        }
        case EX_PAIR:
        {
            Val v = { .kind = VAL_PAIR_COL };
            v.u.pair_col.c0 = as_col(eval_expr(sc, e->u.binary.lhs), ANIM_BLACK);
            v.u.pair_col.c1 = as_col(eval_expr(sc, e->u.binary.rhs), ANIM_BLACK);
            return v;
        }
        case EX_LAMBDA:
        {
            ScriptFn *fn = (ScriptFn *)sc_alloc(sc, sizeof *fn);
            Val       v  = { .kind = VAL_FN };
            fn->is_lambda   = true;
            fn->param_count = e->u.lambda.param_count;
            for (uint32_t i = 0u; i < fn->param_count; ++i)
                fn->params[i] = e->u.lambda.params[i];
            fn->lambda_body = e->u.lambda.body;
            v.u.fn = fn;
            return v;
        }
        case EX_UNARY:
        {
            Val r = eval_expr(sc, e->u.unary.rhs);
            if (e->u.unary.op == TOK_MINUS)
            {
                if (r.kind == VAL_VEC)
                    return val_vec(ANIM_V3(-r.u.vec.x, -r.u.vec.y, -r.u.vec.z));
                return val_num(-as_num(sc, r, 0.0));
            }
            if (e->u.unary.op == TOK_BANG)
                return val_bool(!as_bool(sc, r));
            return r;
        }
        case EX_BINARY:
        {
            Val a = eval_expr(sc, e->u.binary.lhs);
            Val b = eval_expr(sc, e->u.binary.rhs);
            TokKind op = e->u.binary.op;

            /* Mobject CSG & grouping operators */
            if (a.kind == VAL_MOB && b.kind == VAL_MOB)
            {
                if (op == TOK_PLUS)  { AnimId arr[2] = { a.u.mob, b.u.mob }; return val_mob(anim_group(sc->scene, arr, 2u)); }
                if (op == TOK_BAR)   return val_mob(anim_bool_op(sc->scene, a.u.mob, b.u.mob, ANIM_BOOL_UNION));
                if (op == TOK_AMP)   return val_mob(anim_bool_op(sc->scene, a.u.mob, b.u.mob, ANIM_BOOL_INTERSECT));
                if (op == TOK_MINUS) return val_mob(anim_bool_op(sc->scene, a.u.mob, b.u.mob, ANIM_BOOL_DIFFERENCE));
                if (op == TOK_CARET) return val_mob(anim_bool_op(sc->scene, a.u.mob, b.u.mob, ANIM_BOOL_XOR));
            }

            if (a.kind == VAL_VEC || b.kind == VAL_VEC)
            {
                if (op == TOK_PLUS && a.kind == VAL_VEC && b.kind == VAL_VEC)
                    return val_vec(ANIM_V3(a.u.vec.x + b.u.vec.x, a.u.vec.y + b.u.vec.y, a.u.vec.z + b.u.vec.z));
                if (op == TOK_MINUS && a.kind == VAL_VEC && b.kind == VAL_VEC)
                    return val_vec(ANIM_V3(a.u.vec.x - b.u.vec.x, a.u.vec.y - b.u.vec.y, a.u.vec.z - b.u.vec.z));
                if (op == TOK_STAR && a.kind == VAL_VEC && b.kind == VAL_VEC)
                    return val_vec(ANIM_V3(a.u.vec.x * b.u.vec.x, a.u.vec.y * b.u.vec.y, a.u.vec.z * b.u.vec.z));
                if (op == TOK_STAR && a.kind == VAL_VEC)
                {
                    float k = (float)as_num(sc, b, 1.0);
                    return val_vec(ANIM_V3(a.u.vec.x * k, a.u.vec.y * k, a.u.vec.z * k));
                }
                if (op == TOK_STAR && b.kind == VAL_VEC)
                {
                    float k = (float)as_num(sc, a, 1.0);
                    return val_vec(ANIM_V3(b.u.vec.x * k, b.u.vec.y * k, b.u.vec.z * k));
                }
                if (op == TOK_SLASH && a.kind == VAL_VEC)
                {
                    float k = (float)as_num(sc, b, 1.0);
                    return val_vec(fabsf(k) > 1e-7f ? ANIM_V3(a.u.vec.x / k, a.u.vec.y / k, a.u.vec.z / k) : a.u.vec);
                }
            }

            {
                double x = as_num(sc, a, 0.0), y = as_num(sc, b, 0.0);
                switch (op)
                {
                    case TOK_PLUS:    return val_num(x + y);
                    case TOK_MINUS:   return val_num(x - y);
                    case TOK_STAR:    return val_num(x * y);
                    case TOK_SLASH:   return val_num(fabs(y) > 1e-12 ? (x / y) : 0.0);
                    case TOK_PERCENT: return val_num(fabs(y) > 1e-12 ? fmod(x, y) : 0.0);
                    case TOK_CARET:   return val_num(pow(x, y));
                    case TOK_LT:      return val_bool(x < y);
                    case TOK_LE:      return val_bool(x <= y);
                    case TOK_GT:      return val_bool(x > y);
                    case TOK_GE:      return val_bool(x >= y);
                    case TOK_EQ:      return val_bool(fabs(x - y) < 1e-9);
                    case TOK_NEQ:     return val_bool(fabs(x - y) >= 1e-9);
                    case TOK_AND:     return val_bool(as_bool(sc, a) && as_bool(sc, b));
                    case TOK_OR:      return val_bool(as_bool(sc, a) || as_bool(sc, b));
                    default: break;
                }
            }
            return val_nil();
        }
        case EX_INDEX:
        {
            Val obj = eval_expr(sc, e->u.index.obj);
            int idx = (int)as_num(sc, eval_expr(sc, e->u.index.idx), 0.0);
            if (obj.kind == VAL_LIST && idx >= 0 && (uint32_t)idx < obj.u.list.count)
                return obj.u.list.items[idx];
            if (obj.kind == VAL_VEC)
            {
                if (idx == 0) return val_num((double)obj.u.vec.x);
                if (idx == 1) return val_num((double)obj.u.vec.y);
                if (idx == 2) return val_num((double)obj.u.vec.z);
            }
            return val_nil();
        }
        case EX_FIELD:
        {
            Val         obj = eval_expr(sc, e->u.field.obj);
            const char *f   = e->u.field.field;
            if (obj.kind == VAL_VEC)
            {
                if (str_eq(f, "x")) return val_num((double)obj.u.vec.x);
                if (str_eq(f, "y")) return val_num((double)obj.u.vec.y);
                if (str_eq(f, "z")) return val_num((double)obj.u.vec.z);
            }
            if (obj.kind == VAL_LIST && str_eq(f, "len"))
                return val_num((double)obj.u.list.count);
            if (obj.kind == VAL_BAG)
            {
                Val out;
                if (bag_get(&obj.u.bag, f, &out)) return out;
            }
            return val_nil();
        }
        case EX_CALL:
        {
            const Expr       *raw_args[SC_MAX_ARGS];
            const char *const *arg_names = (const char *const *)e->u.call.arg_names;
            Val               args[SC_MAX_ARGS];
            uint32_t          ac = MU_MIN(e->u.call.arg_count, (uint32_t)SC_MAX_ARGS);
            for (uint32_t i = 0u; i < ac; ++i)
            {
                raw_args[i] = e->u.call.args[i];
                args[i]     = eval_expr(sc, raw_args[i]);
            }
            if (e->u.call.callee->kind == EX_IDENT)
            {
                const char *fn_name = e->u.call.callee->u.ident.name;
                VarSlot    *vs      = env_find(sc->cur_env, fn_name);
                if (vs && vs->val.kind == VAL_FN)
                    return invoke_script_fn(sc, vs->val.u.fn, args, ac, e->line);
                if (vs && vs->val.kind == VAL_MOB && ac >= 2u)
                    return val_vec(anim_axes_c2p(sc->scene, vs->val.u.mob, (float)as_num(sc, args[0], 0.0), (float)as_num(sc, args[1], 0.0)));
                return eval_builtin(sc, fn_name, raw_args, arg_names, args, ac, e->line);
            }
            else
            {
                Val cv = eval_expr(sc, e->u.call.callee);
                if (cv.kind == VAL_FN)
                    return invoke_script_fn(sc, cv.u.fn, args, ac, e->line);
                if (cv.kind == VAL_MOB && ac >= 2u)
                    return val_vec(anim_axes_c2p(sc->scene, cv.u.mob, (float)as_num(sc, args[0], 0.0), (float)as_num(sc, args[1], 0.0)));
            }
            sc_error(sc, e->line, "cannot call expression");
            return val_nil();
        }
        case EX_PIPE:
        {
            const Expr       *raw_args[SC_MAX_ARGS];
            const char       *names_buf[SC_MAX_ARGS];
            Val               args[SC_MAX_ARGS];
            uint32_t          ac = MU_MIN(e->u.pipe.arg_count + 1u, (uint32_t)SC_MAX_ARGS);
            VarSlot          *vs;
            raw_args[0]  = e->u.pipe.lhs;
            names_buf[0] = NULL;
            args[0]      = eval_expr(sc, e->u.pipe.lhs);
            for (uint32_t i = 1u; i < ac; ++i)
            {
                raw_args[i]  = e->u.pipe.args[i - 1u];
                names_buf[i] = e->u.pipe.arg_names ? e->u.pipe.arg_names[i - 1u] : NULL;
                args[i]      = eval_expr(sc, raw_args[i]);
            }
            vs = env_find(sc->cur_env, e->u.pipe.fn_name);
            if (vs && vs->val.kind == VAL_FN)
                return invoke_script_fn(sc, vs->val.u.fn, args, ac, e->line);
            return eval_builtin(sc, e->u.pipe.fn_name, raw_args, names_buf, args, ac, e->line);
        }
    }
    return val_nil();
}

static void exec_scene_bag(AnimScript *sc, const BagField *fields, uint32_t count)
{
    AnimScene      *s = sc->scene;
    AnimScriptOpts *o = sc->opts;

    for (uint32_t i = 0u; i < count; ++i)
    {
        const char *k = fields[i].key;
        Val         v = eval_expr(sc, fields[i].val);
        if (str_eq(k, "bg"))
        {
            if (v.kind == VAL_PAIR_COL)
                anim_scene_set_bg(s, v.u.pair_col.c0, v.u.pair_col.c1);
            else if (v.kind == VAL_COL)
                anim_scene_set_bg(s, v.u.col, v.u.col);
        }
        else if (str_eq(k, "camera"))
        {
            anim_scene_set_camera(s, bag_to_camera(sc, v, anim_scene_camera(s)));
        }
        else if (str_eq(k, "cache_dir") && as_str(v))
        {
            anim_scene_set_cache_dir(s, as_str(v));
        }
        else if (o)
        {
            o->has_render_overrides = true;
            if (str_eq(k, "fps"))             o->render.fps = (uint32_t)MU_MAX(as_num(sc, v, 30.0), 1.0);
            else if (str_eq(k, "blur"))       o->render.motion_blur_samples = (uint32_t)MU_MAX(as_num(sc, v, 1.0), 1.0);
            else if (str_eq(k, "bloom"))      { o->render.bloom_strength = (float)as_num(sc, v, 0.4); o->render.bloom = o->render.bloom_strength > 0.0f; }
            else if (str_eq(k, "exposure"))   o->render.exposure = (float)as_num(sc, v, 1.0);
            else if (str_eq(k, "saturation")) o->render.saturation = (float)as_num(sc, v, 1.0);
            else if (str_eq(k, "aberration"))   o->render.aberration_px = (float)as_num(sc, v, 0.0);
            else if (str_eq(k, "grain"))        o->render.grain_amount = (float)as_num(sc, v, 0.0);
            else if (str_eq(k, "dof_aperture")) o->render.dof_aperture_px = (float)as_num(sc, v, 0.0);
            else if (str_eq(k, "dof_focus"))    o->render.dof_focus_dist = (float)as_num(sc, v, 8.0);
            else if (str_eq(k, "dof_range"))    o->render.dof_focus_range = (float)as_num(sc, v, 1.5);
            else if (str_eq(k, "width"))      o->render.width = (uint32_t)MU_MAX(as_num(sc, v, 1280.0), 64.0);
            else if (str_eq(k, "height"))      o->render.height = (uint32_t)MU_MAX(as_num(sc, v, 720.0), 64.0);
            else if (str_eq(k, "transparent")) o->render.transparent_bg = as_bool(sc, v);
            else if (str_eq(k, "tonemap"))
            {
                if (v.kind == VAL_NUM) o->render.tonemap = (AnimToneMapKind)(int)v.u.num;
            }
        }
    }
}

static void exec_stmt(AnimScript *sc, const Stmt *st)
{
    if (!st || sc->has_error || sc->returning)
        return;

    switch (st->kind)
    {
        case ST_EXPR:
            (void)eval_expr(sc, st->u.expr.expr);
            break;
        case ST_ASSIGN:
        {
            Val v = eval_expr(sc, st->u.assign.rhs);
            VarSlot *existing = env_find(sc->cur_env, st->u.assign.name);
            if (existing && existing->val.kind == VAL_TRACKER && (v.kind == VAL_NUM || v.kind == VAL_BOOL))
            {
                anim_tracker_set(sc->scene, existing->val.u.tracker, (float)as_num(sc, v, 0.0));
                break;
            }
            env_set(sc, sc->cur_env, st->u.assign.name, v, false, NULL);
            break;
        }
        case ST_REACT:
        {
            const Expr *rhs = st->u.assign.rhs;
            bool is_const_num = (rhs->kind == EX_NUM) ||
                                (rhs->kind == EX_UNARY && rhs->u.unary.rhs && rhs->u.unary.rhs->kind == EX_NUM);
            Val v = eval_expr(sc, rhs);
            if (is_const_num)
            {
                AnimTrackerId tid = anim_tracker_create(sc->scene, (float)as_num(sc, v, 0.0));
                env_set(sc, sc->cur_env, st->u.assign.name, val_trk(tid), false, NULL);
            }
            else
            {
                env_set(sc, sc->cur_env, st->u.assign.name, v, true, (Expr *)rhs);
            }
            break;
        }
        case ST_SCENE:
            exec_scene_bag(sc, st->u.scene.fields, st->u.scene.count);
            break;
        case ST_BLOCK:
            for (uint32_t i = 0u; i < st->u.block.count && !sc->has_error && !sc->returning; ++i)
                exec_stmt(sc, st->u.block.items[i]);
            break;
        case ST_IF:
            if (as_bool(sc, eval_expr(sc, st->u.ifs.cond)))
                exec_stmt(sc, st->u.ifs.then_b);
            else if (st->u.ifs.else_b)
                exec_stmt(sc, st->u.ifs.else_b);
            break;
        case ST_FOR_LIST:
        {
            Val lv = eval_expr(sc, st->u.for_l.list);
            EnvScope *prev = sc->cur_env;
            EnvScope *loc  = env_push(sc, prev);
            sc->cur_env = loc;
            if (lv.kind == VAL_RANGE)
            {
                double lo   = lv.u.range.lo;
                double hi   = lv.u.range.hi;
                double step = lv.u.range.has_step && fabs(lv.u.range.step) > 1e-9 ? lv.u.range.step : 1.0;
                for (double x = lo; (step > 0.0 ? x < hi - 1e-9 : x > hi + 1e-9) && !sc->has_error && !sc->returning; x += step)
                {
                    env_set(sc, loc, st->u.for_l.var, val_num(x), false, NULL);
                    exec_stmt(sc, st->u.for_l.body);
                }
            }
            else if (lv.kind == VAL_LIST)
            {
                for (uint32_t i = 0u; i < lv.u.list.count && !sc->has_error && !sc->returning; ++i)
                {
                    env_set(sc, loc, st->u.for_l.var, lv.u.list.items[i], false, NULL);
                    exec_stmt(sc, st->u.for_l.body);
                }
            }
            else if (lv.kind == VAL_VEC)
            {
                double comp[3] = { lv.u.vec.x, lv.u.vec.y, lv.u.vec.z };
                for (uint32_t i = 0u; i < 3u && !sc->has_error && !sc->returning; ++i)
                {
                    env_set(sc, loc, st->u.for_l.var, val_num(comp[i]), false, NULL);
                    exec_stmt(sc, st->u.for_l.body);
                }
            }
            sc->cur_env = prev;
            break;
        }
        case ST_FN:
        {
            ScriptFn *fn = (ScriptFn *)sc_alloc(sc, sizeof *fn);
            Val       v  = { .kind = VAL_FN };
            fn->is_lambda   = false;
            fn->param_count = st->u.fn.param_count;
            for (uint32_t i = 0u; i < fn->param_count; ++i)
                fn->params[i] = st->u.fn.params[i];
            fn->fn_body = st->u.fn.body;
            v.u.fn = fn;
            env_set(sc, sc->cur_env, st->u.fn.name, v, false, NULL);
            break;
        }
        case ST_RETURN:
            sc->ret_val   = st->u.ret.expr ? eval_expr(sc, st->u.ret.expr) : val_nil();
            sc->returning = true;
            break;
        case ST_WAIT:
            anim_wait(sc->tl, sc->scene, (float)MU_MAX(as_num(sc, eval_expr(sc, st->u.wait.dur), 0.5), 0.01));
            break;
        case ST_ARROW_CUE:
        {
            if (!sc->in_play) { sc_error(sc, st->line, "'->' cue only valid inside play/lagged/sequence/at block"); break; }
            if (st->u.arrow_cue.lhs->kind == EX_IDENT && str_eq(st->u.arrow_cue.lhs->u.ident.name, "camera"))
            {
                Val rv = eval_expr(sc, st->u.arrow_cue.rhs);
                push_play_spec(sc, anim_anim_camera_to(bag_to_camera(sc, rv, anim_scene_camera(sc->scene))));
                break;
            }
            if (st->u.arrow_cue.lhs->kind == EX_FIELD)
            {
                Val obj = eval_expr(sc, st->u.arrow_cue.lhs->u.field.obj);
                const char *prop = st->u.arrow_cue.lhs->u.field.field;
                if (obj.kind != VAL_MOB) { sc_error(sc, st->line, "property target must be a mobject"); break; }
                if ((str_eq(prop, "pos") || str_eq(prop, "to")) &&
                    st->u.arrow_cue.rhs->kind == EX_UNARY && st->u.arrow_cue.rhs->u.unary.op == TOK_PLUS)
                {
                    Val dv = eval_expr(sc, st->u.arrow_cue.rhs->u.unary.rhs);
                    push_play_spec(sc, anim_anim_shift(obj.u.mob, as_vec(sc, dv, ANIM_ORIGIN)));
                    break;
                }
                {
                    Val rv = eval_expr(sc, st->u.arrow_cue.rhs);
                    if (str_eq(prop, "pos") || str_eq(prop, "to"))
                        push_play_spec(sc, anim_anim_move_to(obj.u.mob, as_vec(sc, rv, ANIM_ORIGIN)));
                    else if (str_eq(prop, "scale"))
                        push_play_spec(sc, anim_anim_scale(obj.u.mob, (float)as_num(sc, rv, 1.0)));
                    else if (str_eq(prop, "rot") || str_eq(prop, "rotate"))
                        push_play_spec(sc, anim_anim_rotate(obj.u.mob, (float)as_num(sc, rv, 0.0), ANIM_OUT));
                    else if (str_eq(prop, "opacity"))
                        push_play_spec(sc, anim_anim_set_opacity(obj.u.mob, (float)as_num(sc, rv, 1.0)));
                    else if (str_eq(prop, "fill") || str_eq(prop, "color"))
                        push_play_spec(sc, anim_anim_set_color(obj.u.mob, as_col(rv, ANIM_WHITE)));
                    else if (str_eq(prop, "stroke"))
                    {
                        AnimColor c = (rv.kind == VAL_LIST && rv.u.list.count >= 1u)
                            ? as_col(rv.u.list.items[0], ANIM_WHITE)
                            : as_col(rv, ANIM_WHITE);
                        push_play_spec(sc, anim_anim_set_color(obj.u.mob, c));
                    }
                    else
                        sc_error(sc, st->line, "unknown animation property '.%s'", prop);
                }
                break;
            }
            {
                Val lv = eval_expr(sc, st->u.arrow_cue.lhs);
                Val rv = eval_expr(sc, st->u.arrow_cue.rhs);
                if (lv.kind == VAL_TRACKER)
                    push_play_spec(sc, anim_anim_tracker_to(lv.u.tracker, (float)as_num(sc, rv, 0.0)));
                else if (lv.kind == VAL_MOB && (rv.kind == VAL_VEC || (rv.kind == VAL_LIST && rv.u.list.count >= 2u)))
                    push_play_spec(sc, anim_anim_move_to(lv.u.mob, as_vec(sc, rv, ANIM_ORIGIN)));
                else if (lv.kind == VAL_MOB && rv.kind == VAL_COL)
                    push_play_spec(sc, anim_anim_set_color(lv.u.mob, rv.u.col));
                else if (lv.kind == VAL_MOB && rv.kind == VAL_NUM)
                    push_play_spec(sc, anim_anim_set_opacity(lv.u.mob, (float)rv.u.num));
                else if (lv.kind == VAL_MOB && rv.kind == VAL_MOB)
                    push_play_spec(sc, anim_anim_transform(lv.u.mob, rv.u.mob));
                else
                    sc_error(sc, st->line, "unsupported types for '->' cue");
            }
            break;
        }
        case ST_PLAY:
        {
            float dur  = st->u.play.dur ? (float)MU_MAX(as_num(sc, eval_expr(sc, st->u.play.dur), 1.0), 0.01) : 1.0f;
            AnimRateKind rate = st->u.play.rate ? as_rate(eval_expr(sc, st->u.play.rate), ANIM_RATE_SMOOTH) : ANIM_RATE_SMOOTH;
            bool prev_in = sc->in_play;
            uint32_t prev_n = sc->play_count;
            sc->in_play    = true;
            sc->play_count = 0u;
            exec_stmt(sc, st->u.play.body);
            if (!sc->has_error && sc->play_count > 0u)
            {
                if (st->u.play.mode == 0)
                    anim_play(sc->tl, sc->scene, sc->play_specs, sc->play_count, dur, rate);
                else if (st->u.play.mode == 1)
                {
                    float lag = st->u.play.lag ? (float)as_num(sc, eval_expr(sc, st->u.play.lag), 0.15) : 0.15f;
                    anim_play_lagged(sc->tl, sc->scene, sc->play_specs, sc->play_count, dur, lag, rate);
                }
                else if (st->u.play.mode == 2)
                    anim_play_sequence(sc->tl, sc->scene, sc->play_specs, sc->play_count, dur, rate);
                else if (st->u.play.mode == 3)
                {
                    float t0 = (float)as_num(sc, eval_expr(sc, st->u.play.t0), 0.0);
                    anim_play_at(sc->tl, sc->scene, t0, sc->play_specs, sc->play_count, dur, rate);
                }
            }
            sc->in_play    = prev_in;
            sc->play_count = prev_n;
            break;
        }
    }
}

/* ----------------------------------------------------- Built-in Constants */

static void seed_builtins(AnimScript *sc)
{
    EnvScope *g = sc->global;
    env_set(sc, g, "PI",      val_num((double)ANIM_PI), false, NULL);
    env_set(sc, g, "TAU",     val_num((double)ANIM_TAU), false, NULL);
    env_set(sc, g, "ORIGIN",  val_vec(ANIM_ORIGIN), false, NULL);
    env_set(sc, g, "UP",      val_vec(ANIM_UP), false, NULL);
    env_set(sc, g, "DOWN",    val_vec(ANIM_DOWN), false, NULL);
    env_set(sc, g, "LEFT",    val_vec(ANIM_LEFT), false, NULL);
    env_set(sc, g, "RIGHT",   val_vec(ANIM_RIGHT), false, NULL);
    env_set(sc, g, "OUT",     val_vec(ANIM_OUT), false, NULL);
    env_set(sc, g, "IN",      val_vec(ANIM_IN), false, NULL);
    env_set(sc, g, "UL",      val_vec(ANIM_UL), false, NULL);
    env_set(sc, g, "UR",      val_vec(ANIM_UR), false, NULL);
    env_set(sc, g, "DL",      val_vec(ANIM_DL), false, NULL);
    env_set(sc, g, "DR",      val_vec(ANIM_DR), false, NULL);

    env_set(sc, g, "WHITE",   val_col(ANIM_WHITE), false, NULL);
    env_set(sc, g, "BLACK",   val_col(ANIM_BLACK), false, NULL);
    env_set(sc, g, "RED",     val_col(ANIM_RED), false, NULL);
    env_set(sc, g, "GREEN",   val_col(ANIM_GREEN), false, NULL);
    env_set(sc, g, "BLUE",    val_col(ANIM_BLUE), false, NULL);
    env_set(sc, g, "TEAL",    val_col(ANIM_TEAL), false, NULL);
    env_set(sc, g, "GOLD",    val_col(ANIM_GOLD), false, NULL);
    env_set(sc, g, "YELLOW",  val_col(ANIM_YELLOW), false, NULL);
    env_set(sc, g, "PURPLE",  val_col(ANIM_PURPLE), false, NULL);
    env_set(sc, g, "PINK",    val_col(ANIM_PINK), false, NULL);
    env_set(sc, g, "ORANGE",  val_col(ANIM_GOLD), false, NULL);
    env_set(sc, g, "GREY",    val_col(ANIM_GREY_B), false, NULL);
    env_set(sc, g, "CLEAR",   val_col(anim_rgba(0,0,0,0)), false, NULL);

    env_set(sc, g, "linear",              val_rate(ANIM_RATE_LINEAR), false, NULL);
    env_set(sc, g, "smooth",              val_rate(ANIM_RATE_SMOOTH), false, NULL);
    env_set(sc, g, "double_smooth",       val_rate(ANIM_RATE_DOUBLE_SMOOTH), false, NULL);
    env_set(sc, g, "rush_into",           val_rate(ANIM_RATE_RUSH_INTO), false, NULL);
    env_set(sc, g, "rush_from",           val_rate(ANIM_RATE_RUSH_FROM), false, NULL);
    env_set(sc, g, "there_and_back",      val_rate(ANIM_RATE_THERE_AND_BACK), false, NULL);
    env_set(sc, g, "there_and_back_pause",val_rate(ANIM_RATE_THERE_AND_BACK_PAUSE), false, NULL);
    env_set(sc, g, "wiggle",              val_rate(ANIM_RATE_WIGGLE), false, NULL);
    env_set(sc, g, "lingering",           val_rate(ANIM_RATE_LINGERING), false, NULL);
    env_set(sc, g, "running_start",       val_rate(ANIM_RATE_RUNNING_START), false, NULL);
    env_set(sc, g, "ease_out_back",       val_rate(ANIM_RATE_EASE_OUT_BACK), false, NULL);
    env_set(sc, g, "cubic",               val_rate(ANIM_RATE_EASE_IN_OUT_CUBIC), false, NULL);
    env_set(sc, g, "spring",              val_rate(ANIM_RATE_SPRING), false, NULL);

    env_set(sc, g, "soft",    val_num((double)ANIM_TONEMAP_SOFT), false, NULL);
    env_set(sc, g, "aces",    val_num((double)ANIM_TONEMAP_ACES), false, NULL);
    env_set(sc, g, "agx",     val_num((double)ANIM_TONEMAP_AGX), false, NULL);

    env_set(sc, g, "tetra",   val_num((double)ANIM_POLY_TETRAHEDRON), false, NULL);
    env_set(sc, g, "cube_poly",val_num((double)ANIM_POLY_CUBE), false, NULL);
    env_set(sc, g, "octa",    val_num((double)ANIM_POLY_OCTAHEDRON), false, NULL);
    env_set(sc, g, "dodeca",  val_num((double)ANIM_POLY_DODECAHEDRON), false, NULL);
    env_set(sc, g, "icosa",   val_num((double)ANIM_POLY_ICOSAHEDRON), false, NULL);

    env_set(sc, g, "default", val_cam(anim_camera_default()), false, NULL);
    env_set(sc, g, "null",    val_nil(), false, NULL);
}

/* --------------------------------------------------------------- Public API */

AnimScript *anim_script_create(void)
{
    AnimScript *sc = (AnimScript *)calloc(1, sizeof *sc);
    MU_ASSERT(sc != NULL);
    sc->global  = env_push(sc, NULL);
    sc->cur_env = sc->global;
    seed_builtins(sc);
    return sc;
}

void anim_script_destroy(AnimScript *sc)
{
    MemBlock *b;
    if (!sc)
        return;
    free(sc->toks);
    b = sc->mem_head;
    while (b)
    {
        MemBlock *next = b->next;
        free(b->ptr);
        free(b);
        b = next;
    }
    free(sc);
}

bool anim_script_exec_string(AnimScript *sc, AnimScene *scene, AnimTimeline *tl,
                             const char *source, const char *filename, AnimScriptOpts *out_opts)
{
    if (!sc || !scene || !tl || !source)
        return false;

    sc->scene     = scene;
    sc->tl        = tl;
    sc->opts      = out_opts;
    sc->filename  = filename ? filename : "<string>";
    sc->src       = source;
    sc->pos       = 0u;
    sc->has_error = false;
    sc->err[0]    = '\0';
    sc->returning = false;
    sc->in_play   = false;
    sc->play_count = 0u;
    sc->cur_env   = sc->global;
    sc->rng_state = 0x9e3779b97f4a7c15ULL;

    if (out_opts)
    {
        out_opts->render = anim_render_opts_default(ANIM_FRAME_PX_W, ANIM_FRAME_PX_H);
        out_opts->render.tonemap = ANIM_TONEMAP_AGX;
        out_opts->has_render_overrides = false;
    }

    if (!tokenize(sc, source))
        return false;

    skip_sep(sc);
    while (peek(sc).kind != TOK_EOF && !sc->has_error)
    {
        Stmt *st = parse_stmt(sc);
        if (!sc->has_error)
            exec_stmt(sc, st);
        skip_sep(sc);
    }
    return !sc->has_error;
}

bool anim_script_exec_file(AnimScript *sc, AnimScene *scene, AnimTimeline *tl,
                           const char *path, AnimScriptOpts *out_opts)
{
    void  *data = NULL;
    size_t sz   = 0u;
    char  *str;
    bool   ok;

    if (!sc || !path || !mu_files_read_all(path, &data, &sz))
    {
        if (sc)
            sc_error(sc, 1, "failed to read script file '%s'", path ? path : "(null)");
        return false;
    }
    str = (char *)malloc(sz + 1u);
    MU_ASSERT(str != NULL);
    if (sz > 0u)
        memcpy(str, data, sz);
    str[sz] = '\0';
    free(data);

    ok = anim_script_exec_string(sc, scene, tl, str, path, out_opts);
    free(str);
    return ok;
}

const char *anim_script_error(const AnimScript *sc)
{
    return (sc && sc->has_error) ? sc->err : "";
}

AnimId anim_script_get_mobject(const AnimScript *sc, const char *name)
{
    VarSlot *v;
    if (!sc || !name || !sc->global)
        return ANIM_ID_NIL;
    v = env_find(sc->global, name);
    return (v && v->val.kind == VAL_MOB) ? v->val.u.mob : ANIM_ID_NIL;
}

AnimTrackerId anim_script_get_tracker(const AnimScript *sc, const char *name)
{
    VarSlot *v;
    if (!sc || !name || !sc->global)
        return ANIM_TRACKER_NIL;
    v = env_find(sc->global, name);
    return (v && v->val.kind == VAL_TRACKER) ? v->val.u.tracker : ANIM_TRACKER_NIL;
}
