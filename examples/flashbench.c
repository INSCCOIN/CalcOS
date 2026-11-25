/* Tables in flash, and how fast they really are.
 *
 *     cc /flashbench.c
 *     run
 *
 * The flash store is memory-mapped, so a table kept there is read with a
 * load instruction and costs no SRAM at all. The catch is that it is read
 * through the XIP cache: a working set that fits the cache is nearly as
 * quick as SRAM, and one that does not pays for a flash fetch every time.
 *
 * Rather than assert where that line falls, this measures it. It builds a
 * 64 KB sine table in the store, then times random reads over working sets
 * from 1 KB to 64 KB. The point where the cost per read jumps is the cache
 * size, measured on your board rather than taken from a datasheet. The
 * same sweep is run over a table in SRAM for comparison.
 *
 * After 'run' these are callable from the prompt:
 *
 *     build()            write the table (only needed once; it survives)
 *     reads(20000)       fewer reads per measurement, for a quicker pass
 *     bench()            run the whole sweep again
 *     flash_ns(16)       ns per random read over a 16 KB working set
 *     ram_ns(16)         the same from SRAM
 *     lookup(1000)       one entry, read straight out of flash
 *     verify()           check the table against sin()
 *
 * A warning about where it writes. The C interface to the store is by page
 * number, not by name, so this claims the last 16 pages for itself. Named
 * objects are allocated from the low end, so a collision needs the store to
 * be nearly full — but if you have been using 'store put', run 'store map'
 * first and make sure the top of the store is free.
 */
#include <stdio.h>

#define PAGE_BYTES  4096
#define TABLE_KB    64
#define TABLE_PAGES (TABLE_KB * 1024 / PAGE_BYTES)     /* 16 */
#define ENTRIES     (TABLE_KB * 1024 / 4)              /* 16384 ints */

#define RAM_KB      16
#define RAM_ENTRIES (RAM_KB * 1024 / 4)                /* 4096 ints */

#define MAGIC       1414676529                         /* 'TBL1' */
#define READS       60000            /* per measurement, by default */

static int ram_table[RAM_ENTRIES];
static int page_buf[PAGE_BYTES / 4];

/* How many reads each measurement makes. The default is chosen so that a
 * measurement lasts a few milliseconds, which is long enough that the
 * microsecond timer's resolution does not show. Lower it from the prompt
 * with reads(n) for a quick look. */
static int nreads = READS;

double reads(double n)
{
    if (n >= 100.0) nreads = (int)n;
    return (double)nreads;
}

/* Where our pages start: the top of the store, away from the directory in
 * page 0 and away from where named objects are allocated. */
static int base_page(void)
{
    return store_pages() - TABLE_PAGES;
}

/* The store is one flat memory-mapped region, so consecutive pages are
 * consecutive addresses and the whole table can be addressed from the
 * pointer to its first page. */
static int *table(void)
{
    return (int *)store_read(base_page());
}

/* ------------------------------------------------------------------ */
/* building the table                                                  */
/* ------------------------------------------------------------------ */

/* Entry i is sin(2*pi*i/1024) scaled by 10000, which is the sort of thing
 * anyone actually precomputes. The table runs to 16 periods because the
 * benchmark needs 64 KB of it, not because anyone needs sixteen copies of
 * a sine.
 *
 * The very last entry holds a magic number instead, so a later run can
 * tell the table is already written and skip it: flash wears out, and
 * rewriting 16 pages every run would be careless. The last entry is used
 * rather than the first because entry 0 is the one a reader is most likely
 * to ask for, and it should be sin(0). */
#define STAMP_AT (ENTRIES - 1)

static int value_at(int i)
{
    if (i == STAMP_AT) return MAGIC;
    return (int)(10000.0 * sin(6.283185307179586 * (double)i / 1024.0));
}

int build(void)
{
    int page, i, t0 = millis();

    if (store_pages() == 0) {
        printf("no flash store on this board\n");
        return 0;
    }
    printf("writing %d KB over %d pages...\n", TABLE_KB, TABLE_PAGES);
    for (page = 0; page < TABLE_PAGES; page++) {
        int first = page * (PAGE_BYTES / 4);
        for (i = 0; i < PAGE_BYTES / 4; i++) page_buf[i] = value_at(first + i);
        if (!store_write(base_page() + page, page_buf, PAGE_BYTES)) {
            printf("page %d would not write\n", base_page() + page);
            return 0;
        }
    }
    printf("done in %d ms\n", millis() - t0);
    return 1;
}

static int is_built(void)
{
    int *t = table();
    if (!t) return 0;
    return t[STAMP_AT] == MAGIC;
}

