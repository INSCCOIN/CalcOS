/* CalcOS evaluator: environments, calls, indexing. */
#include "calc.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cprog.h"

#define MAX_DEPTH 48

/* ------------------------------------------------------------------ */
/* environment                                                        */
/* ------------------------------------------------------------------ */

Var *cos_lookup(Interp *in, const char *name)
{
    Var *v;
    if (in->frame)
        for (v = in->frame->locals; v; v = v->next)
            if (strcmp(v->name, name) == 0) return v;
    for (v = in->globals; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

void cos_setvar(Interp *in, const char *name, Value val)
{
    Var *v;
    /* assignment inside a function body writes the local if one exists */
    if (in->frame)
        for (v = in->frame->locals; v; v = v->next)
            if (strcmp(v->name, name) == 0) { v->v = val; return; }
    for (v = in->globals; v; v = v->next)
        if (strcmp(v->name, name) == 0) {
            v_perm_free(&v->v);
            v->v = v_perm_dup(val);
            return;
        }
    v = (Var *)calloc(1, sizeof(Var));
    if (!v) cos_err(in, "out of memory storing '%s'", name);
    cos_setname(v->name, name);
    v->v = v_perm_dup(val);
    v->next = in->globals;
    in->globals = v;
}

FuncDef *cos_lookup_fn(Interp *in, const char *name)
{
    FuncDef *f;
    for (f = in->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static void define_fn(Interp *in, FuncDef *src)
{
    FuncDef *f, *prev = NULL, *nf;
    for (f = in->funcs; f; prev = f, f = f->next)
        if (strcmp(f->name, src->name) == 0) {
            nf = cos_fn_clone(src);
            if (!nf) cos_err(in, "out of memory");
            nf->next = f->next;
            if (prev) prev->next = nf; else in->funcs = nf;
            cos_fn_free(f);
            return;
        }
    nf = cos_fn_clone(src);
    if (!nf) cos_err(in, "out of memory");
    nf->next = in->funcs;
    in->funcs = nf;
}

/* ------------------------------------------------------------------ */
/* calls                                                              */
/* ------------------------------------------------------------------ */

/* What a builtin does with an argument that carries a unit.
 *
 * Most functions are meaningless on a quantity: sin(5 m) has no reading,
 * and silently discarding the metres would be the worst of the three
 * possible answers. A handful do make sense, either because they are
 * indifferent to the unit or because they transform it in a known way, so
 * those are named here and everything else is refused. */
typedef enum { U_PLAIN, U_KEEP, U_DROP, U_SQRT, U_SQUARE } UnitPolicy;

static UnitPolicy unit_policy(const char *name)
{
    static const char *keep[] = {        /* result carries the same unit */
        "abs", "min", "max", "floor", "ceil", "round", "trunc", "re", "im",
        "conj", "sum", "mean", "median", "sort", "std", "cumsum", "mid",
        "inf", "sup", "wid", "linspace", "print", "diff", NULL
    };
    static const char *drop[] = {        /* result is a plain number */
        "sign", "typeof", "numel", "size", "isnan", "isinf", "conv",
        "unit", "to", "corr", "fit", NULL
    };
    int i;
    for (i = 0; keep[i]; i++) if (!strcmp(keep[i], name)) return U_KEEP;
    for (i = 0; drop[i]; i++) if (!strcmp(drop[i], name)) return U_DROP;
    if (!strcmp(name, "sqrt")) return U_SQRT;
    if (!strcmp(name, "var")) return U_SQUARE;
    return U_PLAIN;
}

/* Strip the unit from the arguments, returning the one the result should
 * carry. Errors if the function cannot accept a quantity at all. */
static Unit take_units(Interp *in, const char *name, Value *args, int nargs)
{
    UnitPolicy pol;
    Unit u = unit_none();
    int i, any = 0, k;

    for (i = 0; i < nargs; i++) if (!unit_is_none(args[i].u)) { any = 1; break; }
    if (!any) return u;

    pol = unit_policy(name);
    u = args[0].u;
    if (pol == U_PLAIN) {
        char us[32];
        unit_str(u, us, sizeof us);
        cos_err(in, "%s needs a plain number, not %s", name, us);
    }
    if (pol == U_KEEP) {
        /* every argument that has one must agree, or the answer is
         * meaningless: min(1 m, 2 s) has no smaller */
        for (i = 1; i < nargs; i++)
            if (!unit_is_none(args[i].u) && !unit_same(args[i].u, u))
                cos_err(in, "%s needs its arguments to be the same kind", name);
    }
    for (i = 0; i < nargs; i++) args[i].u = unit_none();

    if (pol == U_DROP) return unit_none();
    if (pol == U_SQUARE) return unit_mul(u, u);
    if (pol == U_SQRT) {
        for (k = 0; k < UNIT_DIMS; k++) {
            if (u.d[k] % 2) cos_err(in, "sqrt needs even powers in the unit");
            u.d[k] = (signed char)(u.d[k] / 2);
        }
    }
    return u;
}

Value cos_call(Interp *in, FuncDef *f, Value *args, int nargs)
{
    Frame fr;
    Var  slots[COS_MAXPARAM];
    Value r;
    int i;

    if (!f->body) {                       /* builtin reference */
        const Builtin *b = cos_builtin(f->name);
        if (!b) cos_err(in, "unknown function '%s'", f->name);
        if (nargs < b->min || nargs > b->max)
            cos_err(in, "%s expects %d..%d arguments, got %d",
                    b->name, b->min, b->max, nargs);
        {
            Unit u = take_units(in, b->name, args, nargs);
            Value r = b->fn(in, args, nargs);
            if (!unit_is_none(u)) r.u = u;
            return r;
        }
    }
    if (nargs != f->nparams)
        cos_err(in, "%s expects %d argument%s, got %d",
                f->name[0] ? f->name : "function", f->nparams,
                f->nparams == 1 ? "" : "s", nargs);
    if (++in->depth > MAX_DEPTH) {
        in->depth--;
        cos_err(in, "recursion too deep (limit %d)", MAX_DEPTH);
    }
    memset(slots, 0, sizeof slots);
    fr.locals = NULL;
    fr.prev = in->frame;
    for (i = 0; i < nargs; i++) {
        cos_setname(slots[i].name, f->params[i]);
        slots[i].v = args[i];
        slots[i].next = fr.locals;
        fr.locals = &slots[i];
    }
    in->frame = &fr;
    r = cos_eval(in, f->body);
    in->frame = fr.prev;
    in->depth--;
    return r;
}

Value cos_call_value(Interp *in, Value fv, Value *args, int nargs)
{
    if (fv.k != K_FUN || !fv.f) cos_err(in, "value is not callable");
    return cos_call(in, fv.f, args, nargs);
}

double cos_call_real(Interp *in, Value fv, double x)
{
    Value a = v_real(x);
    Value r = cos_call_value(in, fv, &a, 1);
    return v_realnum(in, r);
}

/* ------------------------------------------------------------------ */
/* matrices                                                           */
/* ------------------------------------------------------------------ */

static Value build_range(Interp *in, Node *n)
{
    double a = v_realnum(in, cos_eval(in, n->a));
    double step = 1.0, b;
    Value r;
    int i, cnt;
    if (n->c) {
        step = v_realnum(in, cos_eval(in, n->b));
        b = v_realnum(in, cos_eval(in, n->c));
    } else {
        b = v_realnum(in, cos_eval(in, n->b));
    }
    if (step == 0.0) cos_err(in, "range step is zero");
    cnt = (int)floor((b - a) / step + 1e-10) + 1;
    if (cnt < 0) cnt = 0;
    if (cnt > COS_MAXELEM) cos_err(in, "range too long (%d elements)", cnt);
    r = v_mat(in, 1, cnt);
    for (i = 0; i < cnt; i++) r.m[i] = cx_mk(a + (double)i * step, 0.0);
    return r;
}

static Value build_matrix(Interp *in, Node *n)
{
    /* evaluate every element first, then concatenate like-shaped blocks */
    Value rowsv[32];
    int i, j, nrows = n->nlist, cols = -1, totalrows = 0;
    Value out;
    if (nrows == 0) return v_mat(in, 0, 0);
    if (nrows > 32) cos_err(in, "matrix literal has too many rows");

    for (i = 0; i < nrows; i++) {
        Node *row = n->list[i];
        Value cells[32];
        int ncell = row->nlist, c = 0, r = 1, k;
        if (ncell > 32) cos_err(in, "matrix row has too many entries");
        for (j = 0; j < ncell; j++) {
            cells[j] = cos_eval(in, row->list[j]);
            if (cells[j].k == K_MAT) {
                if (j == 0) r = cells[j].rows;
                else if (cells[j].rows != r)
                    cos_err(in, "row %d: block heights differ", i + 1);
                c += cells[j].cols;
            } else {
                if (j == 0) r = 1;
                else if (r != 1) cos_err(in, "row %d: block heights differ", i + 1);
                c += 1;
            }
        }
        rowsv[i] = v_mat(in, r, c);
        c = 0;
        for (j = 0; j < ncell; j++) {
            if (cells[j].k == K_MAT) {
                int rr, cc;
                for (rr = 0; rr < cells[j].rows; rr++)
                    for (cc = 0; cc < cells[j].cols; cc++)
                        rowsv[i].m[rr * rowsv[i].cols + c + cc] =
                            cells[j].m[rr * cells[j].cols + cc];
                c += cells[j].cols;
            } else {
                for (k = 0; k < r; k++)
                    rowsv[i].m[k * rowsv[i].cols + c] = v_scalar(in, cells[j]);
                c += 1;
            }
        }
        if (cols < 0) cols = rowsv[i].cols;
        else if (cols != rowsv[i].cols)
            cos_err(in, "row %d has %d columns, expected %d",
                    i + 1, rowsv[i].cols, cols);
        totalrows += rowsv[i].rows;
    }
    out = v_mat(in, totalrows, cols);
    {
        int dst = 0;
        for (i = 0; i < nrows; i++) {
            int rr;
            for (rr = 0; rr < rowsv[i].rows; rr++, dst++)
                memcpy(&out.m[dst * cols], &rowsv[i].m[rr * cols],
                       (size_t)cols * sizeof(cx));
        }
    }
    return out;
}

static int index_of(Interp *in, Value idx, int limit, const char *what)
{
    double d = v_realnum(in, idx);
    int k;
    if (d != floor(d)) cos_err(in, "%s index must be an integer", what);
    k = (int)d;
    if (k < 1 || k > limit)
        cos_err(in, "%s index %d out of range 1..%d", what, k, limit);
    return k - 1;
}

static Value matrix_index(Interp *in, Value m, Value *idx, int nidx)
{
    if (nidx == 1) {
        if (idx[0].k == K_MAT) {           /* gather */
            Value r = v_mat(in, 1, idx[0].rows * idx[0].cols);
            int i, n = idx[0].rows * idx[0].cols;
            for (i = 0; i < n; i++)
                r.m[i] = m.m[index_of(in, v_num(idx[0].m[i]),
                                      m.rows * m.cols, "linear")];
            return r;
        }
        return v_num(m.m[index_of(in, idx[0], m.rows * m.cols, "linear")]);
    }
    if (nidx == 2) {
        int i = index_of(in, idx[0], m.rows, "row");
        int j = index_of(in, idx[1], m.cols, "column");
        return v_num(m.m[i * m.cols + j]);
    }
    cos_err(in, "matrices take one or two indices");
    return v_nil();
}

/* ------------------------------------------------------------------ */
/* eval                                                               */
/* ------------------------------------------------------------------ */

static Value eval_call(Interp *in, Node *n)
{
    Value args[COS_MAXARGS];
    Var *var;
    int i;

    /* indexing takes priority over calling when the name holds a matrix */
    if (n->a && n->a->t == N_VAR) {
        var = cos_lookup(in, n->a->name);
        if (var && var->v.k == K_MAT) {
            for (i = 0; i < n->nlist; i++) args[i] = cos_eval(in, n->list[i]);
            return matrix_index(in, var->v, args, n->nlist);
        }
        if (var && var->v.k == K_FUN) {
            for (i = 0; i < n->nlist; i++) args[i] = cos_eval(in, n->list[i]);
            return cos_call_value(in, var->v, args, n->nlist);
        }
        {
            FuncDef *f = cos_lookup_fn(in, n->a->name);
            if (f) {
                for (i = 0; i < n->nlist; i++) args[i] = cos_eval(in, n->list[i]);
                return cos_call(in, f, args, n->nlist);
            }
        }
        {
            const Builtin *b = cos_builtin(n->a->name);
            if (b) {
                if (n->nlist < b->min || n->nlist > b->max)
                    cos_err(in, "%s expects %d..%d arguments, got %d",
                            b->name, b->min, b->max, n->nlist);
                for (i = 0; i < n->nlist; i++) args[i] = cos_eval(in, n->list[i]);
                {
                    Unit u = take_units(in, b->name, args, n->nlist);
                    Value r = b->fn(in, args, n->nlist);
                    if (!unit_is_none(u)) r.u = u;
                    return r;
                }
            }
        }
        /* a function defined by the loaded C program */
        if (cprog_loaded()) {
            double cargs[4], r = 0.0;
            int rc;
            if (n->nlist > 4)
                cos_err(in, "C functions take at most 4 arguments here");
            for (i = 0; i < n->nlist; i++) {
                args[i] = cos_eval(in, n->list[i]);
                cargs[i] = v_realnum(in, args[i]);
            }
            rc = cprog_call(n->a->name, cargs, n->nlist, &r);
            /* as with run: a function that left a tone playing does not get
             * to keep the buzzer */
            plat_tone_off();
            if (rc == 0) return v_real(r);
            if (rc == -3)
                cos_err(in, "C function '%s' takes a different number of arguments",
                        n->a->name);
            if (rc == -2)
                cos_err(in, "C function '%s' has a signature the calculator "
                        "cannot call (use all-double or all-int parameters)",
                        n->a->name);
            if (rc == -4)
                cos_err(in, "compiled code only runs on the device");
        }
        cos_err(in, "unknown name '%s'", n->a->name);
    }

    /* callee is an expression */
    {
        Value f = cos_eval(in, n->a);
        if (f.k == K_MAT) {
            for (i = 0; i < n->nlist; i++) args[i] = cos_eval(in, n->list[i]);
            return matrix_index(in, f, args, n->nlist);
        }
        for (i = 0; i < n->nlist; i++) args[i] = cos_eval(in, n->list[i]);
        return cos_call_value(in, f, args, n->nlist);
    }
}

static Value eval_assign(Interp *in, Node *n)
{
    Value val = cos_eval(in, n->b);
    Node *lhs = n->a;

    if (lhs->t == N_VAR) {
        cos_setvar(in, lhs->name, val);
        return val;
    }
    if (lhs->t == N_CALL && lhs->a && lhs->a->t == N_VAR) {
        Var *var = cos_lookup(in, lhs->a->name);
        Value idx[2];
        int i, k;
        if (!var || var->v.k != K_MAT)
            cos_err(in, "'%s' is not a matrix", lhs->a->name);
        for (i = 0; i < lhs->nlist && i < 2; i++) idx[i] = cos_eval(in, lhs->list[i]);
        if (lhs->nlist == 1)
            k = index_of(in, idx[0], var->v.rows * var->v.cols, "linear");
        else if (lhs->nlist == 2)
            k = index_of(in, idx[0], var->v.rows, "row") * var->v.cols
              + index_of(in, idx[1], var->v.cols, "column");
        else
            { cos_err(in, "matrices take one or two indices"); return v_nil(); }
        var->v.m[k] = v_scalar(in, val);   /* mutates the stored copy */
        return var->v;
    }
    cos_err(in, "left side of '=' is not assignable");
    return v_nil();
}

Value cos_eval(Interp *in, Node *n)
{
    if (!n) return v_nil();
    if (++in->steps > 40000000u) cos_err(in, "evaluation step limit reached");

    switch (n->t) {
    case N_NUM: return v_num(n->num);
    case N_STR: return v_str(in, n->str ? n->str : "");
    case N_VAR: {
        Var *v = cos_lookup(in, n->name);
        FuncDef *f;
        if (v) return v->v;
        f = cos_lookup_fn(in, n->name);
        if (f) return v_fun(f);
        {   /* a unit or a physical constant used as a value */
            double f;
            Unit u;
            if (unit_lookup(n->name, &f, &u)) {
                Value r = v_real(f);
                r.u = u;
                return r;
            }
            if (const_lookup(n->name, &f, &u)) {
                Value r = v_real(f);
                r.u = u;
                return r;
            }
        }
        if (cos_builtin(n->name)) {
            /* a bare builtin name becomes a callable reference */
            FuncDef *b = (FuncDef *)cos_alloc(in, sizeof(FuncDef));
            cos_setname(b->name, n->name);
            b->body = NULL;
            b->nparams = -1;
            return v_fun(b);
        }
        cos_err(in, "undefined variable '%s'", n->name);
        break;
    }
    case N_BIN:
        return v_binop(in, n->op, cos_eval(in, n->a), cos_eval(in, n->b));
    case N_UN:
    case N_POST:
        return v_unop(in, n->op, cos_eval(in, n->a));
    case N_MATRIX: return build_matrix(in, n);
    case N_RANGE:  return build_range(in, n);
    case N_CALL:   return eval_call(in, n);
    case N_ASSIGN: return eval_assign(in, n);
    case N_LAMBDA: return v_fun(n->fn);
    case N_FUNCDEF:
        define_fn(in, n->fn);
        return v_str(in, "function defined");
    default:
        cos_err(in, "cannot evaluate node");
    }
    return v_nil();
}
