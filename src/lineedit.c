/* The command line: editing, history and completion.
 *
 * This lives above the platform layer rather than inside it, so the device
 * and the host run the same editor and the same code can be tested. It
 * needs only three things from the platform: a key, a character out, and
 * the console width.
 *
 * Redrawing is deliberately simple. The editor knows the cursor's column
 * offset from the start of the input, so it can walk back with '\b',
 * reprint the line, blank whatever the previous line left over, and walk
 * back again to the cursor. That costs a few dozen characters per keypress
 * and needs no cursor addressing, which the console does not have.
 */
#include "calc.h"
#include "settings.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define HIST_LINES  32
#define HIST_LEN    120
#define MAXCAND     128

static char hist[HIST_LINES][HIST_LEN];
static int  hist_count;          /* how many are filled, newest last */

/* ------------------------------------------------------------------ */
/* history                                                             */
/* ------------------------------------------------------------------ */

void cos_history_add(const char *line)
{
    int i;
    while (*line == ' ') line++;
    if (!*line) return;
    /* a command repeated straight away is not worth a second slot */
    if (hist_count && strcmp(hist[hist_count - 1], line) == 0) return;
    if (hist_count == HIST_LINES) {
        for (i = 0; i < HIST_LINES - 1; i++)
            memcpy(hist[i], hist[i + 1], HIST_LEN);
        hist_count--;
    }
    snprintf(hist[hist_count], HIST_LEN, "%s", line);
    hist_count++;
}

int cos_history_count(void) { return hist_count; }

const char *cos_history_at(int i)
{
    if (i < 0 || i >= hist_count) return NULL;
    return hist[i];
}

void cos_history_clear(void) { hist_count = 0; }

/* ------------------------------------------------------------------ */
/* completion                                                          */
/* ------------------------------------------------------------------ */

extern const char *const *cos_command_words(void);

/* Everything that could follow the text being completed, gathered into one
 * buffer of NUL-separated names. */
typedef struct {
    char  buf[MAXCAND * 20];
    int   used;
    int   n;
    int   space;      /* follow a unique match with a space */
} Cands;

static void cand_add(Cands *c, const char *name)
{
    int l = (int)strlen(name), i;
    const char *p = c->buf;
    if (c->n >= MAXCAND || c->used + l + 1 > (int)sizeof c->buf) return;
    for (i = 0; i < c->n; i++) {             /* no duplicates */
        if (strcmp(p, name) == 0) return;
        p += strlen(p) + 1;
    }
    memcpy(c->buf + c->used, name, (size_t)l + 1);
    c->used += l + 1;
    c->n++;
}

static const char *cand_at(const Cands *c, int i)
{
    const char *p = c->buf;
    int k;
    for (k = 0; k < i; k++) p += strlen(p) + 1;
    return p;
}

/* A command that takes a file name as its argument; completing one of
 * these offers what is on the card instead of function names. */
static int takes_a_filename(const char *word)
{
    return !strcmp(word, "cc") || !strcmp(word, "cat") || !strcmp(word, "ls")
        || !strcmp(word, "load") || !strcmp(word, "save")
        || !strcmp(word, "run") || !strcmp(word, "rm");
}

