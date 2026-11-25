/* Persistent key-value store demo (survives reboot).
 *
 *   cc /kvdemo.c
 *   run
 *   Then reboot and run again — the counter and name survive.
 */
#include <stdio.h>

int main(void)
{
    char name[32];
    int count = 0;

    /* read previous values if they exist */
    if (kv_get("name", name, sizeof(name)) > 0)
        printf("welcome back, %s\n", name);
    else
        printf("first run\n");

    kv_get_int("count", &count);
    count++;
    kv_set_int("count", count);
    printf("this is run #%d\n", count);

    printf("enter your name: ");
    if (getline_(name, 30) > 0)
        kv_set("name", name);

    printf("saved. reboot and run again to see persistence.\n");
    return 0;
}