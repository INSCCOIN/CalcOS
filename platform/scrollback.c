/* Console scrollback.
 *
 * Every line that scrolls off the top of the screen is kept here, so long
 * output does not simply vanish: 240 lines is about nine screens. It is a
 * ring, which means the index arithmetic is the only thing that can go
 * wrong, so it lives in its own file with its own test rather than inside
 * the console where it would never be exercised directly.
 *
 * Lines are stored without a terminator, exactly SB_COLS characters wide,
 * because that is how the console holds them.
 */
#include "scrollback.h"
#include <string.h>

static char sb[SB_LINES][SB_COLS];
static int  head;        /* where the next line goes */
static int  count;       /* how many are filled      */

void sb_reset(void)
{
    head = 0;
    count = 0;
}

void sb_push(const char *line)
{
    memcpy(sb[head], line, SB_COLS);
    head = (head + 1) % SB_LINES;
    if (count < SB_LINES) count++;
}

int sb_count(void) { return count; }

/* Index 0 is the oldest line still kept, count-1 the most recent. */
const char *sb_at(int i)
{
    if (i < 0 || i >= count) return 0;
    /* head - count is where the oldest line sits; the offset can be
     * negative, so it is brought back into range before the modulo */
    return sb[(head - count + i + 2 * SB_LINES) % SB_LINES];
}
