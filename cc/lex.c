/* Preprocessor and lexer.
 *
 * The two are fused: the token source is a stack of input buffers, and a
 * macro expansion is just another buffer pushed on top. That gives nested
 * and recursive-safe expansion without building a token list, which keeps
 * memory down.
 */
#include "cc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* arena                                                               */
/* ------------------------------------------------------------------ */

void *cc_alloc(CC *cc, size_t n)
{
    void *p;
    n = (n + 7u) & ~(size_t)7u;
    if (cc->arena_used + n > cc->arena_cap)
        cc_error(cc, "compiler out of memory");
    p = (char *)cc->arena + cc->arena_used;
    cc->arena_used += n;
    memset(p, 0, n);
    return p;
}

char *cc_strdup(CC *cc, const char *s, int n)
{
    char *p;
    if (n < 0) n = (int)strlen(s);
    p = (char *)cc_alloc(cc, (size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* keywords                                                            */
/* ------------------------------------------------------------------ */

static const struct { const char *s; int k; } keywords[] = {
    {"int", KW_INT}, {"char", KW_CHAR}, {"short", KW_SHORT}, {"long", KW_LONG},
    {"unsigned", KW_UNSIGNED}, {"signed", KW_SIGNED}, {"float", KW_FLOAT},
    {"double", KW_DOUBLE}, {"void", KW_VOID}, {"struct", KW_STRUCT},
    {"union", KW_UNION}, {"enum", KW_ENUM}, {"if", KW_IF}, {"else", KW_ELSE},
    {"while", KW_WHILE}, {"for", KW_FOR}, {"do", KW_DO}, {"return", KW_RETURN},
    {"break", KW_BREAK}, {"continue", KW_CONTINUE}, {"switch", KW_SWITCH},
    {"case", KW_CASE}, {"default", KW_DEFAULT}, {"sizeof", KW_SIZEOF},
    {"typedef", KW_TYPEDEF}, {"static", KW_STATIC}, {"extern", KW_EXTERN},
    {"const", KW_CONST}, {"goto", KW_GOTO},
    {NULL, 0}
};

static int keyword_of(const char *s)
{
    int i;
    for (i = 0; keywords[i].s; i++)
        if (strcmp(keywords[i].s, s) == 0) return keywords[i].k;
    return 0;
}

/* ------------------------------------------------------------------ */
/* input stack                                                         */
/* ------------------------------------------------------------------ */

static InBuf *top(CC *cc) { return &cc->in[cc->nin - 1]; }

static void push_buf(CC *cc, const char *text, const char *fname,
                     char *owned, Macro *m)
{
    InBuf *b;
    if (cc->nin >= CC_MAXINC) cc_error(cc, "include/macro nesting too deep");
    b = &cc->in[cc->nin++];
    b->p = text;
    b->end = text + strlen(text);
    b->fname = fname;
    b->line = 1;
    b->owned = owned;
    b->macro = m;
}

static int pop_buf(CC *cc)
{
    InBuf *b;
    if (cc->nin <= 1) return 0;
    b = top(cc);
    if (b->owned) free(b->owned);
    cc->nin--;
    return 1;
}

/* Pull a file in as another input buffer. The same file twice in one
 * compile is a cycle as often as not, and a header without an include
 * guard is the common case on a calculator, so a repeat is skipped. */
static void include_file(CC *cc, const char *name)
{
    char *buf;
    char *stored;
    int i, n, cap = 16 * 1024;

    for (i = 0; i < cc->ninc; i++)
        if (!strcmp(cc->incnames[i], name)) return;

    buf = (char *)malloc((size_t)cap);
    if (!buf) cc_error(cc, "out of memory reading %s", name);
    n = cc->readfile(cc->readfile_ud, name, buf, cap - 1);
    if (n < 0) {
        free(buf);
        cc_error(cc, "cannot open %s", name);
        return;
    }
    buf[n] = 0;
    if (cc->ninc < CC_MAXINC) {
        snprintf(cc->incnames[cc->ninc], CC_MAXNAME, "%s", name);
        cc->ninc++;
    }
    /* The buffer owns the name too, so diagnostics inside the file can
     * point at it after the directive line is gone. */
    stored = cc->ninc ? cc->incnames[cc->ninc - 1] : NULL;
    push_buf(cc, buf, stored ? stored : "include", buf, NULL);
}

static int in_macro(CC *cc, Macro *m)
{
    int i;
    for (i = 0; i < cc->nin; i++)
        if (cc->in[i].macro == m) return 1;
    return 0;
}

/* raw character access across the buffer stack */
static int cur(CC *cc)
{
    while (cc->nin > 0) {
        InBuf *b = top(cc);
        if (b->p < b->end) return (unsigned char)*b->p;
        if (!pop_buf(cc)) return 0;
    }
    return 0;
}

static int at(CC *cc, int n)
{
    InBuf *b = top(cc);
    return (b->p + n < b->end) ? (unsigned char)b->p[n] : 0;
}

static void adv(CC *cc, int n)
{
    InBuf *b = top(cc);
    while (n-- > 0 && b->p < b->end) {
        if (*b->p == '\n') b->line++;
        b->p++;
    }
}

void cc_error(CC *cc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cc->err, sizeof cc->err, fmt, ap);
    va_end(ap);
    cc->errline = cc->nin > 0 ? top(cc)->line : 0;
    cc->nerr++;
    longjmp(cc->jb, 1);
}

/* ------------------------------------------------------------------ */
/* whitespace and comments                                             */
/* ------------------------------------------------------------------ */

static int skip_space(CC *cc, int stop_at_newline)
{
    int saw_newline = 0;
    for (;;) {
        int c = cur(cc);
        if (c == 0) return saw_newline;
        if (c == '\\' && at(cc, 1) == '\n') { adv(cc, 2); continue; }
        if (c == '\n') {
            if (stop_at_newline) return saw_newline;
            saw_newline = 1;
            adv(cc, 1);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { adv(cc, 1); continue; }
        if (c == '/' && at(cc, 1) == '/') {
            while (cur(cc) && cur(cc) != '\n') adv(cc, 1);
            continue;
        }
        if (c == '/' && at(cc, 1) == '*') {
            adv(cc, 2);
            while (cur(cc) && !(cur(cc) == '*' && at(cc, 1) == '/')) adv(cc, 1);
            if (cur(cc)) adv(cc, 2);
            continue;
        }
        return saw_newline;
    }
}

/* ------------------------------------------------------------------ */
/* macros                                                              */
/* ------------------------------------------------------------------ */

static Macro *find_macro(CC *cc, const char *name)
{
    Macro *m;
    for (m = cc->macros; m; m = m->next)
        if (strcmp(m->name, name) == 0) return m;
    return NULL;
}

static int read_ident(CC *cc, char *out)
{
    int n = 0;
    int c = cur(cc);
    if (!(isalpha(c) || c == '_')) return 0;
    while ((c = cur(cc)) && (isalnum(c) || c == '_')) {
        if (n < CC_MAXNAME - 1) out[n++] = (char)c;
        adv(cc, 1);
    }
    out[n] = 0;
    return n;
}

/* read the rest of the logical line, splicing backslash-newline */
static char *read_line(CC *cc)
{
    char buf[1024];
    int n = 0;
    for (;;) {
        int c = cur(cc);
        if (c == 0 || c == '\n') break;
        if (c == '\\' && at(cc, 1) == '\n') { adv(cc, 2); continue; }
        if (c == '/' && at(cc, 1) == '*') {       /* comments vanish */
            adv(cc, 2);
            while (cur(cc) && !(cur(cc) == '*' && at(cc, 1) == '/')) adv(cc, 1);
            if (cur(cc)) adv(cc, 2);
            if (n < (int)sizeof buf - 1) buf[n++] = ' ';
            continue;
        }
        if (c == '/' && at(cc, 1) == '/') {
            while (cur(cc) && cur(cc) != '\n') adv(cc, 1);
            break;
        }
        if (n < (int)sizeof buf - 1) buf[n++] = (char)c;
        adv(cc, 1);
    }
    buf[n] = 0;
    return cc_strdup(cc, buf, n);
}

/* substitute arguments into a function-like macro body */
static char *expand_params(CC *cc, Macro *m, char **args)
{
    char out[2048];
    int n = 0, i;
    const char *p = m->body;
    while (*p) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            char id[CC_MAXNAME];
            int k = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                if (k < CC_MAXNAME - 1) id[k++] = *p;
                p++;
            }
            id[k] = 0;
            for (i = 0; i < m->nparams; i++)
                if (strcmp(id, m->params[i]) == 0) break;
            if (i < m->nparams) {
                const char *a = args[i];
                while (*a && n < (int)sizeof out - 1) out[n++] = *a++;
            } else {
                const char *a = id;
                while (*a && n < (int)sizeof out - 1) out[n++] = *a++;
            }
            continue;
        }
        /* string literals pass through untouched */
        if (*p == '"' || *p == '\'') {
            char q = *p;
            if (n < (int)sizeof out - 1) out[n++] = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1] && n < (int)sizeof out - 2) {
                    out[n++] = *p++;
                }
                if (n < (int)sizeof out - 1) out[n++] = *p++;
            }
            if (*p && n < (int)sizeof out - 1) out[n++] = *p++;
            continue;
        }
        if (n < (int)sizeof out - 1) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return cc_strdup(cc, out, n);
}

