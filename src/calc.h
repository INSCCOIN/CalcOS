/* CalcOS - a floating-point laboratory OS for the ClockworkPi PicoCalc
 * Core header: value model, interpreter state, platform interface.
 * C11, freestanding-friendly: depends only on libm + a tiny platform HAL.
 */
#ifndef COS_H
#define COS_H

#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdarg.h>

#define COS_VERSION   "2.2"
#define COS_MAXNAME   24
#define COS_MAXPARAM  4
#define COS_MAXARGS   8
#define COS_MAXELEM   4096   /* matrix element ceiling (RAM guard) */

/* bounded name copy (names are already capped by the lexer) */
static inline void cos_setname(char *dst, const char *src)
{
    unsigned i = 0;
    while (src[i] && i < (unsigned)(COS_MAXNAME - 1)) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E  2.71828182845904523536
#endif

/* ------------------------------------------------------------------ */
/* numeric types                                                      */
/* ------------------------------------------------------------------ */

/* A unit is the exponents of the seven SI base units: metre, kilogram,
 * second, ampere, kelvin, mole, candela. All zero means dimensionless,
 * which is what an ordinary number is, so units cost nothing when they are
 * not used. Values are always held in base SI units. */
#define UNIT_DIMS 7
typedef struct { signed char d[UNIT_DIMS]; } Unit;

Unit   unit_none(void);
int    unit_is_none(Unit u);
int    unit_same(Unit a, Unit b);
Unit   unit_mul(Unit a, Unit b);
Unit   unit_div(Unit a, Unit b);
Unit   unit_pow(Unit a, int n);
void   unit_str(Unit u, char *out, size_t cap);
int    unit_lookup(const char *name, double *factor, Unit *u);
int    unit_parse(const char *text, double *factor, Unit *u);
int    unit_count(void);
const char *unit_name(int i);

int    const_count(void);
int    const_lookup(const char *name, double *value, Unit *u);
int    const_at(int i, const char **name, double *value, Unit *u,
                const char **what, const char **unit_text);

typedef struct { double re, im; } cx;   /* complex double            */
typedef struct { double hi, lo; } dd;   /* double-double, ~32 digits */
typedef struct { double lo, hi; } iv;   /* rigorous interval         */

/* complex */
cx     cx_mk(double re, double im);
cx     cx_add(cx a, cx b);
cx     cx_sub(cx a, cx b);
cx     cx_mul(cx a, cx b);
cx     cx_div(cx a, cx b);
cx     cx_neg(cx a);
double cx_abs(cx a);
double cx_arg(cx a);
cx     cx_conj(cx a);
cx     cx_exp(cx a);
cx     cx_log(cx a);
cx     cx_sqrt(cx a);
cx     cx_pow(cx a, cx b);
cx     cx_sin(cx a);
cx     cx_cos(cx a);
cx     cx_tan(cx a);
cx     cx_sinh(cx a);
cx     cx_cosh(cx a);
cx     cx_tanh(cx a);
cx     cx_asin(cx a);
cx     cx_acos(cx a);
cx     cx_atan(cx a);
int    cx_isreal(cx a);

/* double-double */
dd     dd_from(double x);
dd     dd_mk(double hi, double lo);
double dd_to(dd a);
dd     dd_add(dd a, dd b);
dd     dd_sub(dd a, dd b);
dd     dd_mul(dd a, dd b);
dd     dd_div(dd a, dd b);
dd     dd_neg(dd a);
dd     dd_sqrt(dd a);
dd     dd_exp(dd a);
dd     dd_log(dd a);
dd     dd_sin(dd a);
dd     dd_cos(dd a);
dd     dd_powi(dd a, int n);
int    dd_cmp(dd a, dd b);
void   dd_str(dd a, int digits, char *out, size_t n);
extern dd DD_PI, DD_E, DD_LN2, DD_PHI;
void dd_init_consts(void);

/* interval (outward-rounded, rigorous) */
iv     iv_mk(double lo, double hi);
iv     iv_point(double x);
iv     iv_add(iv a, iv b);
iv     iv_sub(iv a, iv b);
iv     iv_mul(iv a, iv b);
iv     iv_div(iv a, iv b);   /* returns [-inf,inf] if 0 in b */
iv     iv_neg(iv a);
iv     iv_sqrt(iv a);
iv     iv_exp(iv a);
iv     iv_log(iv a);
iv     iv_sin(iv a);
iv     iv_cos(iv a);
iv     iv_powi(iv a, int n);
double iv_mid(iv a);
double iv_wid(iv a);

/* special functions (prefixed to avoid libm/POSIX collisions) */
double cos_gamma(double x);
double cos_lgamma(double x);
double cos_digamma(double x);
double cos_erf(double x);
double cos_erfc(double x);
double cos_beta(double a, double b);
double cos_zeta(double s);
double cos_j0(double x);
double cos_j1(double x);
double cos_y0(double x);
double cos_y1(double x);
double cos_ei(double x);
double cos_li(double x);

/* IEEE-754 inspection */
double cos_ulp(double x);
void   cos_bits(double x, char *out, size_t n);
double cos_relerr(double a, double b);
double cos_ulpdiff(double a, double b);
double cos_prng(void);
void   cos_srand(uint64_t seed);

/* compensated primitives */
double cos_nsum(const double *v, int n);          /* Neumaier summation */
double cos_ndot(const double *a, const double *b, int n);
double cos_horner(const double *p, int n, double x); /* fma-based */

/* ------------------------------------------------------------------ */
/* arena allocator                                                     */
/* ------------------------------------------------------------------ */

typedef struct Block Block;
typedef struct { Block *head; size_t total, peak; } Arena;

void  *arena_alloc(Arena *a, size_t n);
void   arena_reset(Arena *a);
void   arena_free(Arena *a);

/* ------------------------------------------------------------------ */
/* values                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    K_NIL = 0,
    K_NUM,      /* complex double          */
    K_DD,       /* double-double           */
    K_IV,       /* interval                */
    K_MAT,      /* complex matrix          */
    K_STR,
    K_BIG,      /* arbitrary-precision integer */
    K_FUN
} VKind;

