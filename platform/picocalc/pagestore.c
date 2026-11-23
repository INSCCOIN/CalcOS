/* A store in the Pico's own flash, outside SRAM.
 *
 * What this is, and what it is not. The RP2350 has no MMU, so there is no
 * way to page RAM in and out behind a program's back: a pointer is a
 * physical address and nothing can fault it in. What the part does have is
 * 4 MB of flash that is memory-mapped through the XIP cache, of which the
 * firmware uses about a quarter. Reading it is a load instruction, so a
 * table kept here costs *no* SRAM at all and is as quick as a cache hit.
 *
 * So this is not a page file. It is a store: 4 KB pages that a program
 * writes once and then reads directly, by pointer, for free. That is the
 * useful half of what paging would give on a machine like this, and the
 * half that can actually be delivered.
 *
 * Writing is the expensive side. Flash must be erased a whole 4 KB sector
 * at a time before it can be programmed, erasing takes tens of
 * milliseconds with interrupts off, and a sector wears out after on the
 * order of 100,000 erases. Writing a page per frame would destroy the part
 * in a few days, which is why store_write is documented as a save, not a
 * variable.
 */
#include "calc.h"
#include "picocalc.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

/* The store sits at the top of flash. The firmware is linked from the
 * bottom and is nowhere near this, which is checked at startup rather than
 * assumed: overwriting the binary would brick the board until it was
 * reflashed. */
#define STORE_BYTES  (512 * 1024)
#define STORE_OFFSET (PICO_FLASH_SIZE_BYTES - STORE_BYTES)
#define STORE_PAGES  (STORE_BYTES / FLASH_SECTOR_SIZE)

extern char __flash_binary_end;

static int store_ok = -1;          /* -1 not checked yet */

static int store_available(void)
{
    if (store_ok < 0) {
        uintptr_t end = (uintptr_t)&__flash_binary_end;
        uintptr_t first = XIP_BASE + STORE_OFFSET;
        /* leave a sector of daylight between the firmware and the store */
        store_ok = (end + FLASH_SECTOR_SIZE <= first) ? 1 : 0;
    }
    return store_ok;
}

int plat_store_pages(void)
{
    return store_available() ? STORE_PAGES : 0;
}

int plat_store_page_size(void) { return FLASH_SECTOR_SIZE; }

const void *plat_store_read(int page)
{
    if (!store_available() || page < 0 || page >= STORE_PAGES) return 0;
    /* A direct pointer into the XIP window: no copy, no RAM, and the
     * cache makes a second read of the same bytes as quick as SRAM. */
    return (const void *)(XIP_BASE + STORE_OFFSET
                          + (uint32_t)page * FLASH_SECTOR_SIZE);
}

int plat_store_write(int page, const void *data, int len)
{
    static uint8_t buf[FLASH_SECTOR_SIZE];
    uint32_t off;
    uint32_t ints;

    if (!store_available() || page < 0 || page >= STORE_PAGES) return 0;
    if (len < 0 || len > FLASH_SECTOR_SIZE) return 0;

    /* Flash programs in 256-byte pages and only ever clears bits, so a
     * short write still has to present a whole erased sector: the tail is
     * filled with 0xFF, which is what erased flash reads as. */
    memset(buf, 0xFF, sizeof buf);
    if (len) memcpy(buf, data, (size_t)len);

    off = (uint32_t)STORE_OFFSET + (uint32_t)page * FLASH_SECTOR_SIZE;

    /* Erase and program run from RAM with the XIP window turned off, so
     * nothing may try to execute or read from flash meanwhile. */
    ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return 1;
}

int plat_store_erase(int page)
{
    uint32_t off, ints;
    if (!store_available() || page < 0 || page >= STORE_PAGES) return 0;
    off = (uint32_t)STORE_OFFSET + (uint32_t)page * FLASH_SECTOR_SIZE;
    ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return 1;
}
