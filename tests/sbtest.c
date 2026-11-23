/* The scrollback ring, exercised past its wraparound point.
 *
 * The console only ever pushes and reads back, so a bug in the index
 * arithmetic would show up as output quietly going missing after a few
 * hundred lines, which is exactly the kind of thing nobody notices until
 * they need the line that vanished. */
#include "scrollback.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void push_numbered(int n)
{
    char row[SB_COLS];
    memset(row, ' ', SB_COLS);
    snprintf(row, sizeof row, "%d", n);
    row[strlen(row)] = ' ';          /* the console stores no terminator */
    sb_push(row);
}

static int value_at(int i)
{
    char tmp[SB_COLS + 1];
    const char *p = sb_at(i);
    if (!p) return -1;
    memcpy(tmp, p, SB_COLS);
    tmp[SB_COLS] = 0;
    return atoi(tmp);
}

int main(void)
{
    int i;

    sb_reset();
    printf("-- scrollback\n");
    check(sb_count() == 0, "a fresh ring is empty");
    check(sb_at(0) == 0, "reading an empty ring gives nothing");

    for (i = 0; i < 10; i++) push_numbered(i);
    check(sb_count() == 10, "ten lines are counted");
    check(value_at(0) == 0, "index 0 is the oldest line");
    check(value_at(9) == 9, "the last index is the newest line");
    check(sb_at(10) == 0, "reading past the end gives nothing");
    check(sb_at(-1) == 0, "a negative index gives nothing");

    /* fill it exactly */
    sb_reset();
    for (i = 0; i < SB_LINES; i++) push_numbered(i);
    check(sb_count() == SB_LINES, "the ring fills to its capacity");
    check(value_at(0) == 0, "a full ring still starts at the oldest line");
    check(value_at(SB_LINES - 1) == SB_LINES - 1, "and ends at the newest");

    /* one past full: the oldest must be dropped, not overwritten at random */
    push_numbered(SB_LINES);
    check(sb_count() == SB_LINES, "the count stops at the capacity");
    check(value_at(0) == 1, "the oldest line is the one that is dropped");
    check(value_at(SB_LINES - 1) == SB_LINES, "the newest line is the last push");

    /* several times round, which is where an index bug would surface */
    for (i = SB_LINES + 1; i < SB_LINES * 3 + 7; i++) push_numbered(i);
    check(value_at(SB_LINES - 1) == SB_LINES * 3 + 6,
          "after three laps the newest line is still the last push");
    check(value_at(0) == SB_LINES * 3 + 7 - SB_LINES,
          "and the oldest is exactly a ring behind it");
    {
        int ok = 1;
        for (i = 0; i < SB_LINES; i++)
            if (value_at(i) != value_at(0) + i) ok = 0;
        check(ok, "the kept lines are in order with no gaps");
    }

    if (!fails) printf("   ring holds %d lines in order across wraparound\n",
                       SB_LINES);
    return fails ? 1 : 0;
}
