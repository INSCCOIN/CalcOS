/* File I/O: a program that keeps data on the card between runs.
 *
 * Everything a program allocates is gone when it ends; a file is not.
 *
 *   cc /notes.c     run
 */
#include <stdio.h>

#define PATH "/notes.txt"

int main(void)
{
    void *f;
    char line[80];
    int n = 0;

    /* what is already there */
    f = fopen(PATH, "r");
    if (f) {
        printf("so far:\n");
        while (fgets(line, 80, f)) { printf("  %s", line); n++; }
        fclose(f);
    }

    printf("note (enter alone to stop): ");
    if (getline_(line, 78) <= 0) return 0;

    /* append: open "a", which reads the file back in first */
    f = fopen(PATH, "a");
    if (!f) { printf("cannot write %s\n", PATH); return 1; }
    fputs(line, f);
    fputs("\n", f);
    fclose(f);

    printf("%d note(s) before, one added\n", n);
    return 0;
}
