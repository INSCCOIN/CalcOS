/* SD card over SPI plus a FAT16/FAT32 reader and a careful writer.
 *
 * Reading handles long file names, subdirectories and MBR-partitioned cards.
 * Writing is deliberately narrow: whole files only, 8.3 names only, into an
 * existing directory. It never moves or renames anything, and it rewrites
 * every copy of the FAT, so a half-finished write leaves a consistent
 * filesystem rather than a corrupt one. The write path is checked on a
 * workstation against real images with fsck.vfat, because this is the part
 * of the system that could damage something the user cares about.
 */
#include "picocalc.h"
#include <string.h>
#include <stdio.h>

/* The FAT reader is hardware-independent: it only needs sd_read_block().
 * Defining FAT_HOST_TEST leaves the SD/SPI half out so the filesystem can
 * be exercised against a disk image on a workstation. */
#ifndef FAT_HOST_TEST
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#endif

#ifndef FAT_HOST_TEST

#define SD_SPI   spi0
#define SD_MISO  16
#define SD_CS    17
#define SD_SCK   18
#define SD_MOSI  19

#define SD_SLOW_HZ  (400 * 1000)
#define SD_FAST_HZ  (12 * 1000 * 1000)

static int sd_ok;
static int sd_hc;              /* high capacity: address in blocks */

/* ------------------------------------------------------------------ */
/* low level                                                           */
/* ------------------------------------------------------------------ */

static inline void cs_lo(void) { gpio_put(SD_CS, 0); }
static inline void cs_hi(void) { gpio_put(SD_CS, 1); }

static uint8_t xchg(uint8_t v)
{
    uint8_t r = 0xFF;
    spi_write_read_blocking(SD_SPI, &v, &r, 1);
    return r;
}

static void clock_bytes(int n)
{
    while (n-- > 0) xchg(0xFF);
}

static uint8_t wait_ready(int tries)
{
    uint8_t r;
    do { r = xchg(0xFF); } while (r != 0xFF && --tries > 0);
    return r;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t buf[6], r;
    int i;
    buf[0] = (uint8_t)(0x40 | cmd);
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)arg;
    buf[5] = (cmd == 0) ? 0x95 : (cmd == 8 ? 0x87 : 0x01);   /* CRC7 */
    for (i = 0; i < 6; i++) xchg(buf[i]);
    for (i = 0; i < 10; i++) {
        r = xchg(0xFF);
        if (!(r & 0x80)) return r;
    }
    return 0xFF;
}

int sd_init(void)
{
    int i;
    uint8_t r;
    if (sd_ok) return 1;

    gpio_init(SD_CS);
    gpio_set_dir(SD_CS, GPIO_OUT);
    cs_hi();
    spi_init(SD_SPI, SD_SLOW_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO, GPIO_FUNC_SPI);

    sd_ok = 0;
    sd_hc = 0;

    clock_bytes(10);                       /* >=74 clocks with CS high */

    cs_lo();
    for (i = 0; i < 16; i++) {
        if (sd_cmd(0, 0) == 1) break;
        sleep_ms(2);
    }
    if (i == 16) { cs_hi(); return 0; }

    r = sd_cmd(8, 0x1AA);
    if (r == 1) {
        uint8_t v[4];
        for (i = 0; i < 4; i++) v[i] = xchg(0xFF);
        if (v[2] != 0x01 || v[3] != 0xAA) { cs_hi(); return 0; }
        for (i = 0; i < 20000; i++) {
            sd_cmd(55, 0);
            if (sd_cmd(41, 0x40000000) == 0) break;
            sleep_ms(1);
        }
        if (i == 20000) { cs_hi(); return 0; }
        if (sd_cmd(58, 0) == 0) {
            uint8_t o[4];
            for (i = 0; i < 4; i++) o[i] = xchg(0xFF);
            sd_hc = (o[0] & 0x40) ? 1 : 0;
        }
    } else {
        /* SD v1 or MMC */
        for (i = 0; i < 20000; i++) {
            sd_cmd(55, 0);
            if (sd_cmd(41, 0) == 0) break;
            sleep_ms(1);
        }
        if (i == 20000) { cs_hi(); return 0; }
        sd_cmd(16, 512);
    }
    cs_hi();
    clock_bytes(1);

    spi_set_baudrate(SD_SPI, SD_FAST_HZ);
    sd_ok = 1;
    return 1;
}

/* The SPI divider is derived from the system clock, so after the clock
 * changes the card has to be re-timed or transfers run at the wrong rate. */
void sd_reclock(void)
{
    if (sd_ok) spi_set_baudrate(SD_SPI, SD_FAST_HZ);
}

