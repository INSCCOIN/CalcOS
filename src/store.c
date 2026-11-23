/* Named objects in the flash store.
 *
 * The raw store is 128 pages of 4 KB with nothing but numbers. That is
 * workable for one table and hopeless for a working set: you have to
 * remember which page held what, and nothing tells you which pages are
 * free. This adds a directory in page 0 and lets everything else be found
 * by name.
 *
 * The layout is deliberately dull. Page 0 holds a header and a flat array
 * of entries; every object occupies a run of whole pages; allocation is
 * first fit. There is no fragmentation handling beyond that, because a
 * store with 127 data pages and a handful of objects does not need any, and
 * a compaction pass nobody can watch is a good way to lose data.
 *
 * Replacing an object is crash-safe. The new copy is written to pages the
 * old one is not using, and only then does the directory change to point at
 * it. If the power goes between the two, the old object is still there and
 * intact. The cost is that replacing an object needs room for both copies,
 * which is the right trade on a part where a half-written directory would
 * lose everything.
 *
 * Reading costs nothing: store_find hands back a pointer into memory-mapped
 * flash, so an object is read in place and occupies no RAM at all.
 */
#include "calc.h"
#include "store.h"
#include <string.h>
#include <stdio.h>

#define STORE_MAGIC  "CSTORE1"
#define DIR_PAGE     0

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t npages;
    uint32_t nent;
    uint32_t reserved[5];
} DirHdr;                       /* 32 bytes */

typedef struct {
    char     name[STORE_NAMELEN];   /* NUL padded; empty means a free slot */
    uint16_t page;
    uint16_t pages;
    uint32_t len;
    uint32_t kind;
    uint32_t reserved;
} DirEnt;                       /* 48 bytes */

/* One page of directory, held in RAM while it is being changed. The store
 * is only written from the foreground, so a single buffer is enough. */
static uint8_t dirbuf[STORE_PAGE_MAX];
static int     dir_loaded;
static int     dir_valid;

static int page_size(void) { return plat_store_page_size(); }
static int page_count(void) { return plat_store_pages(); }

static DirHdr *hdr(void) { return (DirHdr *)dirbuf; }
static DirEnt *ents(void) { return (DirEnt *)(dirbuf + sizeof(DirHdr)); }

static int max_entries(void)
{
    int n = (page_size() - (int)sizeof(DirHdr)) / (int)sizeof(DirEnt);
    return n > STORE_MAXENT ? STORE_MAXENT : n;
}

/* Read the directory into RAM, or start an empty one if there is no valid
 * directory there. An unformatted store reads as all 0xFF, which fails the
 * magic check and is simply treated as empty. */
static int load_dir(void)
{
    const void *p;
    if (dir_loaded) return dir_valid;
    dir_loaded = 1;
    dir_valid = 0;
    if (page_count() < 2 || page_size() > STORE_PAGE_MAX) return 0;

    p = plat_store_read(DIR_PAGE);
    if (!p) return 0;
    memcpy(dirbuf, p, (size_t)page_size());
    if (memcmp(hdr()->magic, STORE_MAGIC, 8) != 0
        || hdr()->version != 1
        || hdr()->nent > (uint32_t)max_entries()) {
        /* no directory yet: present an empty one without writing anything */
        memset(dirbuf, 0, (size_t)page_size());
        memcpy(hdr()->magic, STORE_MAGIC, 8);
        hdr()->version = 1;
        hdr()->npages = (uint32_t)page_count();
        hdr()->nent = 0;
    }
    dir_valid = 1;
    return 1;
}

static int commit_dir(void)
{
    if (!plat_store_write(DIR_PAGE, dirbuf, page_size())) return 0;
    return 1;
}

/* Mark the pages an object occupies. Page 0 is always the directory. */
static void mark_used(uint8_t *used, const DirEnt *e)
{
    int i;
    for (i = 0; i < e->pages; i++)
        if (e->page + i < page_count()) used[e->page + i] = 1;
}

/* Find a run of `want` free pages. `skip` is an entry whose pages are to be
 * treated as busy even though it is about to be replaced, which is what
 * keeps the old copy intact until the new one is committed. */
static int find_run(int want, const DirEnt *keep_busy)
{
    static uint8_t used[STORE_MAXPAGES];
    int i, run = 0, n = page_count();
    uint32_t k;

    if (want <= 0 || n > STORE_MAXPAGES) return -1;
    memset(used, 0, (size_t)n);
    used[DIR_PAGE] = 1;
    for (k = 0; k < hdr()->nent; k++)
        if (ents()[k].name[0]) mark_used(used, &ents()[k]);
    if (keep_busy) mark_used(used, keep_busy);

    for (i = 0; i < n; i++) {
        if (used[i]) { run = 0; continue; }
        if (++run == want) return i - want + 1;
    }
    return -1;
}

