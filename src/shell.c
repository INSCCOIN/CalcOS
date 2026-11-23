/* The CalcOS shell: a calculator prompt that also drives the C compiler.
 *
 * A line is a command if its first word is one of the commands below and
 * that word is not a variable the user has defined; otherwise it is an
 * expression. That keeps `2+2` and `cc hello.c` both natural to type.
 */
#include "calc.h"
#include "cprog.h"
#include "settings.h"
#include "store.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* One buffer serves three jobs that never overlap: the source a compile
 * reads, the bytes a file moves to or from the flash store, and the
 * packaged program 'ccs' writes out after compiling, by which point the
 * source is dead. Sharing it is worth 16 KB on a 520 KB part. */
#ifndef SRC_BUF_SIZE
#define SRC_BUF_SIZE (32 * 1024)
#endif

/* See SRC_BUF_SIZE: this is also the buffer the store commands move files
 * through, and the one 'ccs' packages a compiled program into. The three
 * uses never overlap. */
static char srcbuf[SRC_BUF_SIZE];
#define iobuf ((unsigned char *)srcbuf)
#define IOBUF_SIZE ((int)sizeof srcbuf)

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

/* Break prose at spaces so it fits the console. An embedded newline starts a
 * fresh line; a run of spaces collapses. Every line, the first included, is
 * indented by `indent`. */
void cos_wrap(int indent, const char *s)
{
    int width = plat_cols(), col = 0, i;

    if (width < 16) width = 16;
    if (indent < 0 || indent > width - 8) indent = 0;

    for (;;) {
        const char *w;
        int len = 0;

        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '\n') { plat_putc('\n'); col = 0; s++; continue; }

        w = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') { s++; len++; }

        if (col && col + 1 + len > width) { plat_putc('\n'); col = 0; }
        if (col == 0) { for (i = 0; i < indent; i++) plat_putc(' '); col = indent; }
        else { plat_putc(' '); col++; }

        for (i = 0; i < len; i++) plat_putc(w[i]);
        col += len;
    }
    plat_putc('\n');
}

void cos_banner(void)
{
    cos_printf("CalcOS %s\n", COS_VERSION);
    cos_printf("calculator + C compiler, %dx%d\n", plat_cols(), plat_rows());
    cos_printf("type 'help' for commands\n\n");
}

/* ------------------------------------------------------------------ */
/* file helpers                                                        */
/* ------------------------------------------------------------------ */