int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    uint8_t r;
    int tries = 40000;
    if (!sd_ok) return 0;
    cs_lo();
    wait_ready(50000);
    if (sd_cmd(24, sd_hc ? lba : lba * 512u) != 0) { cs_hi(); return 0; }
    xchg(0xFF);
    xchg(0xFE);                            /* start block token */
    spi_write_blocking(SD_SPI, buf, 512);
    xchg(0xFF);
    xchg(0xFF);                            /* dummy CRC */
    r = xchg(0xFF);
    if ((r & 0x1F) != 0x05) { cs_hi(); return 0; }   /* data accepted? */
    /* the card holds MISO low while it programs the block */
    do { r = xchg(0xFF); } while (r == 0x00 && --tries > 0);
    cs_hi();
    clock_bytes(1);
    return tries > 0;
}

int sd_read_block(uint32_t lba, uint8_t *buf)
{
    uint8_t tok;
    int tries = 40000;
    if (!sd_ok) return 0;
    cs_lo();
    wait_ready(50000);
    if (sd_cmd(17, sd_hc ? lba : lba * 512u) != 0) { cs_hi(); return 0; }
    do { tok = xchg(0xFF); } while (tok == 0xFF && --tries > 0);
    if (tok != 0xFE) { cs_hi(); return 0; }
    {
        /* a burst read is far quicker than byte-at-a-time */
        memset(buf, 0xFF, 512);
        spi_read_blocking(SD_SPI, 0xFF, buf, 512);
    }
    xchg(0xFF);
    xchg(0xFF);                            /* discard CRC16 */
    cs_hi();
    clock_bytes(1);
    return 1;
}

#endif /* !FAT_HOST_TEST */

/* ------------------------------------------------------------------ */
/* FAT                                                                 */
/* ------------------------------------------------------------------ */

static struct {
    int      mounted;
    int      fat32;
    uint32_t fat_start;
    uint32_t data_start;
    uint32_t root_cluster;   /* FAT32 */
    uint32_t root_start;     /* FAT16 */
    uint32_t root_entries;   /* FAT16 */
    uint32_t sec_per_clus;
    uint32_t total_clusters;
    uint32_t nfats;
    uint32_t sec_per_fat;
    uint32_t fsinfo_sec;     /* FAT32 only, 0 if absent */
} fs;

static uint8_t secbuf[512];
static uint8_t fatbuf[512];
static uint32_t fatbuf_sec = 0xFFFFFFFFu;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fat_mounted(void) { return fs.mounted; }

int fat_mount(void)
{
    uint32_t base = 0;
    uint32_t sec_per_fat, reserved, nfats, root_ent, total_sec;

    fs.mounted = 0;
    fatbuf_sec = 0xFFFFFFFFu;
    if (!sd_init()) return 0;
    if (!sd_read_block(0, secbuf)) return 0;

    /* an MBR has a partition table; a bare volume has a BPB at sector 0 */
    if (secbuf[510] == 0x55 && secbuf[511] == 0xAA
        && !(secbuf[0] == 0xEB || secbuf[0] == 0xE9)) {
        base = rd32(&secbuf[446 + 8]);
        if (base == 0 || !sd_read_block(base, secbuf)) return 0;
    }
    if (rd16(&secbuf[11]) != 512) return 0;       /* 512-byte sectors only */

    fs.sec_per_clus = secbuf[13];
    reserved   = rd16(&secbuf[14]);
    nfats      = secbuf[16];
    root_ent   = rd16(&secbuf[17]);
    total_sec  = rd16(&secbuf[19]);
    if (total_sec == 0) total_sec = rd32(&secbuf[32]);
    sec_per_fat = rd16(&secbuf[22]);
    if (sec_per_fat == 0) {
        sec_per_fat = rd32(&secbuf[36]);
        fs.fat32 = 1;
    } else fs.fat32 = 0;

    if (!fs.sec_per_clus || !nfats || !reserved) return 0;

    fs.fat_start = base + reserved;
    fs.root_entries = root_ent;
    fs.nfats = nfats;
    fs.sec_per_fat = sec_per_fat;
    fs.fsinfo_sec = 0;
    if (fs.fat32) {
        uint16_t fsi = rd16(&secbuf[48]);
        if (fsi && fsi != 0xFFFF) fs.fsinfo_sec = base + fsi;
        fs.root_cluster = rd32(&secbuf[44]);
        fs.data_start = fs.fat_start + nfats * sec_per_fat;
        fs.root_start = 0;
    } else {
        fs.root_start = fs.fat_start + nfats * sec_per_fat;
        fs.data_start = fs.root_start + (root_ent * 32 + 511) / 512;
        fs.root_cluster = 0;
    }
    fs.total_clusters = (total_sec - (fs.data_start - base)) / fs.sec_per_clus + 2;
    fs.mounted = 1;
    return 1;
}