/* collect the actual arguments of a function-like macro invocation */
static int collect_args(CC *cc, Macro *m, char **args, char *store, int storecap)
{
    int depth = 0, na = 0, n = 0;
    skip_space(cc, 0);
    if (cur(cc) != '(') return 0;
    adv(cc, 1);
    args[0] = store;
    if (m->nparams == 0) {
        skip_space(cc, 0);
        if (cur(cc) == ')') { adv(cc, 1); return 0; }
    }
    for (;;) {
        int c = cur(cc);
        if (c == 0) cc_error(cc, "unterminated macro argument list");
        if (c == '(') depth++;
        if (c == ')') {
            if (depth == 0) {
                adv(cc, 1);
                store[n++] = 0;
                na++;
                break;
            }
            depth--;
        }
        if (c == ',' && depth == 0) {
            adv(cc, 1);
            store[n++] = 0;
            na++;
            if (na >= CC_MAXPARAMS) cc_error(cc, "too many macro arguments");
            args[na] = store + n;
            skip_space(cc, 0);
            continue;
        }
        if (n >= storecap - 2) cc_error(cc, "macro arguments too long");
        store[n++] = (char)c;
        adv(cc, 1);
    }
    return na;
}

/* ------------------------------------------------------------------ */
/* directives                                                          */
/* ------------------------------------------------------------------ */