static int slurp(const char *path, char *buf, int cap)
{
    void *f = plat_fopen(path, "r");
    int n = 0;
    char line[256];
    if (!f) return -1;
    while (plat_fgets(f, line, sizeof line)) {
        int l = (int)strlen(line);
        if (n + l + 1 >= cap) { n = -2; break; }
        memcpy(buf + n, line, (size_t)l);
        n += l;
    }
    plat_fclose(f);
    if (n < 0) return n;
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(Interp *in, const char *arg)
{
    int count, i, col = 0, width = plat_cols();
    const Builtin *t = cos_builtin_table(&count);

    if (arg && *arg && strcmp(arg, "all") != 0) {
        const Builtin *b = cos_builtin(arg);
        FuncDef *f;
        if (b) { cos_printf("%s\n", b->name); cos_wrap(2, b->help); return; }
        f = cos_lookup_fn(in, arg);
        if (f) { cos_printf("%s: your function, %d arg(s)\n", arg, f->nparams); return; }
        cos_printf("no help for '%s'\n", arg);
        return;
    }

    /* 'help all' is only the function names: 114 of them scroll a 26-row
     * screen clean, so they are kept off the main help page. */
    if (!(arg && *arg)) {
        cos_printf("commands\n");
        cos_printf("  cc F     compile a C file\n");
        cos_printf("  ccs N F  compile into flash as N\n");
        cos_printf("  use N    load one back from flash\n");
        cos_printf("  run      run its main()\n");
        cos_printf("  cfuncs   list C functions\n");
        cos_printf("  unload   drop the program\n");
        cos_printf("  ls [D]   list the card\n");
        cos_printf("  cat F    show a file\n");
        cos_printf("  set      settings\n");
        cos_printf("  display  panel info\n");
        cos_printf("  vars     your variables\n");
        cos_printf("  funcs    your functions\n");
        cos_printf("  save F   save variables\n");
        cos_printf("  load F   run a file\n");
        cos_printf("  units    unit names\n");
        cos_printf("  consts   physical constants\n");
        cos_printf("  store    the flash store\n");
        cos_printf("  history  what you have typed\n");
        cos_printf("  mem clear digits N ddigits N\n");
        cos_printf("  deg rad bits about\n");
        cos_printf("  rm F  restart  shutdown  quit\n");
        cos_printf("syntax\n");
        cos_printf("  x = 3        f(x) = x^2\n");
        cos_printf("  @(x) sin(x)  [1,2;3,4]\n");
        cos_printf("  1:0.5:3      plot(@(x) x^2,0,2)\n");
        cos_printf("  5 m/s * 3 s  60 mile/hour\n");
        cos_printf("help NAME  one function\n");
        cos_printf("help all   every function name\n");
        return;
    }

    for (i = 0; i < count; i++) {
        int w = (int)strlen(t[i].name) + 1;
        if (col + w > width) { plat_puts("\n"); col = 0; }
        cos_printf("%s ", t[i].name);
        col += w;
    }
    plat_puts("\n");
}

static void cmd_cc(Interp *in, const char *path)
{
    int n;
    (void)in;
    if (!path || !*path) { cos_printf("usage: cc FILE.c\n"); return; }
    n = slurp(path, srcbuf, sizeof srcbuf);
    if (n == -1) { cos_printf("cannot open %s\n", path); return; }
    if (n == -2) { cos_printf("%s is too large (max %d bytes)\n",
                              path, (int)sizeof srcbuf - 1); return; }
    if (cprog_compile(srcbuf, path) != 0) {
        cos_printf("%s line %d\n", path, cprog_errline());
        cos_wrap(2, cprog_error());
        return;
    }
    cos_printf("compiled %s\n", path);
    cos_printf("%d code, %d data, %d function(s)\n",
               cprog_code_size(), cprog_data_size(), cprog_nfuncs());
}

static void store_complain(int rc);
static int  parse_store_name(const char **pp, char *out, int cap);

/* Compile a file and keep the result in the store, so the program is there
 * after a reboot and runs with no card in the machine. */
static void cmd_ccs(Interp *in, const char *arg)
{
    char name[STORE_NAMELEN];
    const char *p = arg ? arg : "";
    int size, rc, got;

    if (!store_available()) { cos_wrap(0, "no flash store on this board"); return; }
    got = parse_store_name(&p, name, (int)sizeof name);
    if (got < 0) { store_complain(STORE_ENAME); return; }
    if (got == 0 || !*p) {
        cos_printf("usage: ccs NAME FILE.c\n");
        cos_wrap(2, "compiles the file and keeps the program in flash "
                    "under NAME; 'use NAME' loads it again");
        return;
    }

    cmd_cc(in, p);
    if (!cprog_loaded()) return;

    size = cprog_snapshot_size();
    if (size <= 0 || size > IOBUF_SIZE) {
        cos_printf("the program is too large to store (%d bytes)\n", size);
        return;
    }
    if (cprog_snapshot(iobuf, IOBUF_SIZE) != size) {
        cos_wrap(0, "the program could not be packaged");
        return;
    }
    rc = store_put(name, iobuf, size, STORE_PROG);
    if (rc == STORE_OK) {
        cos_printf("stored as %s, %d bytes\n", name, size);
        if (!strcmp(name, "boot")) cos_wrap(0, "it will load at every boot");
    } else {
        store_complain(rc);
    }
}

/* Load a program that is already in the store. No compiler, no card. */
static int load_stored_prog(const char *name, int quiet)
{
    const void *d;
    int len = 0, kind = 0, rc;

    d = store_find(name, &len, &kind);
    if (!d) {
        if (!quiet) cos_printf("no object called %s\n", name);
        return 0;
    }
    if (kind != STORE_PROG) {
        if (!quiet) cos_printf("%s is %s, not a program\n",
                               name, store_kind_name(kind));
        return 0;
    }
    rc = cprog_restore(d, len);
    if (rc == CPROG_RESTORE_OK) {
        if (!quiet)
            cos_printf("%s loaded: %d code, %d data, %d function(s)\n",
                       name, cprog_code_size(), cprog_data_size(),
                       cprog_nfuncs());
        return 1;
    }
    if (!quiet) {
        if (rc == CPROG_RESTORE_BUILD) {
            cos_printf("%s was built by another firmware\n", name);
            cos_wrap(2, "compiled code holds addresses into the firmware that "
                        "made it, so it has to be compiled again with ccs");
        } else {
            cos_printf("%s is not a usable program\n", name);
        }
    }
    return 0;
}

static void cmd_use(const char *arg)
{
    if (!arg || !*arg) {
        cos_printf("usage: use NAME\n");
        cos_wrap(2, "loads a program kept in flash by ccs");
        return;
    }
    load_stored_prog(arg, 0);
}

/* ------------------------------------------------------------------ */
/* the flash store                                                     */
/* ------------------------------------------------------------------ */

/* A page that has only ever been erased reads as all 0xFF, which is how a
 * free page is told from one holding data. Nothing is stored about which
 * pages are in use: the flash itself is the record. */
/* Take a name off the front of an argument. A name longer than the limit
 * is an error rather than something to truncate: truncating it would carry
 * the rest of the word into the next argument, and the user would be told
 * their file could not be read. */
static int parse_store_name(const char **pp, char *out, int cap)
{
    const char *p = *pp;
    int n = 0, over = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ') {
        if (n < cap - 1) out[n++] = *p;
        else over = 1;
        p++;
    }
    out[n] = 0;
    while (*p == ' ') p++;
    *pp = p;
    if (over) return -1;
    return n ? 1 : 0;
}