static uint32_t clus_sector(uint32_t clus)
{
    return fs.data_start + (clus - 2) * fs.sec_per_clus;
}

static uint32_t next_cluster(uint32_t clus)
{
    uint32_t off = fs.fat32 ? clus * 4 : clus * 2;
    uint32_t sec = fs.fat_start + off / 512;
    uint32_t idx = off % 512;
    if (sec != fatbuf_sec) {
        if (!sd_read_block(sec, fatbuf)) return 0x0FFFFFFF;
        fatbuf_sec = sec;
    }
    if (fs.fat32) return rd32(&fatbuf[idx]) & 0x0FFFFFFF;
    return rd16(&fatbuf[idx]);
}

static int is_eoc(uint32_t c)
{
    return fs.fat32 ? (c >= 0x0FFFFFF8) : (c >= 0xFFF8);
}

/* assemble a long file name from the 0x0F entries preceding an 8.3 entry */
static void lfn_chars(const uint8_t *e, char *out, int *n)
{
    static const int off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    int i;
    for (i = 0; i < 13 && *n < 254; i++) {
        uint16_t c = rd16(&e[off[i]]);
        if (c == 0 || c == 0xFFFF) break;
        out[(*n)++] = (c < 128) ? (char)c : '?';
    }
}

static void name_83(const uint8_t *e, char *out)
{
    int i, n = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = 0;
    for (i = 0; i < n; i++)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Walk a directory. For each entry, call back with the name and the raw
 * 32-byte record. Returning non-zero from the callback stops the walk. */
typedef int (*dir_cb)(const char *name, const uint8_t *ent, void *ctx);

static int walk_dir(uint32_t clus, dir_cb cb, void *ctx)
{
    char lfn[260];
    int lfn_len = 0;
    uint32_t sec, i;
    int e;

    if (!fs.fat32 && clus == 0) {
        /* FAT16 root is a flat run of sectors */
        uint32_t nsec = (fs.root_entries * 32 + 511) / 512;
        for (i = 0; i < nsec; i++) {
            if (!sd_read_block(fs.root_start + i, secbuf)) return 0;
            for (e = 0; e < 16; e++) {
                uint8_t *ent = &secbuf[e * 32];
                char name[260];
                if (ent[0] == 0x00) return 0;
                if (ent[0] == 0xE5) { lfn_len = 0; continue; }
                if ((ent[11] & 0x0F) == 0x0F) {
                    char part[32];
                    int pn = 0;
                    lfn_chars(ent, part, &pn);
                    if (lfn_len + pn < 255) {
                        memmove(lfn + pn, lfn, (size_t)lfn_len);
                        memcpy(lfn, part, (size_t)pn);
                        lfn_len += pn;
                    }
                    continue;
                }
                if (ent[11] & 0x08) { lfn_len = 0; continue; }   /* volume id */
                if (lfn_len) { memcpy(name, lfn, (size_t)lfn_len); name[lfn_len] = 0; }
                else name_83(ent, name);
                lfn_len = 0;
                if (cb(name, ent, ctx)) return 1;
            }
        }
        return 0;
    }

    while (clus >= 2 && !is_eoc(clus)) {
        sec = clus_sector(clus);
        for (i = 0; i < fs.sec_per_clus; i++) {
            if (!sd_read_block(sec + i, secbuf)) return 0;
            for (e = 0; e < 16; e++) {
                uint8_t *ent = &secbuf[e * 32];
                char name[260];
                if (ent[0] == 0x00) return 0;
                if (ent[0] == 0xE5) { lfn_len = 0; continue; }
                if ((ent[11] & 0x0F) == 0x0F) {
                    char part[32];
                    int pn = 0;
                    lfn_chars(ent, part, &pn);
                    if (lfn_len + pn < 255) {
                        memmove(lfn + pn, lfn, (size_t)lfn_len);
                        memcpy(lfn, part, (size_t)pn);
                        lfn_len += pn;
                    }
                    continue;
                }
                if (ent[11] & 0x08) { lfn_len = 0; continue; }
                if (lfn_len) { memcpy(name, lfn, (size_t)lfn_len); name[lfn_len] = 0; }
                else name_83(ent, name);
                lfn_len = 0;
                if (cb(name, ent, ctx)) return 1;
            }
        }
        clus = next_cluster(clus);
    }
    return 0;
}

struct find_ctx {
    const char *want;
    uint32_t    clus;
    uint32_t    size;
    int         isdir;
    int         found;
};

static int find_cb(const char *name, const uint8_t *ent, void *vctx)
{
    struct find_ctx *c = (struct find_ctx *)vctx;
    if (!ieq(name, c->want)) return 0;
    c->clus = ((uint32_t)rd16(&ent[20]) << 16) | rd16(&ent[26]);
    c->size = rd32(&ent[28]);
    c->isdir = (ent[11] & 0x10) ? 1 : 0;
    c->found = 1;
    return 1;
}

/* Resolve a path such as "/src/main.c" to its starting cluster. */
static int resolve(const char *path, uint32_t *clus, uint32_t *size, int *isdir)
{
    uint32_t cur = fs.fat32 ? fs.root_cluster : 0;
    char part[64];
    int n;

    *isdir = 1;
    *size = 0;
    while (*path == '/') path++;
    if (!*path) { *clus = cur; return 1; }

    for (;;) {
        struct find_ctx c;
        n = 0;
        while (*path && *path != '/' && n < 63) part[n++] = *path++;
        part[n] = 0;
        while (*path == '/') path++;

        memset(&c, 0, sizeof c);
        c.want = part;
        walk_dir(cur, find_cb, &c);
        if (!c.found) return 0;
        cur = c.clus;
        *size = c.size;
        *isdir = c.isdir;
        if (!*path) { *clus = cur; return 1; }
        if (!c.isdir) return 0;
    }
}

int fat_open(FatFile *f, const char *path)
{
    uint32_t clus, size;
    int isdir;
    memset(f, 0, sizeof *f);
    if (!fs.mounted && !fat_mount()) return 0;
    if (!resolve(path, &clus, &size, &isdir)) return 0;
    if (isdir) return 0;
    f->cluster = clus;
    f->cur_cluster = clus;
    f->size = size;
    f->pos = 0;
    f->cur_off = 0;
    f->valid = 1;
    return 1;
}

int fat_read(FatFile *f, void *vbuf, int len)
{
    uint8_t *buf = (uint8_t *)vbuf;
    int got = 0;
    if (!f->valid) return 0;
    /* cur_off is always the offset within the current cluster */
    while (got < len && f->pos < f->size) {
        uint32_t clus_bytes = fs.sec_per_clus * 512u;
        uint32_t sec = clus_sector(f->cur_cluster) + f->cur_off / 512u;
        uint32_t off = f->cur_off % 512u;
        uint32_t take = 512u - off;
        uint32_t left = f->size - f->pos;

        if (f->cur_cluster < 2 || is_eoc(f->cur_cluster)) break;
        if (!sd_read_block(sec, secbuf)) break;

        if (take > (uint32_t)(len - got)) take = (uint32_t)(len - got);
        if (take > left) take = left;
        memcpy(buf + got, secbuf + off, take);
        got += (int)take;
        f->pos += take;
        f->cur_off += take;
        if (f->cur_off >= clus_bytes) {
            f->cur_cluster = next_cluster(f->cur_cluster);
            f->cur_off = 0;
        }
    }
    return got;
}

/* Seek by walking the cluster chain from the start. Files here are small
 * and seeks are rare, so a chain walk is cheaper than caching the map. */
int fat_seek(FatFile *f, uint32_t pos)
{
    uint32_t clus_bytes, want;
    if (!f->valid) return 0;
    if (pos > f->size) pos = f->size;
    clus_bytes = fs.sec_per_clus * 512u;
    f->cur_cluster = f->cluster;
    f->cur_off = 0;
    f->pos = 0;
    for (want = pos; want >= clus_bytes; want -= clus_bytes) {
        uint32_t nxt = next_cluster(f->cur_cluster);
        if (nxt < 2 || is_eoc(nxt)) { want = 0; break; }
        f->cur_cluster = nxt;
        f->pos += clus_bytes;
    }
    f->cur_off = want;
    f->pos += want;
    return 1;
}

struct list_ctx { char *out; size_t cap; size_t used; };

static int list_cb(const char *name, const uint8_t *ent, void *vctx)
{
    struct list_ctx *c = (struct list_ctx *)vctx;
    size_t l = strlen(name);
    int isdir = (ent[11] & 0x10) ? 1 : 0;
    if (name[0] == '.') return 0;
    if (c->used + l + 3 >= c->cap) return 1;
    memcpy(c->out + c->used, name, l);
    c->used += l;
    if (isdir) c->out[c->used++] = '/';
    c->out[c->used++] = '\n';
    c->out[c->used] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/* "calcos.cfg" -> "CALCOS  CFG". Returns 0 for anything that will not fit
 * the 8.3 form; the writer refuses rather than inventing a ~1 name, because
 * guessing at a short name is how you end up clobbering someone's file. */
static int name_to_83(const char *name, char out[11])
{
    int i = 0, n = 0;
    memset(out, ' ', 11);
    while (name[i] && name[i] != '.') {
        char c = name[i++];
        if (n >= 8) return 0;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == '/' || c == '\\') return 0;
        out[n++] = c;
    }
    if (n == 0) return 0;
    if (name[i] == '.') {
        i++;
        n = 8;
        while (name[i]) {
            char c = name[i++];
            if (n >= 11) return 0;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[n++] = c;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* long names                                                          */
/* ------------------------------------------------------------------ */

/* How many 13-character slots a name needs, or 0 if the 8.3 form holds it
 * exactly. A name only needs long entries when it would not survive the
 * round trip through 8.3 — mixed case counts, because "Notes.txt" read
 * back from an 8.3 entry is "notes.txt". */
static int lfn_slots_for(const char *name)
{
    char n83[11], back[16];
    int len = (int)strlen(name);
    if (!name_to_83(name, n83)) return (len + 12) / 13;
    {   /* it fits the form; does it survive it? */
        uint8_t e[11];
        memcpy(e, n83, 11);
        name_83(e, back);
        if (ieq(back, name) && strcmp(back, name) == 0) return 0;
        if (ieq(back, name)) return (len + 12) / 13;   /* case differs */
    }
    return (len + 12) / 13;
}

/* The checksum that ties long-name slots to their 8.3 entry. */
static uint8_t lfn_checksum(const char n83[11])
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)n83[i]);
    return sum;
}

/* Build a short alias for a long name: the first six usable characters,
 * then ~N. The caller checks N for collisions. */
static void short_alias(const char *name, int n, char out[11])
{
    const char *dot = strrchr(name, '.');
    int i = 0, k = 0;
    char suffix[8];

    memset(out, ' ', 11);
    while (name[i] && (!dot || name + i < dot) && k < 6) {
        char c = name[i++];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 33 || c > 126 || strchr("\"*+,/:;<=>?[\\]|", c)) c = '_';
        out[k++] = c;
    }
    if (!k) out[k++] = '_';
    snprintf(suffix, sizeof suffix, "~%d", n);
    for (i = 0; suffix[i] && k < 8; i++) out[k++] = suffix[i];
    if (dot) {
        int e = 8;
        dot++;
        while (*dot && e < 11) {
            char c = *dot++;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[e++] = c;
        }
    }
}

