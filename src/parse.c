/* CalcOS lexer + Pratt parser. */
#include "calc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

typedef enum {
    T_END, T_NUM, T_IDENT, T_STR, T_OP,
    T_LP, T_RP, T_LB, T_RB, T_COMMA, T_SEMI, T_AT, T_ASSIGN, T_COLON
} TokType;

typedef struct {
    TokType t;
    int     op;
    double  num;
    int     imag;
    char    name[COS_MAXNAME];
    char   *str;
} Tok;

typedef struct {
    Interp     *in;
    const char *s;
    size_t      pos;
    Tok         cur, ahead;
    int         has_ahead;
} P;

/* ------------------------------------------------------------------ */

static void lex_error(P *p, const char *msg)
{
    cos_err(p->in, "parse error at offset %u: %s", (unsigned)p->pos, msg);
}

static Tok lex(P *p)
{
    Tok t;
    const char *s = p->s;
    char c;
    memset(&t, 0, sizeof t);

    while (s[p->pos] == ' ' || s[p->pos] == '\t' || s[p->pos] == '\r') p->pos++;
    c = s[p->pos];
    if (c == 0 || c == '\n' || c == '#') { t.t = T_END; return t; }

    if (isdigit((unsigned char)c) ||
        (c == '.' && isdigit((unsigned char)s[p->pos + 1]))) {
        char *end;
        t.t = T_NUM;
        t.num = strtod(s + p->pos, &end);
        p->pos = (size_t)(end - s);
        if (s[p->pos] == 'i' && !isalnum((unsigned char)s[p->pos + 1])
            && s[p->pos + 1] != '_') {
            t.imag = 1;
            p->pos++;
        }
        return t;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t st = p->pos;
        size_t n;
        while (isalnum((unsigned char)s[p->pos]) || s[p->pos] == '_') p->pos++;
        n = p->pos - st;
        if (n >= COS_MAXNAME) n = COS_MAXNAME - 1;
        memcpy(t.name, s + st, n);
        t.name[n] = 0;
        t.t = T_IDENT;
        return t;
    }

    if (c == '"' || c == '\'') {
        /* a quote directly after a value is the transpose operator */
        if (c == '\'' && p->cur.t != T_END) {
            TokType pt = p->cur.t;
            if (pt == T_NUM || pt == T_IDENT || pt == T_RP || pt == T_RB) {
                p->pos++;
                t.t = T_OP; t.op = OP_TRANS;
                return t;
            }
        }
        {
            char q = c;
            size_t st = ++p->pos;
            char *buf;
            while (s[p->pos] && s[p->pos] != q) p->pos++;
            if (!s[p->pos]) lex_error(p, "unterminated string");
            buf = (char *)cos_alloc(p->in, p->pos - st + 1);
            memcpy(buf, s + st, p->pos - st);
            buf[p->pos - st] = 0;
            p->pos++;
            t.t = T_STR;
            t.str = buf;
            return t;
        }
    }

    p->pos++;
    switch (c) {
    case '(': t.t = T_LP; return t;
    case ')': t.t = T_RP; return t;
    case '[': t.t = T_LB; return t;
    case ']': t.t = T_RB; return t;
    case ',': t.t = T_COMMA; return t;
    case ';': t.t = T_SEMI; return t;
    case '@': t.t = T_AT; return t;
    case ':': t.t = T_COLON; return t;
    case '+': t.t = T_OP; t.op = OP_ADD; return t;
    case '-': t.t = T_OP; t.op = OP_SUB; return t;
    case '*': t.t = T_OP; t.op = OP_MUL; return t;
    case '/': t.t = T_OP; t.op = OP_DIV; return t;
    case '^': t.t = T_OP; t.op = OP_POW; return t;
    case '%': t.t = T_OP; t.op = OP_MOD; return t;
    case '!':
        if (s[p->pos] == '=') { p->pos++; t.t = T_OP; t.op = OP_NE; return t; }
        t.t = T_OP; t.op = OP_FACT; return t;
    case '=':
        if (s[p->pos] == '=') { p->pos++; t.t = T_OP; t.op = OP_EQ; return t; }
        t.t = T_ASSIGN; return t;
    case '<':
        if (s[p->pos] == '=') { p->pos++; t.t = T_OP; t.op = OP_LE; return t; }
        t.t = T_OP; t.op = OP_LT; return t;
    case '>':
        if (s[p->pos] == '=') { p->pos++; t.t = T_OP; t.op = OP_GE; return t; }
        t.t = T_OP; t.op = OP_GT; return t;
    case '&':
        if (s[p->pos] == '&') p->pos++;
        t.t = T_OP; t.op = OP_AND; return t;
    case '|':
        if (s[p->pos] == '|') p->pos++;
        t.t = T_OP; t.op = OP_OR; return t;
    case '.':
        if (s[p->pos] == '*') { p->pos++; t.t = T_OP; t.op = OP_EMUL; return t; }
        if (s[p->pos] == '/') { p->pos++; t.t = T_OP; t.op = OP_EDIV; return t; }
        if (s[p->pos] == '^') { p->pos++; t.t = T_OP; t.op = OP_EPOW; return t; }
        if (s[p->pos] == '\'') { p->pos++; t.t = T_OP; t.op = OP_TRANS; return t; }
        lex_error(p, "stray '.'");
        break;
    default:
        cos_err(p->in, "unexpected character '%c'", c);
    }
    t.t = T_END;
    return t;
}