static DirEnt *find_ent(const char *name)
{
    uint32_t i;
    for (i = 0; i < hdr()->nent; i++)
        if (ents()[i].name[0] && strncmp(ents()[i].name, name, STORE_NAMELEN) == 0)
            return &ents()[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* the interface                                                       */
/* ------------------------------------------------------------------ */

int store_available(void) { return page_count() > 1; }

int store_format(void)
{
    int i, n = page_count();
    if (!store_available()) return 0;
    for (i = n - 1; i >= 0; i--) plat_store_erase(i);
    memset(dirbuf, 0, (size_t)page_size());
    memcpy(hdr()->magic, STORE_MAGIC, 8);
    hdr()->version = 1;
    hdr()->npages = (uint32_t)n;
    hdr()->nent = 0;
    dir_loaded = 1;
    dir_valid = 1;
    return commit_dir();
}

int store_count(void)
{
    uint32_t i;
    int n = 0;
    if (!load_dir()) return 0;
    for (i = 0; i < hdr()->nent; i++) if (ents()[i].name[0]) n++;
    return n;
}

int store_entry(int idx, const char **name, int *len, int *kind, int *page,
                int *pages)
{
    uint32_t i;
    int n = 0;
    if (!load_dir()) return 0;
    for (i = 0; i < hdr()->nent; i++) {
        if (!ents()[i].name[0]) continue;
        if (n++ != idx) continue;
        if (name) *name = ents()[i].name;
        if (len) *len = (int)ents()[i].len;
        if (kind) *kind = (int)ents()[i].kind;
        if (page) *page = ents()[i].page;
        if (pages) *pages = ents()[i].pages;
        return 1;
    }
    return 0;
}

const void *store_find(const char *name, int *len, int *kind)
{
    DirEnt *e;
    if (!load_dir()) return NULL;
    e = find_ent(name);
    if (!e) return NULL;
    if (len) *len = (int)e->len;
    if (kind) *kind = (int)e->kind;
    return plat_store_read(e->page);
}

int store_free_pages(void)
{
    static uint8_t used[STORE_MAXPAGES];
    int i, n = page_count(), free_n = 0;
    uint32_t k;
    if (!load_dir() || n > STORE_MAXPAGES) return 0;
    memset(used, 0, (size_t)n);
    used[DIR_PAGE] = 1;
    for (k = 0; k < hdr()->nent; k++)
        if (ents()[k].name[0]) mark_used(used, &ents()[k]);
    for (i = 0; i < n; i++) if (!used[i]) free_n++;
    return free_n;
}

int store_put(const char *name, const void *data, int len, int kind)
{
    int psz = page_size(), want, start, i;
    DirEnt *old, *slot = NULL;
    DirEnt saved;
    uint32_t k;

    if (!load_dir() || !name || !*name || len < 0) return STORE_ERR;
    if ((int)strlen(name) >= STORE_NAMELEN) return STORE_ENAME;

    want = (len + psz - 1) / psz;
    if (want == 0) want = 1;

    old = find_ent(name);
    if (old) saved = *old;

    start = find_run(want, old);
    if (start < 0) return STORE_EFULL;

    /* the data first, so a failure here leaves the old object in place */
    for (i = 0; i < want; i++) {
        int off = i * psz;
        int n = len - off;
        if (n > psz) n = psz;
        if (n < 0) n = 0;
        if (!plat_store_write(start + i, (const char *)data + off, n))
            return STORE_EIO;
    }

    /* then the directory, which is what makes it visible */
    if (old) {
        slot = old;
    } else {
        for (k = 0; k < hdr()->nent; k++)
            if (!ents()[k].name[0]) { slot = &ents()[k]; break; }
        if (!slot) {
            if ((int)hdr()->nent >= max_entries()) return STORE_EFULL;
            slot = &ents()[hdr()->nent];
            hdr()->nent++;
        }
    }
    memset(slot, 0, sizeof *slot);
    snprintf(slot->name, STORE_NAMELEN, "%s", name);
    slot->page = (uint16_t)start;
    slot->pages = (uint16_t)want;
    slot->len = (uint32_t)len;
    slot->kind = (uint32_t)kind;

    if (!commit_dir()) {
        if (old) *slot = saved;          /* put the directory back as it was */
        return STORE_EIO;
    }

    /* the pages the old copy used are now unreferenced; erasing them is
     * tidiness rather than correctness, and it costs wear, so it is left
     * for 'store format' or the next allocation to reuse */
    return STORE_OK;
}

int store_delete(const char *name)
{
    DirEnt *e;
    if (!load_dir()) return STORE_ERR;
    e = find_ent(name);
    if (!e) return STORE_ENOENT;
    memset(e, 0, sizeof *e);
    return commit_dir() ? STORE_OK : STORE_EIO;
}

const char *store_kind_name(int kind)
{
    switch (kind) {
    case STORE_RAW:   return "data";
    case STORE_PROG:  return "program";
    case STORE_IMAGE: return "image";
    case STORE_TEXT:  return "text";
    default:          return "?";
    }
}

void store_forget(void) { dir_loaded = 0; }