static long pp_expr(CC *cc, const char **p);   /* forward */

static int pp_skip_to_endif(CC *cc, int stop_on_else)
{
    int depth = 0;
    for (;;) {
        int c;
        skip_space(cc, 1);
        c = cur(cc);
        if (c == 0) cc_error(cc, "unterminated #if");
        if (c == '\n') { adv(cc, 1); continue; }
        if (c == '#') {
            char id[CC_MAXNAME];
            adv(cc, 1);
            skip_space(cc, 1);
            read_ident(cc, id);
            if (!strcmp(id, "if") || !strcmp(id, "ifdef") || !strcmp(id, "ifndef"))
                depth++;
            else if (!strcmp(id, "endif")) {
                if (depth == 0) { read_line(cc); return 0; }
                depth--;
            } else if (depth == 0 && stop_on_else
                       && (!strcmp(id, "else") || !strcmp(id, "elif"))) {
                return !strcmp(id, "elif") ? 2 : 1;
            }
            read_line(cc);
            continue;
        }
        /* skip the line */
        while (cur(cc) && cur(cc) != '\n') adv(cc, 1);
    }
}

/* #if nesting state */
static int cond_depth;
static char cond_taken[16];

static void do_directive(CC *cc)
{
    char id[CC_MAXNAME];
    adv(cc, 1);                      /* '#' */
    skip_space(cc, 1);
    if (!read_ident(cc, id)) { read_line(cc); return; }

    if (!strcmp(id, "define")) {
        Macro *m = (Macro *)cc_alloc(cc, sizeof(Macro));
        skip_space(cc, 1);
        read_ident(cc, m->name);
        if (cur(cc) == '(') {        /* function-like: no space before ( */
            adv(cc, 1);
            m->funclike = 1;
            for (;;) {
                skip_space(cc, 1);
                if (cur(cc) == ')') { adv(cc, 1); break; }
                if (m->nparams >= CC_MAXPARAMS)
                    cc_error(cc, "too many macro parameters");
                read_ident(cc, m->params[m->nparams]);
                m->nparams++;
                skip_space(cc, 1);
                if (cur(cc) == ',') { adv(cc, 1); continue; }
            }
        }
        skip_space(cc, 1);
        m->body = read_line(cc);
        m->next = cc->macros;
        cc->macros = m;
        return;
    }
    if (!strcmp(id, "undef")) {
        Macro **pp, *m;
        skip_space(cc, 1);
        read_ident(cc, id);
        for (pp = &cc->macros; (m = *pp) != NULL; pp = &m->next)
            if (!strcmp(m->name, id)) { *pp = m->next; break; }
        read_line(cc);
        return;
    }
    if (!strcmp(id, "include")) {
        /* A quoted include is read from the card. An angled one names a
         * standard header: every function those would declare is already
         * built in, so <stdio.h> and friends are satisfied by doing
         * nothing, which is why a program can use printf without them. */
        char name[CC_MAXNAME];
        int n = 0, term, angled;
        skip_space(cc, 1);
        angled = (cur(cc) == '<');
        term = angled ? '>' : '"';
        if (cur(cc) == '<' || cur(cc) == '"') adv(cc, 1);
        while (cur(cc) && cur(cc) != term && n < CC_MAXNAME - 1) {
            name[n++] = (char)cur(cc);
            adv(cc, 1);
        }
        name[n] = 0;
        if (cur(cc)) adv(cc, 1);
        read_line(cc);
        if (angled || !n || !cc->readfile) return;
        include_file(cc, name);
        return;
    }
    if (!strcmp(id, "ifdef") || !strcmp(id, "ifndef")) {
        int want = (id[2] == 'd');
        int have;
        skip_space(cc, 1);
        read_ident(cc, id);
        have = find_macro(cc, id) != NULL;
        read_line(cc);
        if (cond_depth < 15) cond_depth++;
        cond_taken[cond_depth] = (char)(have == want);
        if (have != want) {
            int r = pp_skip_to_endif(cc, 1);
            if (r == 0) cond_depth--;
            else cond_taken[cond_depth] = 1;
        }
        return;
    }
    if (!strcmp(id, "if")) {
        char *line = read_line(cc);
        const char *p = line;
        long v = pp_expr(cc, &p);
        if (cond_depth < 15) cond_depth++;
        cond_taken[cond_depth] = (char)(v != 0);
        if (!v) {
            int r = pp_skip_to_endif(cc, 1);
            if (r == 0) cond_depth--;
            else if (r == 2) {
                /* #elif: re-evaluate */
                char *l2 = read_line(cc);
                const char *q = l2;
                long v2 = pp_expr(cc, &q);
                cond_taken[cond_depth] = (char)(v2 != 0);
                if (!v2) { if (pp_skip_to_endif(cc, 0) == 0) cond_depth--; }
            } else cond_taken[cond_depth] = 1;
        }
        return;
    }
    if (!strcmp(id, "else") || !strcmp(id, "elif")) {
        read_line(cc);
        if (pp_skip_to_endif(cc, 0) == 0 && cond_depth > 0) cond_depth--;
        return;
    }
    if (!strcmp(id, "endif")) {
        read_line(cc);
        if (cond_depth > 0) cond_depth--;
        return;
    }
    /* #line, #pragma, #error and anything else: ignore the line */
    read_line(cc);
}

