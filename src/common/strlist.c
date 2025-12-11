#include "strlist.h"

#include <stdlib.h>
#include <string.h>

void zu_strlist_init(ZU_StrList *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

void zu_strlist_free(ZU_StrList *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->len; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int zu_strlist_grow(ZU_StrList *list) {
    size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
    char **new_items = realloc(list->items, new_cap * sizeof(char *));
    if (!new_items) {
        return -1;
    }
    list->items = new_items;
    list->cap = new_cap;
    return 0;
}

int zu_strlist_push(ZU_StrList *list, const char *value) {
    if (list->len == list->cap) {
        if (zu_strlist_grow(list) != 0) {
            return -1;
        }
    }
    const char *src = value ? value : "";
    size_t len = strlen(src) + 1;
    list->items[list->len] = malloc(len);
    if (!list->items[list->len]) {
        return -1;
    }
    memcpy(list->items[list->len], src, len);
    list->len += 1;
    return 0;
}