static void advance(P *p)
{
    if (p->has_ahead) { p->cur = p->ahead; p->has_ahead = 0; return; }
    p->cur = lex(p);
}

static Tok *peek(P *p)
{
    if (!p->has_ahead) {
        Tok save = p->cur;
        p->ahead = lex(p);
        p->cur = save;
        p->has_ahead = 1;
    }
    return &p->ahead;
}

/* ------------------------------------------------------------------ */

static Node *node(Interp *in, NType t)
{
    Node *n = (Node *)cos_alloc(in, sizeof(Node));
    n->t = t;
    return n;
}

static Node *parse_expr(P *p, int minbp);

static void expect(P *p, TokType t, const char *what)
{
    if (p->cur.t != t) cos_err(p->in, "expected %s", what);
    advance(p);
}

static Node *parse_matrix(P *p)
{
    Interp *in = p->in;
    Node *m = node(in, N_MATRIX);
    Node **rows = NULL;
    int nrows = 0, cap = 0;

    advance(p);                             /* consume '[' */
    if (p->cur.t == T_RB) { advance(p); m->nlist = 0; return m; }

    for (;;) {
        Node *row = node(in, N_ROW);
        Node **els = NULL;
        int ne = 0, ecap = 0;
        for (;;) {
            Node *e = parse_expr(p, 1);
            if (ne >= ecap) {
                int nc = ecap ? ecap * 2 : 4;
                Node **t2 = (Node **)cos_alloc(in, (size_t)nc * sizeof(Node *));
                if (els) memcpy(t2, els, (size_t)ne * sizeof(Node *));
                els = t2; ecap = nc;
            }
            els[ne++] = e;
            if (p->cur.t == T_COMMA) { advance(p); continue; }
            break;
        }
        row->list = els;
        row->nlist = ne;
        if (nrows >= cap) {
            int nc = cap ? cap * 2 : 4;
            Node **t2 = (Node **)cos_alloc(in, (size_t)nc * sizeof(Node *));
            if (rows) memcpy(t2, rows, (size_t)nrows * sizeof(Node *));
            rows = t2; cap = nc;
        }
        rows[nrows++] = row;
        if (p->cur.t == T_SEMI) { advance(p); continue; }
        break;
    }
    expect(p, T_RB, "']'");
    m->list = rows;
    m->nlist = nrows;
    return m;
}