static void store_complain(int rc)
{
    switch (rc) {
    case STORE_EFULL:  cos_wrap(0, "the store is full; 'store free' shows "
                                   "what is left and 'store rm NAME' makes room");
                       break;
    case STORE_ENOENT: cos_wrap(0, "there is no object with that name"); break;
    case STORE_ENAME:  cos_printf("names are at most %d characters\n",
                                  STORE_NAMELEN - 1); break;
    case STORE_EIO:    cos_wrap(0, "the flash write failed"); break;
    default:           cos_wrap(0, "the store could not be changed"); break;
    }
}

static int store_page_free(int page)
{
    const unsigned char *d = (const unsigned char *)plat_store_read(page);
    int i, n = plat_store_page_size();
    if (!d) return 0;
    for (i = 0; i < n; i++) if (d[i] != 0xFF) return 0;
    return 1;
}

static int store_page_arg(const char **pp, int *page)
{
    const char *p = *pp;
    int v = 0, any = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = 1; }
    while (*p == ' ') p++;
    *pp = p;
    if (!any) return 0;
    if (v < 0 || v >= plat_store_pages()) return -1;
    *page = v;
    return 1;
}

static void cmd_store(const char *arg)
{
    int pages = plat_store_pages(), psz = plat_store_page_size();
    int page;

    if (!pages) {
        cos_wrap(0, "no flash store on this board");
        return;
    }
    if (!arg || !*arg) {
        int i, used = 0;
        for (i = 0; i < pages; i++) if (!store_page_free(i)) used++;
        cos_printf("%d pages of %d bytes\n", pages, psz);
        cos_printf("%d KB in flash, outside RAM\n", pages * psz / 1024);
        cos_printf("%d used, %d free\n", used, pages - used);
        cos_printf("%d named object(s)\n", store_count());
        cos_wrap(0, "by name:  list  free  put NAME FILE  get NAME FILE");
        cos_wrap(2, "rm NAME   view NAME [OFF]   format");
        cos_wrap(0, "raw page: map  dump N [OFF]  erase N|all");
        cos_wrap(2, "save N FILE   load N FILE");
        return;
    }

    if (!strcmp(arg, "list") || !strcmp(arg, "ls")) {
        int i, n = store_count();
        if (!n) { cos_wrap(0, "nothing stored yet"); return; }
        for (i = 0; i < n; i++) {
            const char *nm;
            int len, kind, pg, pgs;
            store_entry(i, &nm, &len, &kind, &pg, &pgs);
            cos_printf("%-16s %6d %s\n", nm, len, store_kind_name(kind));
            cos_printf("  page %d, %d page(s)\n", pg, pgs);
        }
        return;
    }

    if (!strcmp(arg, "free")) {
        int fp = store_free_pages();
        cos_printf("%d of %d pages free\n", fp, pages);
        cos_printf("%d KB free, %d KB used\n",
                   fp * psz / 1024, (pages - fp) * psz / 1024);
        cos_printf("%d object(s)\n", store_count());
        return;
    }

    if (!strcmp(arg, "format")) {
        if (store_format()) cos_wrap(0, "store erased and a fresh directory written");
        else cos_wrap(0, "could not format the store");
        return;
    }

    if (!strncmp(arg, "put", 3) && (arg[3] == ' ' || !arg[3])) {
        /* a file from the card into a named object */
        char name[STORE_NAMELEN];
        const char *p = arg + 3;
        void *f;
        int got, rc = parse_store_name(&p, name, (int)sizeof name);
        if (rc < 0) { store_complain(STORE_ENAME); return; }
        if (rc == 0 || !*p) { cos_printf("usage: store put NAME FILE\n"); return; }
        f = plat_fopen(p, "r");
        if (!f) { cos_printf("cannot read %s\n", p); return; }
        got = plat_fread(f, iobuf, IOBUF_SIZE);
        plat_fclose(f);
        if (got <= 0) { cos_printf("%s is empty\n", p); return; }
        rc = store_put(name, iobuf, got, STORE_RAW);
        if (rc == STORE_OK) cos_printf("%s: %d bytes stored\n", name, got);
        else store_complain(rc);
        return;
    }

    if (!strncmp(arg, "get", 3) && (arg[3] == ' ' || !arg[3])) {
        char name[STORE_NAMELEN];
        const char *p = arg + 3, *d;
        int len = 0, kind = 0, rc = parse_store_name(&p, name, (int)sizeof name);
        if (rc < 0) { store_complain(STORE_ENAME); return; }
        if (rc == 0 || !*p) { cos_printf("usage: store get NAME FILE\n"); return; }
        d = (const char *)store_find(name, &len, &kind);
        if (!d) { cos_printf("no object called %s\n", name); return; }
        if (plat_write_file(p, d, len)) cos_printf("%s to %s, %d bytes\n", name, p, len);
        else cos_printf("could not write %s\n", p);
        return;
    }

    if (!strncmp(arg, "rm", 2) && (arg[2] == ' ' || !arg[2])) {
        const char *p = arg + 2;
        int rc;
        while (*p == ' ') p++;
        if (!*p) { cos_printf("usage: store rm NAME\n"); return; }
        rc = store_delete(p);
        if (rc == STORE_OK) cos_printf("%s removed\n", p);
        else store_complain(rc);
        return;
    }

    if (!strncmp(arg, "view", 4) && (arg[4] == ' ' || !arg[4])) {
        char name[STORE_NAMELEN];
        const unsigned char *d;
        const char *p = arg + 4;
        int len = 0, kind = 0, off = 0, r, c, rows = 16;
        int rc = parse_store_name(&p, name, (int)sizeof name);
        if (rc < 0) { store_complain(STORE_ENAME); return; }
        while (*p >= '0' && *p <= '9') off = off * 10 + (*p++ - '0');
        if (rc == 0) { cos_printf("usage: store view NAME [OFFSET]\n"); return; }
        d = (const unsigned char *)store_find(name, &len, &kind);
        if (!d) { cos_printf("no object called %s\n", name); return; }
        off &= ~7;
        if (off >= len) off = 0;
        if (off + rows * 8 > len) rows = (len - off + 7) / 8;
        for (r = 0; r < rows; r++) {
            int base = off + r * 8;
            cos_printf("%04X ", base);
            for (c = 0; c < 8; c++)
                if (base + c < len) cos_printf("%02X", d[base + c]);
                else plat_puts("  ");
            plat_putc(' ');
            for (c = 0; c < 8; c++) {
                int ch = base + c < len ? d[base + c] : ' ';
                plat_putc((char)(ch >= 32 && ch < 127 ? ch : '.'));
            }
            plat_puts("\n");
        }
        cos_printf("%s: %s, %d bytes\n", name, store_kind_name(kind), len);
        return;
    }

    if (!strcmp(arg, "map")) {
        int i, col = 0, width = plat_cols();
        cos_wrap(0, "# is a page with data, . is erased");
        for (i = 0; i < pages; i++) {
            if (col == 0) { cos_printf("%3d ", i); col = 4; }
            cos_printf("%c", store_page_free(i) ? '.' : '#');
            if (++col >= width) { plat_puts("\n"); col = 0; }
        }
        if (col) plat_puts("\n");
        return;
    }

    if (!strncmp(arg, "dump", 4)) {
        const unsigned char *d;
        const char *p = arg + 4;
        int r, c, rows = 16, off = 0;
        int rc = store_page_arg(&p, &page);
        if (rc <= 0) { cos_printf("usage: store dump PAGE [OFFSET]\n"); return; }
        while (*p >= '0' && *p <= '9') off = off * 10 + (*p++ - '0');
        off &= ~7;
        if (off >= psz) off = psz - rows * 8;
        if (off < 0) off = 0;
        d = (const unsigned char *)plat_store_read(page);
        if (!d) { cos_printf("page %d cannot be read\n", page); return; }
        /* Eight bytes to a line: the offset, the hex and the characters
         * come to exactly 30 columns, which fits the panel with room. */
        if (off + rows * 8 > psz) rows = (psz - off) / 8;
        for (r = 0; r < rows; r++) {
            int base = off + r * 8;
            cos_printf("%04X ", base);
            for (c = 0; c < 8; c++) cos_printf("%02X", d[base + c]);
            plat_putc(' ');
            for (c = 0; c < 8; c++) {
                int ch = d[base + c];
                plat_putc((char)(ch >= 32 && ch < 127 ? ch : '.'));
            }
            plat_puts("\n");
        }
        cos_printf("page %d, %d-%d of %d\n", page, off, off + rows * 8 - 1, psz);
        return;
    }

    if (!strncmp(arg, "erase", 5)) {
        const char *p = arg + 5;
        int rc;
        while (*p == ' ') p++;
        if (!strcmp(p, "all")) {
            int i, n = 0;
            for (i = 0; i < pages; i++)
                if (!store_page_free(i) && plat_store_erase(i)) n++;
            cos_printf("erased %d page(s)\n", n);
            return;
        }
        rc = store_page_arg(&p, &page);
        if (rc <= 0) { cos_printf("usage: store erase PAGE | all\n"); return; }
        if (plat_store_erase(page)) cos_printf("erased page %d\n", page);
        else cos_printf("page %d could not be erased\n", page);
        return;
    }

    if (!strncmp(arg, "save", 4)) {
        /* a file from the card into a single raw page */
        const char *p = arg + 4;
        void *f;
        int got, rc = store_page_arg(&p, &page);
        if (rc <= 0 || !*p) {
            cos_printf("usage: store save PAGE FILE\n");
            return;
        }
        f = plat_fopen(p, "r");
        if (!f) { cos_printf("cannot read %s\n", p); return; }
        got = plat_fread(f, iobuf, psz < IOBUF_SIZE ? psz : IOBUF_SIZE);
        plat_fclose(f);
        if (got <= 0) { cos_printf("%s is empty\n", p); return; }
        if (plat_store_write(page, iobuf, got))
            cos_printf("%d bytes into page %d\n", got, page);
        else
            cos_printf("page %d could not be written\n", page);
        return;
    }

    if (!strncmp(arg, "load", 4)) {
        /* a page out to a file on the card */
        const char *p = arg + 4;
        const void *d;
        int rc = store_page_arg(&p, &page);
        if (rc <= 0 || !*p) {
            cos_printf("usage: store load PAGE FILE\n");
            return;
        }
        d = plat_store_read(page);
        if (!d) { cos_printf("page %d cannot be read\n", page); return; }
        if (plat_write_file(p, d, psz)) cos_printf("page %d to %s\n", page, p);
        else {
            cos_printf("could not write %s\n", p);
            cos_wrap(2, "8.3 names only, and the card has to be present");
        }
        return;
    }

    cos_printf("unknown: store %s\n", arg);
    cos_wrap(0, "list free put get rm view format");
    cos_wrap(0, "map dump erase save load");
}

