/* Exercise the FAT writer against a disk image.
 *
 *   fatwrite IMG write NAME SIZE SEED   create/overwrite a generated file
 *   fatwrite IMG verify NAME SIZE SEED  read it back and compare
 *   fatwrite IMG list                   list the root directory
 */
#include "picocalc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *img;

int sd_init(void) { return img != NULL; }

int sd_read_block(uint32_t lba, uint8_t *buf)
{
    if (!img || fseek(img, (long)lba * 512L, SEEK_SET)) return 0;
    return fread(buf, 1, 512, img) == 512;
}

int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    if (!img || fseek(img, (long)lba * 512L, SEEK_SET)) return 0;
    if (fwrite(buf, 1, 512, img) != 512) return 0;
    fflush(img);
    return 1;
}

void sd_reclock(void) { }

static unsigned char gen(int i, int seed)
{
    return (unsigned char)((i * 37 + seed * 101 + 11) & 0xFF);
}

int main(int argc, char **argv)
{
    static unsigned char buf[300000];
    char list[4096];
    const char *op, *name;
    int size, seed, i;

    if (argc < 3) return 2;
    img = fopen(argv[1], "r+b");
    if (!img) { perror(argv[1]); return 2; }
    if (!fat_mount()) { printf("MOUNT FAILED\n"); return 1; }
    op = argv[2];

    if (!strcmp(op, "list")) {
        if (fat_list("/", list, sizeof list) != 0) { printf("LIST FAILED\n"); return 1; }
        fputs(list, stdout);
        return 0;
    }

    if (argc < 6) return 2;
    name = argv[3];
    size = atoi(argv[4]);
    seed = atoi(argv[5]);
    if (size > (int)sizeof buf) return 2;

    if (!strcmp(op, "write")) {
        for (i = 0; i < size; i++) buf[i] = gen(i, seed);
        if (!fat_write(name, buf, size)) { printf("WRITE FAILED %s\n", name); return 1; }
        printf("wrote %s %d bytes\n", name, size);
        return 0;
    }

    if (!strcmp(op, "verify")) {
        FatFile f;
        int n;
        if (!fat_open(&f, name)) { printf("OPEN FAILED %s\n", name); return 1; }
        if ((int)f.size != size) {
            printf("SIZE MISMATCH %s: %u vs %d\n", name, (unsigned)f.size, size);
            return 1;
        }
        n = fat_read(&f, buf, size);
        if (n != size) { printf("SHORT READ %s: %d\n", name, n); return 1; }
        for (i = 0; i < size; i++)
            if (buf[i] != gen(i, seed)) {
                printf("MISMATCH %s at %d\n", name, i);
                return 1;
            }
        printf("verified %s %d bytes\n", name, size);
        return 0;
    }
    return 2;
}