/* Read a few entries back and compare them with sin() computed now. */
int verify(void)
{
    int *t = table();
    int i, bad = 0;
    if (!t) { printf("no store\n"); return 0; }
    /* entry 0, then a spread of others, then the last real one */
    for (i = 0; i < ENTRIES - 1; i = i * 3 + 1) {
        if (t[i] != value_at(i)) {
            printf("entry %d: %d, expected %d\n", i, t[i], value_at(i));
            bad++;
        }
    }
    if (t[ENTRIES - 2] != value_at(ENTRIES - 2)) bad++;
    if (bad) printf("%d entries wrong\n", bad);
    else     printf("table matches sin()\n");
    return bad == 0;
}

/* One entry, straight out of flash. Callable from the prompt. */
double lookup(double n)
{
    int *t = table();
    int i = (int)n;
    if (!t || i < 0 || i >= STAMP_AT) return 0.0;
    return (double)t[i] / 10000.0;
}

/* ------------------------------------------------------------------ */
/* the measurement                                                     */
/* ------------------------------------------------------------------ */

/* A cheap generator, so the address pattern defeats any prefetching and
 * the cost measured is a real lookup rather than a march through memory. */
static unsigned seed = 12345;

static int next_index(int mask)
{
    seed = seed * 1103515245 + 12345;
    return (int)((seed >> 9) & (unsigned)mask);
}

/* Time the loop without the read, so the generator, the loop and the timer
 * can be subtracted out. Without this, a fast read looks like whatever the
 * loop overhead happens to be. */
static int overhead_us(int mask)
{
    int k, t0, acc = 0;
    seed = 12345;
    t0 = micros();
    for (k = 0; k < nreads; k++) acc += next_index(mask);
    /* acc is returned so nothing here can be discarded as unused */
    return (micros() - t0) + (acc & 0);
}

static int flash_us(int mask)
{
    int *t = table();
    int k, t0, acc = 0;
    seed = 12345;
    t0 = micros();
    for (k = 0; k < nreads; k++) acc += t[next_index(mask)];
    return (micros() - t0) + (acc & 0);
}

static int ram_us(int mask)
{
    int k, t0, acc = 0;
    seed = 12345;
    t0 = micros();
    for (k = 0; k < nreads; k++) acc += ram_table[next_index(mask)];
    return (micros() - t0) + (acc & 0);
}

/* Nanoseconds per read over a working set of kb kilobytes. */
double flash_ns(double kb)
{
    int entries = (int)kb * 256;
    int us;
    if (entries < 1 || entries > ENTRIES) return -1.0;
    us = flash_us(entries - 1) - overhead_us(entries - 1);
    if (us < 0) us = 0;
    return (double)us * 1000.0 / (double)nreads;
}

double ram_ns(double kb)
{
    int entries = (int)kb * 256;
    int us;
    if (entries < 1 || entries > RAM_ENTRIES) return -1.0;
    us = ram_us(entries - 1) - overhead_us(entries - 1);
    if (us < 0) us = 0;
    return (double)us * 1000.0 / (double)nreads;
}

/* Sequential reads, for comparison: a cache line fetched for one miss
 * serves the next several, so this should be far quicker than random. */
double flash_seq_ns(void)
{
    int *t = table();
    int k, i = 0, t0, acc = 0;
    double us;
    t0 = micros();
    for (k = 0; k < nreads; k++) {
        acc += t[i];
        i++;
        if (i >= ENTRIES) i = 0;
    }
    us = (double)(micros() - t0) + (double)(acc & 0);
    return us * 1000.0 / (double)nreads;
}

int bench(void)
{
    int kb;

    if (!is_built()) {
        printf("table not there yet; building it\n");
        if (!build()) return 0;
    }

    for (kb = 0; kb < RAM_ENTRIES; kb++) ram_table[kb] = value_at(kb);

    printf("\n  set     flash      ram\n");
    printf("   KB     ns/rd    ns/rd\n");
    for (kb = 1; kb <= TABLE_KB; kb = kb * 2) {
        double f = flash_ns((double)kb);
        double r = (kb <= RAM_KB) ? ram_ns((double)kb) : -1.0;
        if (r >= 0.0) printf("%5d %9.1f %8.1f\n", kb, f, r);
        else          printf("%5d %9.1f        -\n", kb, f);
    }
    printf("\nsequential %.1f ns/read\n", flash_seq_ns());
    printf("the jump in the flash column is the\n");
    printf("cache running out\n");
    return 1;
}

int main(void)
{
    printf("flash table benchmark\n");
    if (store_pages() == 0) {
        printf("no flash store on this board\n");
        return 1;
    }
    printf("store: %d pages of %d bytes\n", store_pages(), store_page_size());
    printf("table: pages %d..%d\n", base_page(), base_page() + TABLE_PAGES - 1);

    if (!is_built()) {
        if (!build()) return 1;
    } else {
        printf("table already in flash, reusing it\n");
    }
    verify();
    bench();
    printf("\nfrom the prompt: build() verify() bench()\n");
    printf("lookup(n) flash_ns(kb) ram_ns(kb)\n");
    printf("reads(n) to change the sample size\n");
    return 0;
}