typedef struct Node Node;
typedef struct FuncDef FuncDef;

struct Big;

typedef struct {
    VKind  k;
    Unit   u;           /* dimensionless unless units are in play */
    struct Big *big;    /* K_BIG; owned by the interpreter's big list */
    cx     n;
    dd     d;
    iv     v;
    int    rows, cols;
    cx    *m;           /* rows*cols, row-major */
    char  *s;
    FuncDef *f;
} Value;

struct FuncDef {
    char   name[COS_MAXNAME];
    char   params[COS_MAXPARAM][COS_MAXNAME];
    int    nparams;
    Node  *body;
    int    perm;        /* allocated with malloc rather than arena */
    struct FuncDef *next;
};

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_BIN, N_UN, N_POST,
    N_MATRIX, N_ROW, N_INDEX, N_ASSIGN, N_FUNCDEF, N_LAMBDA, N_RANGE
} NType;

enum {
    OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_MOD,
    OP_EMUL, OP_EDIV, OP_EPOW,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_NEG, OP_NOT, OP_FACT, OP_TRANS, OP_AND, OP_OR
};

struct Node {
    NType  t;
    int    op;
    cx     num;
    char   name[COS_MAXNAME];
    char  *str;
    Node  *a, *b, *c;
    Node **list;
    int    nlist;
    FuncDef *fn;
};

/* ------------------------------------------------------------------ */
/* interpreter                                                         */
/* ------------------------------------------------------------------ */

typedef struct Var {
    char  name[COS_MAXNAME];
    Value v;
    struct Var *next;
} Var;

struct BigNode;

typedef struct Frame {
    Var *locals;
    struct Frame *prev;
} Frame;

