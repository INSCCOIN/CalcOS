#ifndef FAKE_HW_SYNC_H
#define FAKE_HW_SYNC_H
#include <stdint.h>
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t s);
void __dsb(void);
void __sev(void);
void __wfe(void);
void __dmb(void);
void __isb(void);
#endif