/* FAT32 keeps a cached free-cluster count in the FSInfo sector. Rather than
 * track it incrementally and risk drift, mark it unknown after any change:
 * 0xFFFFFFFF is the spec's own "not known" value, so the next host to mount
 * the card recomputes it instead of trusting a stale number. */
static void fsinfo_invalidate(void)
{
    static uint8_t fsi[512];
    if (!fs.fat32 || !fs.fsinfo_sec) return;
    if (!sd_read_block(fs.fsinfo_sec, fsi)) return;
    if (rd32(&fsi[0]) != 0x41615252u || rd32(&fsi[484]) != 0x61417272u) return;
    memset(&fsi[488], 0xFF, 8);          /* free count and next free */
    sd_write_block(fs.fsinfo_sec, fsi);
}

/* Write one FAT entry into every copy of the FAT. */
static int fat_set(uint32_t clus, uint32_t val)
{
    uint32_t off = fs.fat32 ? clus * 4 : clus * 2;
    uint32_t rel = off / 512, idx = off % 512;
    uint32_t c;
    if (!sd_read_block(fs.fat_start + rel, fatbuf)) return 0;
    if (fs.fat32) {
        uint32_t old = rd32(&fatbuf[idx]);
        uint32_t nv = (old & 0xF0000000u) | (val & 0x0FFFFFFFu);
        fatbuf[idx + 0] = (uint8_t)nv;
        fatbuf[idx + 1] = (uint8_t)(nv >> 8);
        fatbuf[idx + 2] = (uint8_t)(nv >> 16);
        fatbuf[idx + 3] = (uint8_t)(nv >> 24);
    } else {
        fatbuf[idx + 0] = (uint8_t)val;
        fatbuf[idx + 1] = (uint8_t)(val >> 8);
    }
    for (c = 0; c < fs.nfats; c++)
        if (!sd_write_block(fs.fat_start + c * fs.sec_per_fat + rel, fatbuf))
            return 0;
    fatbuf_sec = 0xFFFFFFFFu;
    fsinfo_invalidate();
    return 1;
}