static Node *parse_lambda(P *p)
{
    Interp *in = p->in;
    Node *n = node(in, N_LAMBDA);
    FuncDef *f = (FuncDef *)cos_alloc(in, sizeof(FuncDef));
    advance(p);                              /* consume '@' */
    expect(p, T_LP, "'(' after @");
    while (p->cur.t == T_IDENT) {
        if (f->nparams >= COS_MAXPARAM)
            cos_err(in, "too many parameters (max %d)", COS_MAXPARAM);
        cos_setname(f->params[f->nparams], p->cur.name);
        f->nparams++;
        advance(p);
        if (p->cur.t == T_COMMA) { advance(p); continue; }
        break;
    }
    expect(p, T_RP, "')'");
    f->body = parse_expr(p, 1);
    n->fn = f;
    return n;
}

static Node *parse_call_args(P *p, Node *callee)
{
    Interp *in = p->in;
    Node *c = node(in, N_CALL);
    Node **args = NULL;
    int na = 0, cap = 0;
    c->a = callee;
    cos_setname(c->name, callee->name);
    advance(p);                              /* consume '(' */
    if (p->cur.t != T_RP) {
        for (;;) {
            Node *e = parse_expr(p, 1);
            if (na >= cap) {
                int nc = cap ? cap * 2 : 4;
                Node **t2 = (Node **)cos_alloc(in, (size_t)nc * sizeof(Node *));
                if (args) memcpy(t2, args, (size_t)na * sizeof(Node *));
                args = t2; cap = nc;
            }
            if (na >= COS_MAXARGS) cos_err(in, "too many arguments");
            args[na++] = e;
            if (p->cur.t == T_COMMA) { advance(p); continue; }
            break;
        }
    }
    expect(p, T_RP, "')'");
    c->list = args;
    c->nlist = na;
    return c;
}

/* The exponent of a unit must be a plain number. */
static void expect_num(P *p)
{
    if (p->cur.t != T_NUM) cos_err(p->in, "a unit power must be a number");
    advance(p);
}