typedef struct Interp {
    Arena   arena;          /* reset after every input line */
    void   *bigs;           /* big integers made while evaluating a line */
    Var    *globals;
    FuncDef*funcs;
    Frame  *frame;
    jmp_buf jb;
    int     jb_set;
    char    err[192];
    /* settings */
    int     deg;            /* angle mode: 0 rad, 1 deg */
    int     digits;         /* display significant digits (1..17) */
    int     ddigits;        /* digits used for dd display (1..34)  */
    int     verbose_fp;     /* show ulp/hex annotation on results   */
    int     depth;          /* recursion guard */
    uint32_t steps;         /* evaluation step counter */
} Interp;

void   cos_init(Interp *in);
void   cos_shutdown(Interp *in);
void   cos_err(Interp *in, const char *fmt, ...);
void  *cos_alloc(Interp *in, size_t n);
char  *cos_strdup_arena(Interp *in, const char *s);

/* Wrap a big integer as a value. The interpreter takes ownership and frees
 * it when the line ends; storing it in a variable copies it first. */
Value  v_big(Interp *in, struct Big *b);
void   cos_bigs_release(Interp *in);

/* values */
Value  v_nil(void);
Value  v_num(cx z);
Value  v_real(double x);
Value  v_dd(dd d);
Value  v_iv(iv i);
Value  v_str(Interp *in, const char *s);
Value  v_fun(FuncDef *f);
Value  v_mat(Interp *in, int rows, int cols);
Value  v_binop(Interp *in, int op, Value a, Value b);
Value  v_unop(Interp *in, int op, Value a);
cx     v_scalar(Interp *in, Value a);       /* coerce to complex */
double v_realnum(Interp *in, Value a);      /* coerce to real, error if complex */
int    v_truth(Interp *in, Value a);
void   v_format(Interp *in, Value a, char *out, size_t n);
void   v_print(Interp *in, Value a);
Value  v_perm_dup(Value a);
void   v_perm_free(Value *a);
#define V_AT(v, i, j) ((v).m[(i) * (v).cols + (j)])

/* parser */
Node  *cos_parse(Interp *in, const char *src);

/* AST deep copy into malloc'd storage (for definitions that outlive a line) */
Node     *cos_node_clone(Node *n);
void      cos_node_free(Node *n);
FuncDef  *cos_fn_clone(FuncDef *f);
void      cos_fn_free(FuncDef *f);

/* eval */
Value  cos_eval(Interp *in, Node *n);
Value  cos_call(Interp *in, FuncDef *f, Value *args, int nargs);
Value  cos_call_value(Interp *in, Value fv, Value *args, int nargs);
double cos_call_real(Interp *in, Value fv, double x);
Var   *cos_lookup(Interp *in, const char *name);
void   cos_setvar(Interp *in, const char *name, Value v);
FuncDef *cos_lookup_fn(Interp *in, const char *name);

/* builtins */
typedef Value (*BuiltinFn)(Interp *in, Value *a, int n);
typedef struct {
    const char *name;
    int  min, max;
    BuiltinFn fn;
    const char *help;
} Builtin;
const Builtin *cos_builtin(const char *name);
const Builtin *cos_builtin_table(int *count);

/* the command line: editing, history and completion */
int    cos_readline(Interp *in, char *buf, int cap, const char *prompt);
void   cos_history_add(const char *line);
int    cos_history_count(void);
const char *cos_history_at(int i);
void   cos_history_clear(void);
const char *cos_prompt(void);
const char *const *cos_command_words(void);

/* shell */
void   cos_shell_run(Interp *in);
void   cos_run_startup(Interp *in);     /* /startup.txt and /autoexec.c */
void   cos_load_boot_prog(void);        /* a program kept in flash as "boot" */
int    cos_shell_line(Interp *in, char *line);   /* 0 = continue, 1 = exit */
void   cos_banner(void);