/* tiny constant expression evaluator for #if */
static void pp_ws(const char **p) { while (**p == ' ' || **p == '\t') (*p)++; }

static long pp_prim(CC *cc, const char **p)
{
    pp_ws(p);
    if (**p == '(') {
        long v;
        (*p)++;
        v = pp_expr(cc, p);
        pp_ws(p);
        if (**p == ')') (*p)++;
        return v;
    }
    if (**p == '!') { (*p)++; return !pp_prim(cc, p); }
    if (**p == '-') { (*p)++; return -pp_prim(cc, p); }
    if (isdigit((unsigned char)**p)) return strtol(*p, (char **)p, 0);
    if (isalpha((unsigned char)**p) || **p == '_') {
        char id[CC_MAXNAME];
        int n = 0;
        while (isalnum((unsigned char)**p) || **p == '_') {
            if (n < CC_MAXNAME - 1) id[n++] = **p;
            (*p)++;
        }
        id[n] = 0;
        if (!strcmp(id, "defined")) {
            char nm[CC_MAXNAME];
            int k = 0, paren = 0;
            pp_ws(p);
            if (**p == '(') { paren = 1; (*p)++; pp_ws(p); }
            while (isalnum((unsigned char)**p) || **p == '_') {
                if (k < CC_MAXNAME - 1) nm[k++] = **p;
                (*p)++;
            }
            nm[k] = 0;
            if (paren) { pp_ws(p); if (**p == ')') (*p)++; }
            return find_macro(cc, nm) != NULL;
        }
        {
            Macro *m = find_macro(cc, id);
            if (m && m->body) {
                const char *q = m->body;
                return pp_expr(cc, &q);
            }
        }
        return 0;
    }
    return 0;
}