static void cmd_run(void)
{
    int rc;
    if (!cprog_loaded()) { cos_printf("nothing compiled yet (cc FILE.c)\n"); return; }
    rc = cprog_run_main();
    /* A program that started a tone and returned without stopping it would
     * otherwise leave the buzzer going with nothing left to turn it off. */
    plat_tone_off();
    cos_printf("\n[exit %d]\n", rc);
}

static void cmd_cfuncs(void)
{
    int i, n = cprog_nfuncs();
    if (!cprog_loaded()) { cos_printf("nothing compiled yet\n"); return; }
    for (i = 0; i < n; i++)
        cos_printf("%s/%d\n", cprog_func_name(i), cprog_func_nargs(i));
}

static void cmd_ls(const char *dir)
{
    static char buf[1024];
    if (plat_listdir(dir && *dir ? dir : "/", buf, sizeof buf) != 0) {
        cos_printf("cannot list %s\n", dir && *dir ? dir : "/");
        return;
    }
    plat_puts(buf);
}

static void cmd_cat(const char *path)
{
    void *f;
    char line[256];
    if (!path || !*path) { cos_printf("usage: cat FILE\n"); return; }
    f = plat_fopen(path, "r");
    if (!f) { cos_printf("cannot open %s\n", path); return; }
    while (plat_fgets(f, line, sizeof line)) plat_puts(line);
    plat_fclose(f);
}