static Node *parse_primary(P *p)
{
    Interp *in = p->in;
    Node *n;

    switch (p->cur.t) {
    case T_NUM:
        n = node(in, N_NUM);
        n->num = p->cur.imag ? cx_mk(0.0, p->cur.num) : cx_mk(p->cur.num, 0.0);
        advance(p);
        /* A number followed directly by the name of a unit means that
         * quantity: "5 m" is five metres. This is the only place where
         * juxtaposition means multiplication, and it is limited to names
         * that are units and are not shadowed by a variable or function
         * of the user's own, so nothing that worked before changes
         * meaning. */
        while (p->cur.t == T_IDENT && unit_lookup(p->cur.name, 0, 0)
               && !cos_lookup(in, p->cur.name)
               && !cos_lookup_fn(in, p->cur.name)) {
            Node *u = node(in, N_VAR);
            Node *mul = node(in, N_BIN);
            cos_setname(u->name, p->cur.name);
            advance(p);
            /* An exponent belongs to the unit, not to the quantity:
             * "16 m^2" is sixteen square metres, not 256 of them. */
            if (p->cur.t == T_OP && p->cur.op == OP_POW) {
                Node *pw = node(in, N_BIN);
                advance(p);
                pw->op = OP_POW;
                pw->a = u;
                if (p->cur.t == T_OP && p->cur.op == OP_SUB) {
                    Node *neg = node(in, N_UN);
                    advance(p);
                    neg->op = OP_NEG;
                    neg->a = node(in, N_NUM);
                    neg->a->num = cx_mk(p->cur.num, 0.0);
                    expect_num(p);
                    pw->b = neg;
                } else {
                    pw->b = node(in, N_NUM);
                    pw->b->num = cx_mk(p->cur.num, 0.0);
                    expect_num(p);
                }
                u = pw;
            }
            mul->op = OP_MUL;
            mul->a = n;
            mul->b = u;
            n = mul;
            /* "m/s" keeps going: a division by a unit name is part of the
             * same quantity, which is handled by the ordinary operator
             * loop, so only adjacent names are consumed here. */
        }
        break;
    case T_STR:
        n = node(in, N_STR);
        n->str = p->cur.str;
        advance(p);
        break;
    case T_IDENT:
        n = node(in, N_VAR);
        cos_setname(n->name, p->cur.name);
        advance(p);
        if (p->cur.t == T_LP) n = parse_call_args(p, n);
        break;
    case T_LP:
        advance(p);
        n = parse_expr(p, 1);
        expect(p, T_RP, "')'");
        break;
    case T_LB:
        n = parse_matrix(p);
        break;
    case T_AT:
        n = parse_lambda(p);
        break;
    case T_OP:
        if (p->cur.op == OP_SUB) {
            advance(p);
            n = node(in, N_UN);
            n->op = OP_NEG;
            n->a = parse_expr(p, 7);
            break;
        }
        if (p->cur.op == OP_ADD) { advance(p); return parse_primary(p); }
        if (p->cur.op == OP_FACT) {       /* prefix '!' is logical not */
            advance(p);
            n = node(in, N_UN);
            n->op = OP_NOT;
            n->a = parse_expr(p, 7);
            break;
        }
        cos_err(in, "unexpected operator");
        return NULL;
    default:
        cos_err(in, "unexpected end of expression");
        return NULL;
    }

    /* postfix */
    for (;;) {
        if (p->cur.t == T_OP && (p->cur.op == OP_TRANS || p->cur.op == OP_FACT)) {
            Node *u = node(in, N_POST);
            u->op = p->cur.op;
            u->a = n;
            n = u;
            advance(p);
            continue;
        }
        if (p->cur.t == T_LP && (n->t == N_VAR || n->t == N_CALL
                                 || n->t == N_POST || n->t == N_MATRIX)) {
            n = parse_call_args(p, n);
            continue;
        }
        break;
    }
    return n;
}

/* binding powers: higher binds tighter */
static int bp_of(Tok *t, int *rightassoc)
{
    *rightassoc = 0;
    if (t->t == T_COLON) return 4;
    if (t->t != T_OP) return 0;
    switch (t->op) {
    case OP_OR:  return 1;
    case OP_AND: return 2;
    case OP_EQ: case OP_NE: case OP_LT:
    case OP_GT: case OP_LE: case OP_GE: return 3;
    case OP_ADD: case OP_SUB: return 5;
    case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_EMUL: case OP_EDIV: return 6;
    case OP_POW: case OP_EPOW: *rightassoc = 1; return 8;
    default: return 0;
    }
}

static Node *parse_expr(P *p, int minbp)
{
    Node *lhs = parse_primary(p);
    for (;;) {
        int ra, bp;
        Tok t = p->cur;
        bp = bp_of(&t, &ra);
        if (bp == 0 || bp < minbp) break;

        if (t.t == T_COLON) {
            Node *r = node(p->in, N_RANGE);
            advance(p);
            r->a = lhs;
            r->b = parse_expr(p, 5);
            if (p->cur.t == T_COLON) {      /* a : step : b */
                advance(p);
                r->c = parse_expr(p, 5);
            }
            lhs = r;
            continue;
        }
        advance(p);
        {
            Node *b = node(p->in, N_BIN);
            b->op = t.op;
            b->a = lhs;
            b->b = parse_expr(p, ra ? bp : bp + 1);
            lhs = b;
        }
    }
    return lhs;
}

/* ------------------------------------------------------------------ */