static long pp_mul(CC *cc, const char **p)
{
    long v = pp_prim(cc, p);
    for (;;) {
        pp_ws(p);
        if (**p == '*') { (*p)++; v *= pp_prim(cc, p); }
        else if (**p == '/') { long d; (*p)++; d = pp_prim(cc, p); v = d ? v / d : 0; }
        else if (**p == '%') { long d; (*p)++; d = pp_prim(cc, p); v = d ? v % d : 0; }
        else return v;
    }
}

static long pp_add(CC *cc, const char **p)
{
    long v = pp_mul(cc, p);
    for (;;) {
        pp_ws(p);
        if (**p == '+') { (*p)++; v += pp_mul(cc, p); }
        else if (**p == '-' && (*p)[1] != '-') { (*p)++; v -= pp_mul(cc, p); }
        else return v;
    }
}

static long pp_rel(CC *cc, const char **p)
{
    long v = pp_add(cc, p);
    for (;;) {
        pp_ws(p);
        if ((*p)[0] == '<' && (*p)[1] == '=') { (*p) += 2; v = v <= pp_add(cc, p); }
        else if ((*p)[0] == '>' && (*p)[1] == '=') { (*p) += 2; v = v >= pp_add(cc, p); }
        else if ((*p)[0] == '=' && (*p)[1] == '=') { (*p) += 2; v = v == pp_add(cc, p); }
        else if ((*p)[0] == '!' && (*p)[1] == '=') { (*p) += 2; v = v != pp_add(cc, p); }
        else if (**p == '<' && (*p)[1] != '<') { (*p)++; v = v < pp_add(cc, p); }
        else if (**p == '>' && (*p)[1] != '>') { (*p)++; v = v > pp_add(cc, p); }
        else return v;
    }
}

static long pp_expr(CC *cc, const char **p)
{
    long v = pp_rel(cc, p);
    for (;;) {
        pp_ws(p);
        if ((*p)[0] == '&' && (*p)[1] == '&') { (*p) += 2; v = pp_rel(cc, p) && v; }
        else if ((*p)[0] == '|' && (*p)[1] == '|') { long r; (*p) += 2; r = pp_rel(cc, p); v = v || r; }
        else return v;
    }
}

/* ------------------------------------------------------------------ */
/* escapes and literals                                                */
/* ------------------------------------------------------------------ */

static int escape(CC *cc)
{
    int c = cur(cc);
    adv(cc, 1);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
        int v = 0;
        while (isxdigit(cur(cc))) {
            int d = cur(cc);
            d = isdigit(d) ? d - '0' : (tolower(d) - 'a' + 10);
            v = v * 16 + d;
            adv(cc, 1);
        }
        return v & 0xFF;
    }
    default: return c;
    }
}

/* ------------------------------------------------------------------ */
/* the lexer proper                                                    */
/* ------------------------------------------------------------------ */