/* ------------------------------------------------------------------ */
/* settings                                                            */
/* ------------------------------------------------------------------ */

const char *settings_help(const char *key);

/* Settings that the rest of the system caches have to be pushed out after
 * any change, so 'set fg 0xFF0000' takes effect immediately rather than at
 * the next boot. */
static void settings_apply(Interp *in)
{
    in->digits = g_set.digits;
    in->ddigits = g_set.ddigits;
    in->deg = g_set.deg;
    plat_apply_display();
}

static void cmd_set(Interp *in, char *arg)
{
    char *sp;
    /* Two key/value columns of 20 fit the 40-column screen exactly, so the
     * whole table is eight lines and nothing wraps. The descriptions are a
     * line each under 'set help'. */
    if (!arg || !*arg) {
        int i, n = settings_count(), percol = plat_cols() >= 40 ? 2 : 1;
        char v[32];
        for (i = 0; i < n; i++) {
            const char *k = settings_key(i);
            settings_get(k, v, sizeof v);
            if (i % percol == percol - 1 || i == n - 1)
                cos_printf("%-9s %s\n", k, v);
            else
                cos_printf("%-9s %-9s ", k, v);
        }
        cos_printf("\nset KEY VALUE   set help\n");
        cos_printf("set save | load | reset\n");
        return;
    }
    if (!strcmp(arg, "help")) {
        int i, n = settings_count();
        char v[32];
        for (i = 0; i < n; i++) {
            const char *k = settings_key(i);
            settings_get(k, v, sizeof v);
            cos_printf("%-9s %s\n", k, v);
            cos_wrap(2, settings_help(k));
        }
        return;
    }
    if (!strcmp(arg, "save")) {
        if (settings_save()) cos_printf("saved %s\n", SETTINGS_PATH);
        else cos_wrap(0, "could not write the settings file: no card, "
                         "card full, or write protected");
        return;
    }
    if (!strcmp(arg, "load")) {
        if (settings_load()) { settings_apply(in); cos_printf("loaded %s\n", SETTINGS_PATH); }
        else cos_printf("no %s on the card\n", SETTINGS_PATH);
        return;
    }
    if (!strcmp(arg, "reset")) {
        settings_defaults();
        settings_apply(in);
        cos_printf("defaults restored, not yet saved\n");
        return;
    }

    sp = arg;
    while (*sp && *sp != ' ' && *sp != '\t' && *sp != '=') sp++;
    if (*sp) {
        *sp++ = 0;
        while (*sp == ' ' || *sp == '\t' || *sp == '=') sp++;
    }
    if (!*sp) {
        char v[32];
        if (settings_get(arg, v, sizeof v) != 0) {
            cos_printf("no setting called '%s'\n", arg);
            return;
        }
        cos_printf("%s = %s\n", arg, v);
        cos_wrap(2, settings_help(arg));
        return;
    }
    switch (settings_set(arg, sp)) {
    case 0: {
        char v[32];
        settings_get(arg, v, sizeof v);
        settings_apply(in);
        cos_printf("%s = %s\n", arg, v);
        if (!strcmp(arg, "cpu"))
            cos_wrap(0, "takes effect at the next boot; "
                        "'set save' to keep it");
        break;
    }
    case -1: cos_printf("no setting called '%s'\n", arg); break;
    default: cos_printf("'%s' is not a valid value for %s\n", sp, arg); break;
    }
}