Node *cos_parse(Interp *in, const char *src)
{
    P p;
    Node *n;
    memset(&p, 0, sizeof p);
    p.in = in;
    p.s = src;
    advance(&p);
    if (p.cur.t == T_END) return NULL;

    /* function definition: name(a, b) = expr */
    if (p.cur.t == T_IDENT) {
        size_t save_pos = p.pos;
        Tok save_cur = p.cur;
        if (peek(&p)->t == T_LP) {
            FuncDef *f = (FuncDef *)cos_alloc(in, sizeof(FuncDef));
            int ok = 1;
            cos_setname(f->name, p.cur.name);
            advance(&p);                     /* name */
            advance(&p);                     /* '(' */
            if (p.cur.t == T_RP) ok = 0;     /* zero-arg: treat as a call */
            while (ok && p.cur.t == T_IDENT) {
                if (f->nparams >= COS_MAXPARAM) { ok = 0; break; }
                cos_setname(f->params[f->nparams], p.cur.name);
                f->nparams++;
                advance(&p);
                if (p.cur.t == T_COMMA) { advance(&p); continue; }
                break;
            }
            if (ok && p.cur.t == T_RP) {
                advance(&p);
                if (p.cur.t == T_ASSIGN) {
                    advance(&p);
                    f->body = parse_expr(&p, 1);
                    if (p.cur.t != T_END) cos_err(in, "trailing input after definition");
                    n = node(in, N_FUNCDEF);
                    n->fn = f;
                    return n;
                }
            }
            /* not a definition: rewind and parse as an expression */
            p.pos = save_pos;
            p.cur = save_cur;
            p.has_ahead = 0;
        }
    }

    n = parse_expr(&p, 1);

    if (p.cur.t == T_ASSIGN) {
        Node *as;
        if (n->t != N_VAR && n->t != N_CALL)
            cos_err(in, "left side of '=' is not assignable");
        advance(&p);
        as = node(in, N_ASSIGN);
        as->a = n;
        as->b = parse_expr(&p, 1);
        n = as;
    }
    if (p.cur.t != T_END) cos_err(in, "unexpected trailing input");
    return n;
}

/* ------------------------------------------------------------------ */
/* permanent (malloc'd) AST copies                                     */
/* ------------------------------------------------------------------ */

Node *cos_node_clone(Node *n)
{
    Node *r;
    int i;
    if (!n) return NULL;
    r = (Node *)malloc(sizeof(Node));
    if (!r) return NULL;
    *r = *n;
    r->a = cos_node_clone(n->a);
    r->b = cos_node_clone(n->b);
    r->c = cos_node_clone(n->c);
    r->list = NULL;
    r->fn = NULL;
    if (n->str) {
        size_t sl = strlen(n->str) + 1;
        r->str = (char *)malloc(sl);
        if (r->str) memcpy(r->str, n->str, sl);
    }
    if (n->nlist > 0 && n->list) {
        r->list = (Node **)malloc((size_t)n->nlist * sizeof(Node *));
        if (r->list)
            for (i = 0; i < n->nlist; i++) r->list[i] = cos_node_clone(n->list[i]);
    }
    if (n->fn) r->fn = cos_fn_clone(n->fn);
    return r;
}

void cos_node_free(Node *n)
{
    int i;
    if (!n) return;
    cos_node_free(n->a);
    cos_node_free(n->b);
    cos_node_free(n->c);
    if (n->list) {
        for (i = 0; i < n->nlist; i++) cos_node_free(n->list[i]);
        free(n->list);
    }
    if (n->str) free(n->str);
    if (n->fn) cos_fn_free(n->fn);
    free(n);
}

FuncDef *cos_fn_clone(FuncDef *f)
{
    FuncDef *r;
    if (!f) return NULL;
    r = (FuncDef *)malloc(sizeof(FuncDef));
    if (!r) return NULL;
    *r = *f;
    r->next = NULL;
    r->perm = 1;
    r->body = cos_node_clone(f->body);
    return r;
}

void cos_fn_free(FuncDef *f)
{
    if (!f) return;
    cos_node_free(f->body);
    free(f);
}