static void scan(CC *cc, Token *t)
{
    int c;
    memset(t, 0, sizeof *t);

again:
    skip_space(cc, 0);
    c = cur(cc);
    t->line = cc->nin > 0 ? top(cc)->line : 0;

    if (c == 0) { t->kind = TK_EOF; return; }

    /* a '#' at the start of a line is a directive */
    if (c == '#') {
        InBuf *b = top(cc);
        const char *q = b->p - 1;
        int atbol = 1;
        while (q >= (b->end - (b->end - b->p)) && q > (const char *)0) break;
        /* simpler: treat any '#' not inside a macro body as a directive */
        (void)atbol;
        if (!b->macro) { do_directive(cc); goto again; }
        adv(cc, 1);
        goto again;
    }

    /* numbers */
    if (isdigit(c) || (c == '.' && isdigit(at(cc, 1)))) {
        char buf[64];
        int n = 0, isfloat = 0, base = 10;
        if (c == '0' && (at(cc, 1) == 'x' || at(cc, 1) == 'X')) {
            base = 16;
            buf[n++] = (char)cur(cc); adv(cc, 1);
            buf[n++] = (char)cur(cc); adv(cc, 1);
            while (isxdigit(cur(cc)) && n < 60) { buf[n++] = (char)cur(cc); adv(cc, 1); }
        } else {
            while (n < 60) {
                c = cur(cc);
                if (isdigit(c)) { buf[n++] = (char)c; adv(cc, 1); continue; }
                if (c == '.' && !isfloat) { isfloat = 1; buf[n++] = (char)c; adv(cc, 1); continue; }
                if ((c == 'e' || c == 'E')
                    && (isdigit(at(cc, 1)) || ((at(cc, 1) == '+' || at(cc, 1) == '-')
                                               && isdigit(at(cc, 2))))) {
                    isfloat = 1;
                    buf[n++] = (char)c; adv(cc, 1);
                    buf[n++] = (char)cur(cc); adv(cc, 1);
                    continue;
                }
                break;
            }
        }
        buf[n] = 0;
        /* suffixes */
        {
            int nl = 0, uns = 0;
            while (cur(cc) == 'u' || cur(cc) == 'U' || cur(cc) == 'l' || cur(cc) == 'L'
                   || cur(cc) == 'f' || cur(cc) == 'F') {
                if (cur(cc) == 'f' || cur(cc) == 'F') isfloat = 1;
                if (cur(cc) == 'u' || cur(cc) == 'U') uns = 1;
                if (cur(cc) == 'l' || cur(cc) == 'L') nl++;
                adv(cc, 1);
            }
            if (isfloat) { t->kind = TK_DNUM; t->dval = strtod(buf, NULL); return; }
            t->kind = TK_NUM;
            t->llval = (long long)strtoull(buf, NULL, base);
            t->val = (int)t->llval;
            t->is_uns = uns;
            /* 64 bits if asked for, or if the value simply does not fit */
            t->is_ll = (nl >= 2)
                       || (t->llval > 0x7FFFFFFFLL)
                       || (t->llval < -0x80000000LL);
        }
        return;
    }

    /* identifiers, keywords, macro expansion */
    if (isalpha(c) || c == '_') {
        char id[CC_MAXNAME];
        Macro *m;
        read_ident(cc, id);
        m = find_macro(cc, id);
        if (m && !in_macro(cc, m)) {
            if (m->funclike) {
                /* only expand when an argument list actually follows */
                InBuf *b = top(cc);
                const char *save_p = b->p;
                int save_line = b->line;
                skip_space(cc, 0);
                if (cur(cc) == '(') {
                    char *args[CC_MAXPARAMS];
                    char store[1024];
                    char *body;
                    collect_args(cc, m, args, store, sizeof store);
                    body = expand_params(cc, m, args);
                    push_buf(cc, body, top(cc)->fname, NULL, m);
                    goto again;
                }
                b = top(cc);
                b->p = save_p;
                b->line = save_line;
            } else if (m->body && m->body[0]) {
                push_buf(cc, m->body, top(cc)->fname, NULL, m);
                goto again;
            } else if (m->body) {
                goto again;               /* empty macro */
            }
        }
        t->kind = TK_IDENT;
        strncpy(t->name, id, CC_MAXNAME - 1);
        { int k = keyword_of(id); if (k) { t->kind = TK_KW; t->val = k; } }
        return;
    }

    /* character literal */
    if (c == '\'') {
        int v;
        adv(cc, 1);
        v = cur(cc);
        if (v == '\\') { adv(cc, 1); v = escape(cc); }
        else adv(cc, 1);
        if (cur(cc) == '\'') adv(cc, 1);
        t->kind = TK_NUM;
        t->val = (char)v;          /* plain char is signed */
        t->llval = t->val;
        t->is_ll = t->is_uns = 0;
        return;
    }

    /* string literal, with adjacent concatenation */
    if (c == '"') {
        char buf[1024];
        int n = 0;
        for (;;) {
            adv(cc, 1);                 /* opening quote */
            while (cur(cc) && cur(cc) != '"') {
                int ch = cur(cc);
                if (ch == '\\') { adv(cc, 1); ch = escape(cc); }
                else adv(cc, 1);
                if (n < (int)sizeof buf - 1) buf[n++] = (char)ch;
            }
            if (cur(cc)) adv(cc, 1);    /* closing quote */
            skip_space(cc, 0);
            if (cur(cc) != '"') break;
        }
        buf[n] = 0;
        t->kind = TK_STR;
        t->str = cc_strdup(cc, buf, n);
        t->slen = n;
        return;
    }

    /* punctuation */
    t->kind = TK_PUNCT;
    {
        int c1 = at(cc, 1), c2 = at(cc, 2);
        if (c == '.' && c1 == '.' && c2 == '.') { adv(cc, 3); t->val = P_ELLIPSIS; return; }
        if (c == '<' && c1 == '<' && c2 == '=') { adv(cc, 3); t->val = P_SHLEQ; return; }
        if (c == '>' && c1 == '>' && c2 == '=') { adv(cc, 3); t->val = P_SHREQ; return; }
        if (c == '-' && c1 == '>') { adv(cc, 2); t->val = P_ARROW; return; }
        if (c == '+' && c1 == '+') { adv(cc, 2); t->val = P_INC; return; }
        if (c == '-' && c1 == '-') { adv(cc, 2); t->val = P_DEC; return; }
        if (c == '<' && c1 == '<') { adv(cc, 2); t->val = P_SHL; return; }
        if (c == '>' && c1 == '>') { adv(cc, 2); t->val = P_SHR; return; }
        if (c == '<' && c1 == '=') { adv(cc, 2); t->val = P_LE; return; }
        if (c == '>' && c1 == '=') { adv(cc, 2); t->val = P_GE; return; }
        if (c == '=' && c1 == '=') { adv(cc, 2); t->val = P_EQ; return; }
        if (c == '!' && c1 == '=') { adv(cc, 2); t->val = P_NE; return; }
        if (c == '&' && c1 == '&') { adv(cc, 2); t->val = P_ANDAND; return; }
        if (c == '|' && c1 == '|') { adv(cc, 2); t->val = P_OROR; return; }
        if (c == '+' && c1 == '=') { adv(cc, 2); t->val = P_ADDEQ; return; }
        if (c == '-' && c1 == '=') { adv(cc, 2); t->val = P_SUBEQ; return; }
        if (c == '*' && c1 == '=') { adv(cc, 2); t->val = P_MULEQ; return; }
        if (c == '/' && c1 == '=') { adv(cc, 2); t->val = P_DIVEQ; return; }
        if (c == '%' && c1 == '=') { adv(cc, 2); t->val = P_MODEQ; return; }
        if (c == '&' && c1 == '=') { adv(cc, 2); t->val = P_ANDEQ; return; }
        if (c == '|' && c1 == '=') { adv(cc, 2); t->val = P_OREQ; return; }
        if (c == '^' && c1 == '=') { adv(cc, 2); t->val = P_XOREQ; return; }
        adv(cc, 1);
        t->val = c;
    }
}

void lex_init(CC *cc, const char *src, const char *fname)
{
    cc->nin = 0;
    cond_depth = 0;
    push_buf(cc, src, fname, NULL, NULL);
    cc->has_ahead = 0;
    scan(cc, &cc->tok);
}

void lex_next(CC *cc)
{
    if (cc->has_ahead) { cc->tok = cc->ahead; cc->has_ahead = 0; return; }
    scan(cc, &cc->tok);
}

Token *lex_peek(CC *cc)
{
    if (!cc->has_ahead) { scan(cc, &cc->ahead); cc->has_ahead = 1; }
    return &cc->ahead;
}