static void cmd_display(Interp *in, const char *arg)
{
    if (arg && !strcmp(arg, "test")) { plat_display_test(); return; }
    if (arg && !strcmp(arg, "reset")) {
        g_set.lcd_bits = 16;
        g_set.lcd_madctl = 0x48;
        g_set.lcd_invert = 1;
        g_set.lcd_spi_mhz = 0;
        settings_apply(in);
        cos_printf("panel settings reset and reapplied\n");
        return;
    }
    cos_printf("panel   %d-bit  madctl 0x%02X\n",
               g_set.lcd_bits, (unsigned)g_set.lcd_madctl);
    cos_printf("invert %d  spi %d MHz  light %d\n",
               g_set.lcd_invert, g_set.lcd_spi_mhz, g_set.backlight);
    cos_printf("console %dx%d  font %s\n", plat_cols(), plat_rows(),
               g_set.font ? "bold" : "regular");
    cos_printf("fg 0x%06X  bg 0x%06X\n", g_set.fg, g_set.bg);
    cos_printf("\nif the screen is blank, in order:\n");
    cos_printf("  set lcdbits 18\n");
    cos_wrap(4, "some panels want 18-bit pixels");
    cos_printf("  set backlight 255\n");
    cos_wrap(4, "the keyboard controller drives it");
    cos_printf("  set lcdmadctl 0x40\n");
    cos_wrap(4, "if text is mirrored or rotated");
    cos_printf("  set lcdspi 20\n");
    cos_wrap(4, "if the output is garbled");
    cos_printf("  display test\n");
    cos_wrap(4, "colour bars, then white");
    cos_wrap(0, "'set save' keeps whatever works.");
}

static void cmd_vars(Interp *in)
{
    Var *v;
    char buf[160];
    for (v = in->globals; v; v = v->next) {
        v_format(in, v->v, buf, sizeof buf);
        cos_printf("%-8s %s\n", v->name, buf);
    }
}

static void cmd_funcs(Interp *in)
{
    FuncDef *f;
    for (f = in->funcs; f; f = f->next)
        cos_printf("%s/%d\n", f->name, f->nparams);
    if (!in->funcs) cos_printf("(none)\n");
}

static void cmd_save(Interp *in, const char *path)
{
    static char out[4096];
    Var *v;
    char buf[200];
    int n = 0;
    if (!path || !*path) { cos_printf("usage: save FILE\n"); return; }
    for (v = in->globals; v; v = v->next) {
        if (v->v.k == K_MAT || v->v.k == K_FUN) continue;
        if (!strcmp(v->name, "pi") || !strcmp(v->name, "e")
            || !strcmp(v->name, "i") || !strcmp(v->name, "ans")
            || !strcmp(v->name, "eps") || !strcmp(v->name, "inf")
            || !strcmp(v->name, "nan") || !strcmp(v->name, "phi")) continue;
        v_format(in, v->v, buf, sizeof buf);
        n += snprintf(out + n, sizeof out - (size_t)n, "%s = %s\n", v->name, buf);
        if (n > (int)sizeof out - 256) break;
    }
    if (plat_write_file(path, out, n)) cos_printf("saved %d bytes to %s\n", n, path);
    else {
        cos_printf("could not write %s\n", path);
        cos_wrap(2, "8.3 names only, and the card has to be present");
    }
}