static uint32_t fat_get(uint32_t clus)
{
    return next_cluster(clus);
}

static int free_chain(uint32_t clus)
{
    while (clus >= 2 && !is_eoc(clus)) {
        uint32_t nx = fat_get(clus);
        if (!fat_set(clus, 0)) return 0;
        clus = nx;
    }
    return 1;
}

/* Allocate n clusters and link them. Returns the first, or 0 if the card is
 * full; on failure anything already taken is released. */
static uint32_t alloc_chain(uint32_t n)
{
    uint32_t first = 0, prev = 0, c;
    uint32_t eoc = fs.fat32 ? 0x0FFFFFFFu : 0xFFFFu;
    if (n == 0) n = 1;
    for (c = 2; c < fs.total_clusters && n > 0; c++) {
        if (fat_get(c) != 0) continue;
        if (!fat_set(c, eoc)) { if (first) free_chain(first); return 0; }
        if (prev && !fat_set(prev, c)) { free_chain(first); return 0; }
        if (!first) first = c;
        prev = c;
        n--;
    }
    if (n > 0) { if (first) free_chain(first); return 0; }
    return first;
}

/* ------------------------------------------------------------------ */
/* directory slots                                                     */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t sec, off; } Slot;

/* Visit every 32-byte directory entry in order. The callback gets the
 * entry's position and a pointer into the shared sector buffer, which it
 * must not hold on to; returning non-zero stops the walk. */
