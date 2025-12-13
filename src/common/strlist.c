#include "strlist.h"

#include <stdlib.h>
#include <string.h>

/*
 * Growable list of owned C strings
 *
 * Ownership model
 * - zu_strlist_push duplicates the provided string into heap storage
 * - The list owns each element pointer in list->items
 * - zu_strlist_free releases every element and the backing pointer array
 *
 * Intended usage
 * - Collecting include/exclude patterns and operands from CLI parsing
 * - Storing small string vectors without requiring a heavy container dependency
 *
 * Error model
 * - Functions that allocate return 0 on success, -1 on allocation failure
 * - The list remains in a valid state on failure, but the failed push does not append an item
 */

/*
 * Initialize a list into the empty state
 * - Safe to call on a zeroed struct or an uninitialized struct
 * - After init, the list can be freed with zu_strlist_free even if never used
 */
void zu_strlist_init(ZU_StrList* list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

/*
 * Free all strings and internal storage owned by the list
 *
 * Requirements
 * - list may be NULL
 * - Each element is assumed to have been allocated with malloc-compatible allocator
 *
 * Postconditions
 * - The list is reset to the initialized empty state
 */
void zu_strlist_free(ZU_StrList* list) {
    if (!list)
        return;

    for (size_t i = 0; i < list->len; ++i)
        free(list->items[i]);

    free(list->items);

    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

/*
 * Free list storage using a custom destructor for elements
 *
 * Use cases
 * - When list->items contains pointers that are not plain strings
 * - When elements require extra cleanup beyond free
 *
 * Behavior
 * - If dtor is non-NULL, call dtor(element) for each element
 * - Always frees the pointer array afterwards
 *
 * Postconditions
 * - The list is reset to the initialized empty state
 */
void zu_strlist_free_with_dtor(ZU_StrList* list, void (*dtor)(void*)) {
    if (!list)
        return;

    if (dtor) {
        for (size_t i = 0; i < list->len; ++i)
            dtor(list->items[i]);
    }

    free(list->items);

    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

/*
 * Ensure there is room for at least one more element
 *
 * Growth policy
 * - Start with a small base capacity to avoid tiny realloc churn
 * - Double capacity to keep amortized push cost near O(1)
 *
 * Return value
 * - 0 on success
 * - -1 on allocation failure, list remains unchanged
 */
static int zu_strlist_grow(ZU_StrList* list) {
    size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
    char** new_items = realloc(list->items, new_cap * sizeof(char*));
    if (!new_items)
        return -1;

    list->items = new_items;
    list->cap = new_cap;
    return 0;
}

/*
 * Append a new string to the list, duplicating the input
 *
 * Input handling
 * - value may be NULL, which is treated as an empty string
 *
 * Return value
 * - 0 on success
 * - -1 on allocation failure
 */
int zu_strlist_push(ZU_StrList* list, const char* value) {
    if (list->len == list->cap) {
        if (zu_strlist_grow(list) != 0)
            return -1;
    }

    const char* src = value ? value : "";
    size_t n = strlen(src) + 1;

    char* copy = malloc(n);
    if (!copy)
        return -1;

    memcpy(copy, src, n);

    list->items[list->len] = copy;
    list->len += 1;
    return 0;
}