static void cmd_load(Interp *in, const char *path)
{
    void *fp;
    char line[256];
    int n = 0;
    if (!path || !*path) { cos_printf("usage: load FILE\n"); return; }
    fp = plat_fopen(path, "r");
    if (!fp) { cos_printf("cannot read %s\n", path); return; }
    while (plat_fgets(fp, line, sizeof line)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        cos_shell_line(in, s);
        n++;
    }
    plat_fclose(fp);
    cos_printf("loaded %d line(s)\n", n);
}

/* ------------------------------------------------------------------ */
/* command dispatch                                                    */
/* ------------------------------------------------------------------ */

static const char *cmd_words[] = {
    "help", "cc", "run", "cfuncs", "unload", "ls", "cat", "rm", "vars",
    "funcs", "mem", "clear", "free", "save", "load", "digits", "ddigits",
    "deg", "rad", "bits", "about", "quit", "exit", "set", "display",
    "history", "shutdown", "restart", "units", "consts", "store",
    "ccs", "use", NULL
};

/* for tab completion */
const char *const *cos_command_words(void) { return cmd_words; }
const char *cos_prompt(void) { return "> "; }

static int is_command_word(Interp *in, const char *w)
{
    int i;
    for (i = 0; cmd_words[i]; i++)
        if (strcmp(cmd_words[i], w) == 0) break;
    if (!cmd_words[i]) return 0;
    /* a variable or function of the same name wins, so nothing the user
     * defines can be shadowed by the shell */
    if (cos_lookup(in, w)) return 0;
    if (cos_lookup_fn(in, w)) return 0;
    return 1;
}

static int run_command(Interp *in, char *word, char *arg)
{
    if (!strcmp(word, "quit") || !strcmp(word, "exit")) return 1;
    else if (!strcmp(word, "help")) cmd_help(in, arg);
    else if (!strcmp(word, "cc")) cmd_cc(in, arg);
    else if (!strcmp(word, "run")) cmd_run();
    else if (!strcmp(word, "ccs")) cmd_ccs(in, arg);
    else if (!strcmp(word, "use")) cmd_use(arg);
    else if (!strcmp(word, "cfuncs")) cmd_cfuncs();
    else if (!strcmp(word, "unload")) { cprog_unload(); cos_printf("unloaded\n"); }
    else if (!strcmp(word, "ls")) cmd_ls(arg);
    else if (!strcmp(word, "cat")) cmd_cat(arg);
    else if (!strcmp(word, "vars")) cmd_vars(in);
    else if (!strcmp(word, "funcs")) cmd_funcs(in);
    else if (!strcmp(word, "set")) cmd_set(in, arg);
    else if (!strcmp(word, "display")) cmd_display(in, arg);
    else if (!strcmp(word, "save")) cmd_save(in, arg);
    else if (!strcmp(word, "load")) cmd_load(in, arg);
    else if (!strcmp(word, "deg")) { in->deg = g_set.deg = 1; cos_printf("degrees\n"); }
    else if (!strcmp(word, "rad")) { in->deg = g_set.deg = 0; cos_printf("radians\n"); }
    else if (!strcmp(word, "bits")) {
        in->verbose_fp = !in->verbose_fp;
        cos_printf("bit view %s\n", in->verbose_fp ? "on" : "off");
    }
    else if (!strcmp(word, "digits")) {
        if (arg && *arg) in->digits = atoi(arg);
        if (in->digits < 1) in->digits = 1;
        if (in->digits > 17) in->digits = 17;
        g_set.digits = in->digits;
        cos_printf("%d significant digits\n", in->digits);
    }
    else if (!strcmp(word, "ddigits")) {
        if (arg && *arg) in->ddigits = atoi(arg);
        if (in->ddigits < 1) in->ddigits = 1;
        if (in->ddigits > 34) in->ddigits = 34;
        g_set.ddigits = in->ddigits;
        cos_printf("%d extended digits\n", in->ddigits);
    }
    else if (!strcmp(word, "mem") || !strcmp(word, "free")) {
        cos_printf("arena %u  peak %u\n",
                   (unsigned)in->arena.total, (unsigned)in->arena.peak);
        cos_printf("heap free %u\n", (unsigned)plat_free_mem());
        if (cprog_loaded()) {
            cos_printf("program %s\n", cprog_name());
            cos_printf("%d code, %d data\n",
                       cprog_code_size(), cprog_data_size());
        }
    }
    else if (!strcmp(word, "clear")) {
        cos_shutdown(in);
        cos_init(in);
        cos_printf("workspace cleared\n");
    }
    else if (!strcmp(word, "about")) {
        cos_printf("CalcOS %s\n", COS_VERSION);
        cos_wrap(0, "for the ClockworkPi PicoCalc (RP2350): doubles, "
                    "32-digit extended precision, intervals, matrices, "
                    "and a C compiler that emits native Thumb-2.");
    }
    else if (!strcmp(word, "store")) cmd_store(arg);
    else if (!strcmp(word, "units")) {
        int i, n = unit_count(), col = 0, width = plat_cols();
        for (i = 0; i < n; i++) {
            const char *u = unit_name(i);
            int w = (int)strlen(u) + 1;
            if (col + w > width) { plat_puts("\n"); col = 0; }
            cos_printf("%s ", u);
            col += w;
        }
        plat_puts("\n");
        cos_wrap(0, "write a quantity as '5 m/s'; conv(5,\"mile\",\"km\") "
                    "and to(x,\"km\") convert.");
    }
    else if (!strcmp(word, "consts")) {
        int i, n = const_count();
        for (i = 0; i < n; i++) {
            const char *nm, *what, *ut;
            double v;
            Unit u;
            char num[40];
            const_at(i, &nm, &v, &u, &what, &ut);
            snprintf(num, sizeof num, "%.10g", v);
            cos_printf("%-9s %s %s\n", nm, num, ut);
            cos_wrap(2, what);
        }
    }
    else if (!strcmp(word, "rm")) {
        if (!arg || !*arg) cos_printf("usage: rm FILE\n");
        else if (plat_remove(arg)) cos_printf("removed %s\n", arg);
        else {
            cos_printf("could not remove %s\n", arg);
            cos_wrap(2, "no such file, or the card is write protected");
        }
    }
    else if (!strcmp(word, "history")) {
        int i, n = cos_history_count();
        if (arg && !strcmp(arg, "clear")) { cos_history_clear(); return 0; }
        for (i = 0; i < n; i++) cos_printf("%3d %s\n", i + 1, cos_history_at(i));
    }
    else if (!strcmp(word, "restart")) {
        plat_reboot();
    }
    else if (!strcmp(word, "shutdown")) {
        /* Settings are saved first: a shutdown that loses the colour you
         * just set would be worse than a slow one. */
        settings_save();
        plat_poweroff();
    }
    else cos_printf("unknown command '%s'\n", word);
    return 0;
}