static void gather(Interp *in, const char *line, int wordstart,
                   const char *frag, Cands *c)
{
    int flen = (int)strlen(frag);
    int first_word = 1, i;

    for (i = 0; i < wordstart; i++)
        if (line[i] != ' ') { first_word = 0; break; }

    if (!first_word) {
        /* after a command that names a file, complete against the card */
        char head[32];
        int n = 0;
        while (line[n] && line[n] != ' ' && n < (int)sizeof head - 1) {
            head[n] = line[n];
            n++;
        }
        head[n] = 0;
        if (takes_a_filename(head)) {
            c->space = 1;
            static char names[1024];
            if (plat_listdir("/", names, sizeof names) >= 0) {
                const char *p = names;
                while (*p) {
                    const char *e = strchr(p, '\n');
                    int l = e ? (int)(e - p) : (int)strlen(p);
                    char nm[64];
                    if (l > 0 && l < (int)sizeof nm) {
                        memcpy(nm, p, (size_t)l);
                        nm[l] = 0;
                        /* the listing marks directories with a trailing / */
                        if (!strncmp(nm, frag, (size_t)flen)) cand_add(c, nm);
                    }
                    if (!e) break;
                    p = e + 1;
                }
            }
            return;
        }
    }

    if (first_word) {
        const char *const *w = cos_command_words();
        for (i = 0; w[i]; i++)
            if (!strncmp(w[i], frag, (size_t)flen)) cand_add(c, w[i]);
    }
    {
        int nb;
        const Builtin *t = cos_builtin_table(&nb);
        for (i = 0; i < nb; i++)
            if (!strncmp(t[i].name, frag, (size_t)flen)) cand_add(c, t[i].name);
    }
    {
        Var *v;
        FuncDef *f;
        for (v = in->globals; v; v = v->next)
            if (!strncmp(v->name, frag, (size_t)flen)) cand_add(c, v->name);
        for (f = in->funcs; f; f = f->next)
            if (!strncmp(f->name, frag, (size_t)flen)) cand_add(c, f->name);
    }
}

/* ------------------------------------------------------------------ */
/* function keys                                                       */
/* ------------------------------------------------------------------ */

/* F1 to F10 stand for the commands that are tedious to type on a thumb
 * keyboard. On an empty line the command runs; with something already
 * typed the word is inserted at the cursor instead, so a function key can
 * never destroy a line being worked on. The keyboard sends F6 to F10 as
 * the shifted F1 to F5. */
