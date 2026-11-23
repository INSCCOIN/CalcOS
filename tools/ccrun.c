/* Host driver: compile a .c file and dump the result for the Unicorn harness.
 *
 *   ccrun <file.c>
 *
 * Prints a small text header followed by hex blobs, which tests/run_cc.py
 * loads into an emulated Cortex-M33.
 */
#include "cc.h"
#include "cprog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_BASE 0x20010000u
#define DATA_BASE 0x20030000u
#define RT_BASE   0x08000000u

/* The runtime symbols compiled code may call come from the firmware's own
 * table, so this tool and the device cannot disagree about what exists or
 * what signature it has. Only the addresses differ: on the device they are
 * real firmware addresses, here each gets a synthetic one the harness
 * hooks. */
static uint8_t code[64 * 1024];
static uint8_t data[32 * 1024];
static char    arena[512 * 1024];
static char    src[128 * 1024];

/* #include "x.h" resolves beside the source file, which is what the device
 * does too: there both live in the card's root. */
static int host_readfile(void *ud, const char *name, char *buf, int cap)
{
    const char *srcpath = (const char *)ud;
    char path[512];
    const char *slash = strrchr(srcpath, '/');
    FILE *f;
    int n;

    if (slash) {
        int dirlen = (int)(slash - srcpath) + 1;
        if (dirlen > (int)sizeof path - 1) dirlen = (int)sizeof path - 1;
        memcpy(path, srcpath, (size_t)dirlen);
        snprintf(path + dirlen, sizeof path - (size_t)dirlen, "%s", name);
    } else {
        snprintf(path, sizeof path, "%s", name);
    }
    f = fopen(path, "rb");
    if (!f) return -1;
    n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    static CC cc;
    static RtSym rt[256];
    const RtSym *real;
    int nrt = 0, i;
    FILE *f;
    size_t n;

    if (argc < 2) { fprintf(stderr, "usage: ccrun <file.c>\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    n = fread(src, 1, sizeof src - 1, f);
    src[n] = 0;
    fclose(f);

    real = cprog_rt_table(&nrt);
    if (nrt > (int)(sizeof rt / sizeof rt[0])) nrt = (int)(sizeof rt / sizeof rt[0]);
    for (i = 0; i < nrt; i++) {
        rt[i] = real[i];
        rt[i].fn = (void *)(uintptr_t)(RT_BASE + (uint32_t)(i * 4));
    }

    cc.arena = arena;
    cc.arena_cap = sizeof arena;
    cc.readfile = host_readfile;
    cc.readfile_ud = argv[1];

    if (cc_compile(&cc, src, argv[1], code, sizeof code, CODE_BASE,
                   data, sizeof data, DATA_BASE, rt, nrt) != 0) {
        fprintf(stderr, "%s:%d: error: %s\n", argv[1], cc.errline, cc.err);
        return 1;
    }

    printf("code_base %08x\n", CODE_BASE);
    printf("data_base %08x\n", DATA_BASE);
    printf("rt_base %08x\n", RT_BASE);
    for (i = 0; i < nrt; i++)
        printf("rt %s %08x\n", rt[i].name, (unsigned)(uintptr_t)rt[i].fn);
    {
        Sym *s;
        for (s = cc.globals; s; s = s->next) {
            if (s->kind == S_FUNC && s->defined && s->code_off >= 0)
                printf("fn %s %08x\n", s->name, CODE_BASE + (uint32_t)s->code_off);
            else if (s->kind == S_GLOBAL && s->addr)
                printf("var %s %08x %d\n", s->name, s->addr, s->type ? s->type->size : 0);
        }
    }
    printf("arena %d of %d\n", cc.arena_used, (int)sizeof arena);
    printf("code %d\n", cc.code.len);
    for (i = 0; i < cc.code.len; i++) printf("%02x", code[i]);
    printf("\n");
    printf("data %d\n", cc.data_len);
    for (i = 0; i < cc.data_len; i++) printf("%02x", data[i]);
    printf("\n");
    return 0;
}
