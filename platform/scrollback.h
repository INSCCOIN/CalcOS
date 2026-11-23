/* Console scrollback: the lines that have scrolled off the top. */
#ifndef SCROLLBACK_H
#define SCROLLBACK_H

#define SB_LINES 240      /* about nine screens, 9.6 KB */
#define SB_COLS  40

void        sb_reset(void);
void        sb_push(const char *line);   /* exactly SB_COLS characters */
int         sb_count(void);
const char *sb_at(int i);                /* 0 = oldest kept, NULL if none */

#endif