static int dir_iterate(uint32_t clus,
                       int (*cb)(uint32_t sec, uint32_t off, uint8_t *ent, void *ud),
                       void *ud)
{
    uint32_t sec, i;
    int e;

    if (!fs.fat32 && clus == 0) {
        uint32_t nsec = (fs.root_entries * 32 + 511) / 512;
        for (i = 0; i < nsec; i++) {
            if (!sd_read_block(fs.root_start + i, secbuf)) return -1;
            for (e = 0; e < 16; e++)
                if (cb(fs.root_start + i, (uint32_t)e * 32,
                       &secbuf[e * 32], ud))
                    return 1;
        }
        return 0;
    }
    while (clus >= 2 && !is_eoc(clus)) {
        sec = clus_sector(clus);
        for (i = 0; i < fs.sec_per_clus; i++) {
            if (!sd_read_block(sec + i, secbuf)) return -1;
            for (e = 0; e < 16; e++)
                if (cb(sec + i, (uint32_t)e * 32, &secbuf[e * 32], ud))
                    return 1;
        }
        clus = fat_get(clus);
    }
    return 0;
}

/* finding a free run of entries */
struct run_ctx {
    int   want, have;
    Slot  slot[21];                 /* 20 long slots plus the 8.3 entry */
    int   done;
};

static int run_cb(uint32_t sec, uint32_t off, uint8_t *ent, void *ud)
{
    struct run_ctx *c = (struct run_ctx *)ud;
    if (ent[0] != 0x00 && ent[0] != 0xE5) { c->have = 0; return 0; }
    c->slot[c->have].sec = sec;
    c->slot[c->have].off = off;
    if (++c->have == c->want) { c->done = 1; return 1; }
    return 0;
}

static int dir_free_run(uint32_t clus, int want, Slot *out)
{
    struct run_ctx c;
    int i;
    if (want > (int)(sizeof c.slot / sizeof c.slot[0])) return 0;
    c.want = want; c.have = 0; c.done = 0;
    if (dir_iterate(clus, run_cb, &c) < 0) return 0;
    if (!c.done) return 0;
    for (i = 0; i < want; i++) out[i] = c.slot[i];
    return 1;
}

/* finding an existing entry by name, long or short */
struct wfind_ctx {
    const char *want;
    char  lfn[256];
    int   lfn_len;
    int   found;
    Slot  at;
};

static int wfind_cb(uint32_t sec, uint32_t off, uint8_t *ent, void *ud)
{
    struct wfind_ctx *c = (struct wfind_ctx *)ud;
    char shortname[16];

    if (ent[0] == 0x00) return 1;                 /* end of directory */
    if (ent[0] == 0xE5) { c->lfn_len = 0; return 0; }
    if ((ent[11] & 0x0F) == 0x0F) {
        /* long-name slots arrive last-first, so each one is prepended */
        char part[16];
        int n = 0, i;
        lfn_chars(ent, part, &n);
        if (c->lfn_len + n < (int)sizeof c->lfn) {
            memmove(c->lfn + n, c->lfn, (size_t)c->lfn_len);
            for (i = 0; i < n; i++) c->lfn[i] = part[i];
            c->lfn_len += n;
        }
        return 0;
    }
    c->lfn[c->lfn_len < (int)sizeof c->lfn ? c->lfn_len : (int)sizeof c->lfn - 1] = 0;
    name_83(ent, shortname);
    if ((c->lfn_len && ieq(c->lfn, c->want)) || ieq(shortname, c->want)) {
        c->found = 1;
        c->at.sec = sec;
        c->at.off = off;
        return 1;
    }
    c->lfn_len = 0;
    return 0;
}

