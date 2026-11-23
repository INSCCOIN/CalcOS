/* Exercise the read-only FAT reader against a disk image. */
#include "picocalc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static FILE *img;
int sd_init(void) { return img != NULL; }
int sd_read_block(uint32_t lba, uint8_t *buf)
{
    if (!img) return 0;
    if (fseek(img, (long)lba * 512L, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 512, img) == 512;
}

/* read-only harness: writing is covered by tests/fatwrite.c */
int sd_write_block(uint32_t lba, const uint8_t *buf)
{
    (void)lba; (void)buf;
    return 0;
}
void sd_reclock(void) { }

int main(int argc, char **argv)
{
    char list[2048];
    FatFile f;
    if (argc < 2) return 2;
    img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 2; }
    if (!fat_mount()) { printf("MOUNT FAILED\n"); return 1; }
    printf("mounted ok\n");
    if (fat_list("/", list, sizeof list) == 0) printf("ROOT:\n%s", list);
    else printf("list / failed\n");
    if (fat_list("/sub", list, sizeof list) == 0) printf("SUB:\n%s", list);
    {
        int i;
        for (i = 2; i < argc; i++) {
            if (!fat_open(&f, argv[i])) { printf("open %s FAILED\n", argv[i]); continue; }
            printf("--- %s (%u bytes)\n", argv[i], (unsigned)f.size);
            {
                static char buf[200000];
                int n = fat_read(&f, buf, (int)sizeof buf);
                unsigned long sum = 0;
                int k;
                for (k = 0; k < n; k++) sum += (unsigned char)buf[k];
                printf("read %d bytes, checksum %lu\n", n, sum);
                if (n < 300) fwrite(buf, 1, (size_t)n, stdout);
            }
        }
    }
    return 0;
}
