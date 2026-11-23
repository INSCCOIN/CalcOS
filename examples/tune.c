/* A short tune on the buzzer, to show tone() and the timer.
 *
 *   cc /tune.c      run
 */

/* frequency, milliseconds */
static int notes[] = {
    262, 200,  294, 200,  330, 200,  349, 200,
    392, 400,  392, 400,
    440, 200,  440, 200,  440, 200,  440, 200,
    392, 800,
    349, 200,  349, 200,  349, 200,  349, 200,
    330, 400,  330, 400,
    294, 200,  294, 200,  294, 200,  294, 200,
    262, 800,
    0, 0
};

int main(void)
{
    int i = 0;
    printf("playing, any key stops\n");
    while (notes[i] && !kbhit()) {
        /* tone() returns early if a key is waiting, so a key stops the
         * tune within a note rather than at the end of one */
        tone(notes[i], notes[i + 1]);
        delay(20);                   /* a gap, so repeated notes separate */
        i += 2;
    }
    notone();
    if (kbhit()) getkey();
    return 0;
}
