/* The flash store: a table written once, then read with no RAM cost.
 *
 * store_read hands back a pointer straight into memory-mapped flash, so
 * the 1024 values below live outside the 520 KB of SRAM entirely. Writing
 * erases a 4 KB page, so do it when saving, not in a loop.
 *
 *   cc /store.c     run      then   lookup(500)
 */
#include <stdio.h>

#define N 1024

static int scratch[N];

/* Fill page 0 with squares. Only needed once; it survives a power cycle. */
int build(void)
{
    int i;
    for (i = 0; i < N; i++) scratch[i] = i * i;
    return store_write(0, scratch, N * 4);
}

/* Read one, without copying anything into RAM. */
double lookup(double n)
{
    int *table = (int *)store_read(0);
    int i = (int)n;
    if (!table || i < 0 || i >= N) return -1.0;
    return (double)table[i];
}

int main(void)
{
    int *table;
    if (!store_pages()) { printf("no flash store here\n"); return 1; }
    printf("%d pages of %d bytes\n", store_pages(), store_page_size());
    if (!build()) { printf("write failed\n"); return 1; }
    table = (int *)store_read(0);
    printf("table at %p, [500] = %d\n", (void *)table, table[500]);
    printf("call lookup(n) from the prompt\n");
    return 0;
}