/* ------------------------------------------------------------------ */
/* startup files                                                       */
/* ------------------------------------------------------------------ */

/* Run whatever the card says to run at boot.
 *
 *   /startup.txt   lines of calculator input, run as if typed
 *   /autoexec.c    compiled, and its main() run if it has one
 *
 * Holding escape at boot skips both, which is the way back from a startup
 * file that does something unwelcome.
 */
/* A program kept in the store under the name "boot" is loaded before
 * anything else, so a machine with no card in it still comes up with its
 * functions available. */
void cos_load_boot_prog(void)
{
    if (!store_available()) return;
    if (load_stored_prog("boot", 1))
        cos_printf("boot: %d function(s) from flash\n", cprog_nfuncs());
}

void cos_run_startup(Interp *in)
{
    void *f = plat_fopen("/startup.txt", "r");
    if (f) {
        char line[256];
        cos_printf("running /startup.txt\n");
        while (plat_fgets(f, line, sizeof line)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!line[0] || line[0] == '#') continue;
            if (cos_shell_line(in, line)) break;
        }
        plat_fclose(f);
    }

    f = plat_fopen("/autoexec.c", "r");
    if (f) {
        plat_fclose(f);
        cos_printf("compiling /autoexec.c\n");
        cmd_cc(in, "/autoexec.c");
        if (cprog_loaded()) cmd_run();
    }
}

/* ------------------------------------------------------------------ */

int cos_shell_line(Interp *in, char *raw)
{
    char *line = trim(raw);
    int quiet = 0;
    size_t len;

    if (!*line || *line == '#') return 0;

    /* ':' is always a command, for scripts that want to be unambiguous */
    if (*line == ':') line++;
    {
        char word[24];
        char *p = line, *arg;
        int n = 0;
        while (*p && *p != ' ' && *p != '\t' && n < (int)sizeof word - 1)
            word[n++] = *p++;
        word[n] = 0;
        arg = trim(p);
        if (raw[0] == ':' || is_command_word(in, word)) {
            /* not a command if an operator follows, so 'free = 3' still works */
            if (!(*arg == '=' && arg[1] != '='))
                return run_command(in, word, arg);
        }
    }

    len = strlen(line);
    if (len && line[len - 1] == ';') { quiet = 1; line[len - 1] = 0; }

    in->steps = 0;
    in->depth = 0;
    in->jb_set = 1;
    if (setjmp(in->jb) == 0) {
        Node *n = cos_parse(in, line);
        if (n) {
            Value v = cos_eval(in, n);
            if (v.k != K_NIL) {
                cos_setvar(in, "ans", v);
                if (!quiet) v_print(in, v);
            }
        }
    } else {
        cos_printf("error: %s\n", in->err);
    }
    in->jb_set = 0;
    cos_bigs_release(in);
    arena_reset(&in->arena);
    return 0;
}

void cos_shell_run(Interp *in)
{
    char line[256];
    cos_banner();
    for (;;) {
        if (cos_readline(in, line, sizeof line, cos_prompt()) < 0) break;
        if (cos_shell_line(in, line)) break;
    }
    cos_printf("bye\n");
}