static const char *fkey_word(int k)
{
    switch (k) {
    case KEY_F1:  return "help";
    case KEY_F2:  return "vars";
    case KEY_F3:  return "funcs";
    case KEY_F4:  return "units";
    case KEY_F5:  return "consts";
    case KEY_F6:  return "mem";
    case KEY_F7:  return "ls";
    case KEY_F8:  return "display";
    case KEY_F9:  return "history";
    case KEY_F10: return "clear";
    default:      return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* the editor                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char   *buf;
    int     cap;
    int     len, pos;
    int     shown;          /* characters drawn after the prompt */
} Line;

static void set_text(Line *l, const char *s);

static void back(int n)   { while (n-- > 0) plat_putc('\b'); }
static void blanks(int n) { while (n-- > 0) plat_putc(' '); }

/* Repaint the line. `screen_col` says where the cursor physically is,
 * counted from the first character after the prompt, which is not always
 * l->pos: an insertion moves l->pos before the screen has caught up. */
static void redraw_from(Line *l, int screen_col)
{
    int i;
    back(screen_col);
    for (i = 0; i < l->len; i++) plat_putc(l->buf[i]);
    if (l->shown > l->len) {
        blanks(l->shown - l->len);
        back(l->shown - l->len);
    }
    l->shown = l->len;
    back(l->len - l->pos);
}

/* Only safe where the cursor has not moved since the last redraw: anything
 * that changes l->pos must capture the old column first and use
 * redraw_from, or the repaint starts in the wrong place and leaves the
 * characters it was meant to overwrite sitting on screen. */
static void redraw(Line *l) { redraw_from(l, l->pos); }

/* Replace the whole line, as the history keys do. */
static void replace_line(Line *l, const char *s)
{
    int old = l->shown, i;
    back(l->pos);
    set_text(l, s);
    for (i = 0; i < l->len; i++) plat_putc(l->buf[i]);
    if (old > l->len) { blanks(old - l->len); back(old - l->len); }
    l->shown = l->len;
}

static void insert_text(Line *l, const char *s, int n)
{
    int i;
    if (n <= 0 || l->len + n >= l->cap) return;
    for (i = l->len - 1; i >= l->pos; i--) l->buf[i + n] = l->buf[i];
    memcpy(l->buf + l->pos, s, (size_t)n);
    l->len += n;
    l->pos += n;
}

static void set_text(Line *l, const char *s)
{
    int n = (int)strlen(s);
    if (n >= l->cap) n = l->cap - 1;
    memcpy(l->buf, s, (size_t)n);
    l->len = l->pos = n;
    l->buf[n] = 0;
}

static void do_complete(Interp *in, Line *l)
{
    Cands c;
    int start = l->pos, i, common;
    char frag[64];

    c.used = c.n = c.space = 0;
    /* The buffer is only terminated when the line is entered, so anything
     * that reads it as a string — working out which command this word
     * belongs to — would otherwise run into whatever a longer earlier line
     * left behind. */
    l->buf[l->len] = 0;
    while (start > 0) {
        char ch = l->buf[start - 1];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.'
            || ch == '/') start--;
        else break;
    }
    if (l->pos - start >= (int)sizeof frag) return;
    memcpy(frag, l->buf + start, (size_t)(l->pos - start));
    frag[l->pos - start] = 0;

    gather(in, l->buf, start, frag, &c);
    if (c.n == 0) return;

    /* extend by as much as every candidate agrees on */
    common = (int)strlen(cand_at(&c, 0));
    for (i = 1; i < c.n; i++) {
        const char *s = cand_at(&c, i);
        int k = 0;
        while (k < common && s[k] == cand_at(&c, 0)[k]) k++;
        common = k;
    }
    if (c.n == 1 || common > (int)strlen(frag)) {
        int col = l->pos;
        if (common > (int)strlen(frag))
            insert_text(l, cand_at(&c, 0) + strlen(frag),
                        common - (int)strlen(frag));
        /* A command or a file name is finished once it is unique, so a
         * space follows. A function name is not: what usually comes next
         * is '(', and a space there would just have to be deleted. */
        if (c.n == 1 && l->len < l->cap - 1) {
            const char *only = cand_at(&c, 0);
            int cmd = 0, k;
            const char *const *w = cos_command_words();
            for (k = 0; w[k]; k++) if (!strcmp(w[k], only)) { cmd = 1; break; }
            if (c.space || cmd) insert_text(l, " ", 1);
        }
        redraw_from(l, col);
        return;
    }

    /* nothing more is shared: show what is possible and reprint the line */
    plat_putc('\n');
    {
        int col = 0, width = plat_cols();
        for (i = 0; i < c.n && i < 40; i++) {
            const char *s = cand_at(&c, i);
            int w = (int)strlen(s) + 1;
            if (col + w > width) { plat_putc('\n'); col = 0; }
            plat_puts(s);
            plat_putc(' ');
            col += w;
        }
        if (c.n > 40) plat_puts("...");
        plat_putc('\n');
    }
    l->shown = 0;
    plat_puts(cos_prompt());
    for (i = 0; i < l->len; i++) plat_putc(l->buf[i]);
    l->shown = l->len;
    back(l->len - l->pos);
}

