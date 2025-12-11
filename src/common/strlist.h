// Simple dynamic string list used by the modern rewrite.
#ifndef ZU_STRLIST_H
#define ZU_STRLIST_H

#include <stddef.h>

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} ZU_StrList;

void zu_strlist_init(ZU_StrList *list);
void zu_strlist_free(ZU_StrList *list);
int zu_strlist_push(ZU_StrList *list, const char *value);

#endif
