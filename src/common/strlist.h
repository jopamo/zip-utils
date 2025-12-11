// Simple dynamic string list used by the modern rewrite.
#ifndef ZU_STRLIST_H
#define ZU_STRLIST_H

#include <stddef.h>

typedef struct {
    char** items;
    size_t len;
    size_t cap;
} ZU_StrList;

void zu_strlist_init(ZU_StrList* list);
void zu_strlist_free(ZU_StrList* list);
/* Frees the list using a custom destructor for each item. */
void zu_strlist_free_with_dtor(ZU_StrList* list, void (*dtor)(void*));
int zu_strlist_push(ZU_StrList* list, const char* value);

#endif