static int dir_find_name(uint32_t clus, const char *name, Slot *at)
{
    struct wfind_ctx c;
    memset(&c, 0, sizeof c);
    c.want = name;
    if (dir_iterate(clus, wfind_cb, &c) < 0) return 0;
    if (!c.found) return 0;
    *at = c.at;
    return 1;
}

/* Does this 8.3 name already exist in the directory? Used to pick a ~N
 * suffix that does not collide. */
struct dup_ctx { const char *n83; int hit; };

static int dup_cb(uint32_t sec, uint32_t off, uint8_t *ent, void *ud)
{
    struct dup_ctx *c = (struct dup_ctx *)ud;
    (void)sec; (void)off;
    if (ent[0] == 0x00) return 1;
    if (ent[0] == 0xE5 || (ent[11] & 0x0F) == 0x0F) return 0;
    if (memcmp(ent, c->n83, 11) == 0) { c->hit = 1; return 1; }
    return 0;
}

static int short_name_taken(uint32_t clus, const char n83[11])
{
    struct dup_ctx c;
    c.n83 = n83;
    c.hit = 0;
    dir_iterate(clus, dup_cb, &c);
    return c.hit;
}

/* Split "/dir/name.ext" into the parent directory and the final component. */
static int split_path(const char *path, char *parent, int pcap, const char **leaf)
{
    const char *slash = NULL, *p = path;
    int n;
    for (; *p; p++) if (*p == '/') slash = p;
    if (!slash) { if (pcap) parent[0] = 0; *leaf = path; return 1; }
    n = (int)(slash - path);
    if (n >= pcap) return 0;
    memcpy(parent, path, (size_t)n);
    parent[n] = 0;
    *leaf = slash + 1;
    return **leaf != 0;
}

int fat_write(const char *path, const void *vdata, int len)
{
    const uint8_t *data = (const uint8_t *)vdata;
    char parent[96];
    const char *leaf;
    char n83[11];
    uint32_t dclus, dsize, dsec = 0, doff = 0, first = 0;
    uint32_t need, written = 0, clus;
    int isdir, existed, nlfn = 0;
    Slot slots[21];

    if (len < 0) return 0;
    if (!fs.mounted && !fat_mount()) return 0;
    if (!split_path(path, parent, sizeof parent, &leaf)) return 0;
    if (!resolve(parent[0] ? parent : "/", &dclus, &dsize, &isdir)) return 0;
    if (!isdir) return 0;

    /* An existing file keeps its entry, whatever kind of name it has: only
     * the cluster and the size change, so a long name survives being
     * written to and the crash-safe ordering below still holds. */
    {
        Slot at;
        existed = dir_find_name(dclus, leaf, &at);
        if (existed) {
            dsec = at.sec;
            doff = at.off;
            if (!sd_read_block(dsec, secbuf)) return 0;
            memcpy(n83, &secbuf[doff], 11);
        } else {
            nlfn = lfn_slots_for(leaf);
            if (nlfn > 20) return 0;             /* 255 characters is the limit */
            if (nlfn == 0) {
                if (!name_to_83(leaf, n83)) return 0;
            } else {
                int tilde;
                for (tilde = 1; tilde < 100; tilde++) {
                    short_alias(leaf, tilde, n83);
                    if (!short_name_taken(dclus, n83)) break;
                }
                if (tilde >= 100) return 0;
            }
            if (!dir_free_run(dclus, nlfn + 1, slots)) return 0;
            dsec = slots[nlfn].sec;
            doff = slots[nlfn].off;
        }
    }

    /* Release the old contents first: if the write fails from here on, the
     * entry is left pointing at nothing rather than at half-stale clusters. */
    if (existed) {
        if (!sd_read_block(dsec, secbuf)) return 0;
        {
            uint32_t old = ((uint32_t)rd16(&secbuf[doff + 20]) << 16)
                         | rd16(&secbuf[doff + 26]);
            if (old >= 2 && !free_chain(old)) return 0;
        }
    }

    need = ((uint32_t)len + fs.sec_per_clus * 512u - 1) / (fs.sec_per_clus * 512u);
    if (len > 0) {
        first = alloc_chain(need);
        if (!first) return 0;
    }

    clus = first;
    while (written < (uint32_t)len && clus >= 2 && !is_eoc(clus)) {
        uint32_t s;
        for (s = 0; s < fs.sec_per_clus && written < (uint32_t)len; s++) {
            uint32_t take = (uint32_t)len - written;
            if (take > 512) take = 512;
            memset(secbuf, 0, 512);
            memcpy(secbuf, data + written, take);
            if (!sd_write_block(clus_sector(clus) + s, secbuf)) {
                free_chain(first);
                return 0;
            }
            written += take;
        }
        clus = fat_get(clus);
    }

    /* Long-name slots come first, in reverse order, each tied to the 8.3
     * entry by its checksum. They are written before the 8.3 entry itself,
     * because a run of orphaned 0x0F slots with no entry after them is
     * ignored by every reader, while an 8.3 entry with half its name in
     * front of it is not. */
    if (!existed && nlfn > 0) {
        uint8_t sum = lfn_checksum(n83);
        int nm = (int)strlen(leaf), k;
        for (k = 0; k < nlfn; k++) {
            static const int off13[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
            int seq = nlfn - k;               /* slot 1 holds the first 13 */
            int base = (seq - 1) * 13, i;
            uint8_t *e;
            if (!sd_read_block(slots[k].sec, secbuf)) { free_chain(first); return 0; }
            e = &secbuf[slots[k].off];
            memset(e, 0, 32);
            e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));   /* 0x40: last slot */
            e[11] = 0x0F;
            e[13] = sum;
            for (i = 0; i < 13; i++) {
                int idx = base + i;
                uint16_t ch;
                if (idx < nm) ch = (uint8_t)leaf[idx];
                else if (idx == nm) ch = 0;
                else ch = 0xFFFF;
                e[off13[i]] = (uint8_t)ch;
                e[off13[i] + 1] = (uint8_t)(ch >> 8);
            }
            if (!sd_write_block(slots[k].sec, secbuf)) { free_chain(first); return 0; }
        }
    }

    /* Only now does the directory entry start pointing at the new data. */
    if (!sd_read_block(dsec, secbuf)) { free_chain(first); return 0; }
    {
        uint8_t *ent = &secbuf[doff];
        memset(ent, 0, 32);
        memcpy(ent, n83, 11);
        ent[11] = 0x20;                              /* archive */
        ent[20] = (uint8_t)(first >> 16);
        ent[21] = (uint8_t)(first >> 24);
        ent[26] = (uint8_t)first;
        ent[27] = (uint8_t)(first >> 8);
        ent[28] = (uint8_t)len;
        ent[29] = (uint8_t)(len >> 8);
        ent[30] = (uint8_t)(len >> 16);
        ent[31] = (uint8_t)(len >> 24);
    }
    if (!sd_write_block(dsec, secbuf)) { free_chain(first); return 0; }
    return 1;
}

