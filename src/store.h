/* Named objects in the flash store: a directory in page 0, so things can be
 * found by name and the free space is known. See src/store.c. */
#ifndef STORE_H
#define STORE_H

#define STORE_NAMELEN  32
#define STORE_MAXENT   84
#define STORE_MAXPAGES 256
#define STORE_PAGE_MAX 4096

/* The largest file the shell will move in or out of the store in one go.
 * It matches the card writer's limit, so anything that fits on the card
 * fits here. */
#define STORE_FILE_MAX (16 * 1024)

/* what an object holds, so a listing can say and a loader can check */
enum { STORE_RAW = 0, STORE_PROG = 1, STORE_IMAGE = 2, STORE_TEXT = 3 };

/* results */
enum {
    STORE_OK = 0, STORE_ERR = -1, STORE_EFULL = -2, STORE_ENOENT = -3,
    STORE_EIO = -4, STORE_ENAME = -5
};

int         store_available(void);
int         store_format(void);
int         store_count(void);
int         store_entry(int idx, const char **name, int *len, int *kind,
                        int *page, int *pages);
const void *store_find(const char *name, int *len, int *kind);
int         store_put(const char *name, const void *data, int len, int kind);
int         store_delete(const char *name);
int         store_free_pages(void);
const char *store_kind_name(int kind);
void        store_forget(void);      /* re-read the directory next time */

#endif
