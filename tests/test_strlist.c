#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "strlist.h"

static int dtor_calls = 0;

static void dtor(void* ptr) {
    free(ptr);
    dtor_calls++;
}

static int test_init_free(void) {
    ZU_StrList list;
    zu_strlist_init(&list);
    if (list.items != NULL || list.len != 0 || list.cap != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    zu_strlist_free(&list);
    // Free on empty list should be safe
    zu_strlist_free(&list);
    return 0;
}

static int test_push_basic(void) {
    ZU_StrList list;
    zu_strlist_init(&list);

    if (zu_strlist_push(&list, "hello") != 0) {
        fprintf(stderr, "push hello failed\n");
        zu_strlist_free(&list);
        return 1;
    }
    if (list.len != 1 || list.cap < 1 || list.items[0] == NULL) {
        fprintf(stderr, "push state incorrect\n");
        zu_strlist_free(&list);
        return 1;
    }
    if (strcmp(list.items[0], "hello") != 0) {
        fprintf(stderr, "push value mismatch\n");
        zu_strlist_free(&list);
        return 1;
    }

    // Push NULL -> empty string
    if (zu_strlist_push(&list, NULL) != 0) {
        fprintf(stderr, "push NULL failed\n");
        zu_strlist_free(&list);
        return 1;
    }
    if (list.len != 2 || strcmp(list.items[1], "") != 0) {
        fprintf(stderr, "NULL push not empty string\n");
        zu_strlist_free(&list);
        return 1;
    }

    // Push another
    if (zu_strlist_push(&list, "world") != 0) {
        fprintf(stderr, "push world failed\n");
        zu_strlist_free(&list);
        return 1;
    }
    if (list.len != 3 || strcmp(list.items[2], "world") != 0) {
        fprintf(stderr, "third push mismatch\n");
        zu_strlist_free(&list);
        return 1;
    }

    zu_strlist_free(&list);
    return 0;
}

static int test_growth(void) {
    ZU_StrList list;
    zu_strlist_init(&list);

    // Push more than initial capacity (8)
    for (int i = 0; i < 20; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "item%d", i);
        if (zu_strlist_push(&list, buf) != 0) {
            fprintf(stderr, "push %d failed\n", i);
            zu_strlist_free(&list);
            return 1;
        }
    }
    if (list.len != 20) {
        fprintf(stderr, "growth length mismatch\n");
        zu_strlist_free(&list);
        return 1;
    }
    for (int i = 0; i < 20; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "item%d", i);
        if (strcmp(list.items[i], buf) != 0) {
            fprintf(stderr, "growth item %d mismatch\n", i);
            zu_strlist_free(&list);
            return 1;
        }
    }

    zu_strlist_free(&list);
    return 0;
}

static int test_free_with_dtor(void) {
    ZU_StrList list;
    zu_strlist_init(&list);

    // Push some strings
    zu_strlist_push(&list, "a");
    zu_strlist_push(&list, "b");
    zu_strlist_push(&list, "c");

    // Count calls to dtor
    dtor_calls = 0;
    zu_strlist_free_with_dtor(&list, dtor);
    if (dtor_calls != 3) {
        fprintf(stderr, "dtor called %d times, expected 3\n", dtor_calls);
        return 1;
    }
    // List should be empty
    if (list.items != NULL || list.len != 0 || list.cap != 0) {
        fprintf(stderr, "list not reset after free_with_dtor\n");
        return 1;
    }
    // Free again (should be safe)
    zu_strlist_free_with_dtor(&list, NULL);
    return 0;
}

int main(void) {
    if (test_init_free() != 0) {
        return 1;
    }
    if (test_push_basic() != 0) {
        return 1;
    }
    if (test_growth() != 0) {
        return 1;
    }
    if (test_free_with_dtor() != 0) {
        return 1;
    }
    printf("All strlist tests passed\n");
    return 0;
}