/* Delete a file: free its clusters, then mark the directory entry free.
 * Long-name slots ahead of the entry are left alone, which a scan treats as
 * orphans and ignores; fsck reports nothing because the 8.3 entry is what
 * holds the chain. */
int fat_remove(const char *path)
{
    char parent[96];
    const char *leaf;
    uint32_t dclus, dsize, dsec, doff;
    int isdir;
    Slot at;

    if (!fs.mounted && !fat_mount()) return 0;
    if (!split_path(path, parent, sizeof parent, &leaf)) return 0;
    if (!resolve(parent[0] ? parent : "/", &dclus, &dsize, &isdir)) return 0;
    if (!isdir) return 0;
    if (!dir_find_name(dclus, leaf, &at)) return 0;
    dsec = at.sec;
    doff = at.off;

    if (!sd_read_block(dsec, secbuf)) return 0;
    {
        uint32_t old = ((uint32_t)rd16(&secbuf[doff + 20]) << 16)
                     | rd16(&secbuf[doff + 26]);
        if (old >= 2 && !free_chain(old)) return 0;
    }
    if (!sd_read_block(dsec, secbuf)) return 0;
    secbuf[doff] = 0xE5;
    if (!sd_write_block(dsec, secbuf)) return 0;

    /* Mark the long-name slots in front of it free as well, so the name
     * does not linger in a listing and the slots can be reused. */
    {
        uint32_t sec = dsec, off = doff;
        for (;;) {
            if (off >= 32) {
                off -= 32;
            } else if (sec > fs.root_start) {
                sec--;
                off = 480;
            } else {
                break;
            }
            if (!sd_read_block(sec, secbuf)) break;
            if ((secbuf[off + 11] & 0x0F) != 0x0F || secbuf[off] == 0xE5) break;
            secbuf[off] = 0xE5;
            if (!sd_write_block(sec, secbuf)) break;
        }
    }
    return 1;
}

int fat_list(const char *path, char *out, size_t cap)
{
    uint32_t clus, size;
    int isdir;
    struct list_ctx c;
    if (!fs.mounted && !fat_mount()) return -1;
    if (!resolve(path && *path ? path : "/", &clus, &size, &isdir)) return -1;
    if (!isdir) return -1;
    c.out = out;
    c.cap = cap;
    c.used = 0;
    out[0] = 0;
    walk_dir(clus, list_cb, &c);
    return 0;
}