/* Read one line. Returns its length, or -1 at end of input. */
int cos_readline(Interp *in, char *buf, int cap, const char *prompt)
{
    Line l;
    int hpos = hist_count;          /* one past the end: the line being typed */
    char saved[HIST_LEN];

    l.buf = buf; l.cap = cap; l.len = 0; l.pos = 0; l.shown = 0;
    buf[0] = 0;
    saved[0] = 0;
    if (prompt) plat_puts(prompt);

    for (;;) {
        int k;
        /* Everything drawn for the last keystroke goes to the panel before
         * we block here, or the line being typed never appears. */
        plat_flush();
        k = plat_getc();
        if (k < 0) return -1;

        if (k == '\r' || k == '\n') {
            while (l.pos < l.len) { plat_putc(l.buf[l.pos]); l.pos++; }
            plat_putc('\n');
            buf[l.len] = 0;
            cos_history_add(buf);
            plat_flush();
            return l.len;
        }
        if (k == '\t') { do_complete(in, &l); continue; }
        if (k == 8 || k == 127) {            /* backspace */
            if (l.pos > 0) {
                int col = l.pos;             /* where the cursor still is */
                memmove(l.buf + l.pos - 1, l.buf + l.pos, (size_t)(l.len - l.pos));
                l.pos--; l.len--;
                redraw_from(&l, col);
            }
            continue;
        }
        if (k == KEY_DEL || k == 4) {        /* delete under the cursor */
            if (l.pos < l.len) {
                memmove(l.buf + l.pos, l.buf + l.pos + 1, (size_t)(l.len - l.pos - 1));
                l.len--;
                redraw(&l);
            }
            continue;
        }
        if (k == KEY_LEFT || k == 2) {
            if (l.pos > 0) { l.pos--; plat_putc('\b'); }
            continue;
        }
        if (k == KEY_RIGHT || k == 6) {
            if (l.pos < l.len) { plat_putc(l.buf[l.pos]); l.pos++; }
            continue;
        }
        if (k == KEY_HOME || k == 1) { back(l.pos); l.pos = 0; continue; }
        if (k == KEY_END || k == 5) {
            while (l.pos < l.len) { plat_putc(l.buf[l.pos]); l.pos++; }
            continue;
        }
        if (k == 11) {                       /* ^K: kill to end of line */
            l.len = l.pos;
            redraw(&l);
            continue;
        }
        if (k == 21) {                       /* ^U: kill the whole line */
            int col = l.pos;
            l.pos = l.len = 0;
            redraw_from(&l, col);
            continue;
        }
        if (k == 23) {                       /* ^W: kill the word behind */
            int e = l.pos, col = l.pos;
            while (l.pos > 0 && l.buf[l.pos - 1] == ' ') l.pos--;
            while (l.pos > 0 && l.buf[l.pos - 1] != ' ') l.pos--;
            memmove(l.buf + l.pos, l.buf + e, (size_t)(l.len - e));
            l.len -= e - l.pos;
            redraw_from(&l, col);
            continue;
        }
        if (k == 3) {                        /* ^C: abandon the line */
            l.pos = l.len;
            plat_puts("\n");
            plat_flush();
            buf[0] = 0;
            return 0;
        }
        if (k == 12) {                       /* ^L: clear the screen */
            plat_clear_screen();
            l.shown = 0;
            if (prompt) plat_puts(prompt);
            {
                int i;
                for (i = 0; i < l.len; i++) plat_putc(l.buf[i]);
            }
            l.shown = l.len;
            back(l.len - l.pos);
            continue;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            int want = hpos + (k == KEY_UP ? -1 : 1);
            if (want < 0) continue;
            if (want > hist_count) continue;
            if (hpos == hist_count) {        /* remember the unsent line */
                buf[l.len] = 0;
                snprintf(saved, sizeof saved, "%s", buf);
            }
            hpos = want;
            replace_line(&l, hpos == hist_count ? saved : hist[hpos]);
            continue;
        }
        if (k == KEY_PGUP) { plat_scrollback(1); continue; }
        if (k == KEY_PGDN) { plat_scrollback(-1); continue; }

        {
            const char *word = fkey_word(k);
            if (word) {
                if (l.len == 0) {              /* nothing typed: just run it */
                    set_text(&l, word);
                    { int i; for (i = 0; i < l.len; i++) plat_putc(l.buf[i]); }
                    l.shown = l.len;
                    plat_putc('\n');
                    buf[l.len] = 0;
                    cos_history_add(buf);
                    plat_flush();
                    return l.len;
                } else {                        /* otherwise type it in */
                    int col = l.pos;
                    insert_text(&l, word, (int)strlen(word));
                    redraw_from(&l, col);
                }
                continue;
            }
        }

        if (k >= 32 && k < 127 && l.len + 1 < cap) {
            char ch = (char)k;
            if (l.pos == l.len) {            /* the common case: appending */
                l.buf[l.len++] = ch;
                l.pos = l.len;
                l.shown = l.len;
                plat_putc(ch);
            } else {
                int col = l.pos;             /* where the cursor still is */
                insert_text(&l, &ch, 1);
                redraw_from(&l, col);
            }
        }
    }
}