/* ------------------------------------------------------------------ */
/* platform HAL - implemented per target                               */
/* ------------------------------------------------------------------ */

void   plat_init(void);
void   plat_putc(char c);
/* Draw whatever plat_putc has written but not yet shown. Characters are
 * buffered by row, so anything that echoes one at a time must call this
 * before it waits for the next one. */
void   plat_flush(void);
void   plat_puts(const char *s);
int    plat_getc(void);                 /* blocking, returns key code   */
int    plat_readline(char *buf, size_t n, const char *prompt);
int    plat_cols(void);
int    plat_rows(void);
size_t plat_free_mem(void);
void   plat_poll(void);

/* Key codes above the ASCII range. These are part of the platform contract
 * rather than the board's own business: the line editor is shared, so it
 * has to name the arrow keys without knowing what hardware sent them. */
enum {
    KEY_F1 = 0x81, KEY_F2 = 0x82, KEY_F3 = 0x83, KEY_F4 = 0x84, KEY_F5 = 0x85,
    KEY_F6 = 0x86, KEY_F7 = 0x87, KEY_F8 = 0x88, KEY_F9 = 0x89, KEY_F10 = 0x8A,
    KEY_INS = 0xD1,
    KEY_ESC = 0xB1, KEY_LEFT = 0xB4, KEY_UP = 0xB5, KEY_DOWN = 0xB6,
    KEY_RIGHT = 0xB7, KEY_BREAK = 0xD0, KEY_HOME = 0xD2, KEY_DEL = 0xD4,
    KEY_END = 0xD5, KEY_PGUP = 0xD6, KEY_PGDN = 0xD7, KEY_CAPS = 0xC1
};
enum { MOD_SHIFT_L = 3, MOD_SHIFT_R = 5, MOD_ALT = 8, MOD_CTRL = 16 };

/* Non-blocking keyboard, for compiled programs that drive their own loop.
 * plat_getkey returns the next key or -1; it never waits. */
int    plat_key_ready(void);
int    plat_getkey(void);

/* Monotonic time since boot, and a sleep that still services the console. */
uint32_t plat_millis(void);
uint64_t plat_micros(void);
void     plat_delay_ms(int ms);

/* Square-wave tone on the PicoCalc's buzzer. ms <= 0 starts the tone and
 * returns at once; plat_tone_off stops it. */
void   plat_tone(int hz, int ms);
void   plat_tone_off(void);

/* Power. plat_reboot restarts the board; plat_poweroff asks the keyboard
 * controller to cut power and halts if it cannot. */
void   plat_reboot(void);
void   plat_poweroff(void);

/* storage: a small stdio-alike over stdio (host) or the FAT driver (device) */
void  *plat_fopen(const char *path, const char *mode);
int    plat_fgets(void *f, char *buf, size_t n);
int    plat_fputs(void *f, const char *s);
int    plat_fread(void *f, void *buf, int len);
int    plat_fwrite(void *f, const void *buf, int len);
int    plat_fseek(void *f, long off, int whence);
long   plat_ftell(void *f);
long   plat_fsize(void *f);
int    plat_feof(void *f);
void   plat_fclose(void *f);
int    plat_remove(const char *path);
int    plat_listdir(const char *path, char *out, size_t n);
/* whole-file write; the SD writer only accepts 8.3 names */
int    plat_write_file(const char *path, const void *data, int len);

/* The second core.
 *
 * plat_par_for runs fn(i, ctx) for every i below n, splitting the indices
 * between the two cores and returning when both have finished. The body
 * must be independent of the other indices and must not allocate, touch
 * the interpreter, or write to the flash store: core1 runs from flash, and
 * a flash write turns the XIP window off underneath it.
 *
 * It is deliberately a parallel loop and not threads. Threads would mean
 * making the whole calculator re-entrant for the sake of a part with no
 * cache coherency to speak of; a loop whose iterations are independent is
 * the shape of nearly all the slow work here, and it is safe by
 * construction. */
void   plat_par_for(int n, void (*fn)(int i, void *ctx), void *ctx);
int    plat_par_cores(void);     /* 1 if the second core is unavailable */
int    plat_par_active(void);    /* true while a parallel loop is running */

/* A store in flash, outside SRAM.
 *
 * There is no MMU on this part, so RAM cannot be paged behind a program's
 * back. What can be done is better in one way and worse in another: flash
 * is memory-mapped, so plat_store_read hands back a direct pointer that
 * costs no RAM and reads at cache speed, while writing erases a whole page
 * and wears the part, so it is for saving, not for scratch.
 */
int         plat_store_pages(void);        /* 0 if there is no store    */
int         plat_store_page_size(void);
const void *plat_store_read(int page);     /* direct pointer, or NULL   */
int         plat_store_write(int page, const void *data, int len);
int         plat_store_erase(int page);

/* Clear the console, and scroll the saved output back or forward by a
 * screenful. A platform without scrollback may ignore the second. */
void   plat_clear_screen(void);
void   plat_scrollback(int screens);

/* graphics overlay, used by plot() and by compiled programs */
void   plat_gfx_clear(void);
void   plat_gfx_raw_point(int x, int y, unsigned rgb);   /* no clipping */
void   plat_gfx_point(int x, int y, unsigned rgb);

/* A clip rectangle every drawing call respects, so text and sprites can be
 * kept inside a panel rather than running off the edge of it. */
void   plat_gfx_clip(int x, int y, int w, int h);
void   plat_gfx_noclip(void);

/* Sprites and images: four bits to a pixel, two pixels to a byte, low
 * nibble first. For a sprite, index 0 is transparent. */
void   plat_gfx_sprite(int x, int y, int w, int h, const void *data,
                       const unsigned *palette, int npal);
void   plat_gfx_blit(int x, int y, int w, int h, const void *data,
                     const unsigned *palette, int npal);

/* A stored 16-colour image: "CIM1", width, height, its own palette, then
 * the pixels. tools/make_image.py builds one from a PNG. */
typedef struct {
    char          magic[4];      /* "CIM1" */
    unsigned char w_lo, w_hi;
    unsigned char h_lo, h_hi;
    unsigned char flags;
    unsigned char reserved[7];
    unsigned char palette[16][3];
} GfxImageHdr;                   /* 64 bytes */

int    plat_gfx_image(int x, int y, const void *blob, int len);
int    plat_gfx_image_size(const void *blob, int len, int *w, int *h);
int    plat_gfx_get(int x, int y);           /* packed rgb, -1 off screen */
void   plat_gfx_line(int x0, int y0, int x1, int y1, unsigned rgb);
void   plat_gfx_rect(int x, int y, int w, int h, unsigned rgb);
void   plat_gfx_fill_rect(int x, int y, int w, int h, unsigned rgb);
void   plat_gfx_circle(int x, int y, int r, unsigned rgb);
void   plat_gfx_fill_circle(int x, int y, int r, unsigned rgb);
void   plat_gfx_text(int x, int y, const char *s, unsigned rgb);
void   plat_gfx_flush(void);
void   plat_gfx_hide(void);                  /* back to the console      */
int    plat_gfx_width(void);
int    plat_gfx_height(void);
int    plat_gfx_colours(void);               /* palette entries available */

/* make freshly written machine code visible to the instruction fetcher */
void   plat_sync_icache(const void *addr, size_t len);
int    plat_sysclk_khz(void);
/* re-apply display settings (colours, font, panel registers) at runtime */
void   plat_apply_display(void);
void   plat_display_test(void);

void   cos_printf(const char *fmt, ...);

/* Print prose broken at spaces to fit the console, every line indented by
 * `indent` columns. The screen is 40 columns wide, so anything written as a
 * sentence has to be wrapped deliberately or it breaks mid-word. */
void   cos_wrap(int indent, const char *s);

#endif /* COS_H */
