/*
  Unix/Linux-only matcher for Info-ZIP UnZip.

  Supported pattern syntax:
    *    — any sequence (including empty)
    ?    — any single byte
    [..] — set/range; [!..] or [^..] negates
    \x   — escape to match literal x

  Case-insensitive mode uses a safe ToLower on unsigned char values.
*/

#define __MATCH_C

#define UNZIP_INTERNAL
#include "unzip.h"

#include <string.h>
#include <ctype.h>

/* --- Safe case-folding -------------------------------------------------- */

#ifndef ToLower
#define ToLower(x) ((int)tolower((int)(unsigned char)(x)))
#endif

#ifndef Case
#define Case(x) (ic ? ToLower((unsigned char)(x)) : (int)(unsigned char)(x))
#endif

/* --- Tokens (Unix style) ------------------------------------------------ */

#define WILDCHAR '?'
#define BEG_RANGE '['
#define END_RANGE ']'

/* Core recursive matcher (Unix semantics) */
static int recmatch(ZCONST uch* p, ZCONST uch* s, int ic);
static char* isshexp(ZCONST char* p);
static int namecmp(ZCONST char* s1, ZCONST char* s2);

/* Public shell: booleanize recmatch() */
int match(ZCONST char* string, ZCONST char* pattern, int ignore_case) {
    return recmatch((ZCONST uch*)pattern, (ZCONST uch*)string, ignore_case) == 1;
}

static int recmatch(ZCONST uch* p, ZCONST uch* s, int ic) {
    unsigned int c;

    /* fetch next pattern byte */
    c = *p;
    INCSTR(p);

    /* end of pattern: match iff end of string */
    if (c == 0)
        return *s == 0;

    /* single-char wildcard */
    if (c == (unsigned)WILDCHAR)
        return *s ? recmatch(p, s + CLEN(s), ic) : 0;

    /* multi-char wildcard '*' */
    if (c == '*') {
        /* trailing '*' matches the rest */
        if (*p == 0)
            return 1;

        /* if the rest of the pattern is literal, fast-tail compare */
        if (isshexp((ZCONST char*)p) == NULL) {
            ZCONST uch* srest = s + (strlen((ZCONST char*)s) - strlen((ZCONST char*)p));
            if (srest - s < 0)
                return 0;
#ifdef _MBCS
            {
                /* walk forward to srest by characters (avoid mid-MBCS split) */
                ZCONST uch* q = s;
                while (q < srest)
                    INCSTR(q);
                if (q != srest)
                    return 0;
                return ((ic ? namecmp((ZCONST char*)p, (ZCONST char*)q) : strcmp((ZCONST char*)p, (ZCONST char*)q)) == 0);
            }
#else
            return ((ic ? namecmp((ZCONST char*)p, (ZCONST char*)srest) : strcmp((ZCONST char*)p, (ZCONST char*)srest)) == 0);
#endif
        }

        /* general case: try to consume any number of bytes */
        for (; *s; INCSTR(s)) {
            int r = recmatch(p, s, ic);
            if (r != 0)
                return r;
        }
        return 2; /* give up -> overall "no match" */
    }

    /* character class/range */
    if (c == (unsigned)BEG_RANGE) {
        int invert, esc = 0;
        ZCONST uch* q;
        unsigned int want = (unsigned int)Case(*s);
        unsigned int lo = 0;
        int matched = 0;

        if (*s == 0)
            return 0;

        invert = (*p == '!' || *p == '^') ? (INCSTR(p), 1) : 0;

        /* find closing ']' */
        for (q = p; *q; INCSTR(q)) {
            if (esc) {
                esc = 0;
                continue;
            }
            if (*q == '\\') {
                esc = 1;
                continue;
            }
            if (*q == (unsigned)END_RANGE)
                break;
        }
        if (*q != (unsigned)END_RANGE)
            return 0; /* bad class -> no match */

        /* iterate class content */
        esc = (*p == '-') ? 1 : 0; /* leading '-' literal */
        for (; p < q; INCSTR(p)) {
            if (!esc && *p == '\\') {
                esc = 1;
                continue;
            }

            if (!esc && *p == '-') {
                /* range: lo already set to previous, hi is next */
                ZCONST uch* hi_p = p + 1;
                if (hi_p >= q) { /* trailing '-' literal */
                    if (want == (unsigned)Case('-'))
                        matched = 1;
                    break;
                }
                /* compute inclusive range [lo..*hi_p] */
                {
                    unsigned int hi = (unsigned int)Case(*hi_p);
                    unsigned int a = lo, b = hi;
                    if (a > b) {
                        unsigned int t = a;
                        a = b;
                        b = t;
                    }
                    if (want >= a && want <= b)
                        matched = 1;
                }
                /* skip the hi char (loop INCSTR will add one more) */
                INCSTR(p); /* consume hi */
                lo = 0;
                esc = 0;
                continue;
            }

            /* literal member: set as possible lo for a following range,
               and also check singleton match */
            lo = (unsigned int)Case(*p);
            if (want == lo)
                matched = 1;
            esc = 0;
        }

        /* apply inversion and continue after ']' */
        return (invert ? !matched : matched) ? recmatch(q + 1, s + CLEN(s), ic) : 0;
    }

    /* escape: match literal next char */
    if (c == '\\') {
        c = *p++;
        if (c == 0)
            return 0; /* dangling '\' -> no match */
    }

    /* literal compare */
    return (Case((uch)c) == Case(*s)) ? recmatch(p, s + CLEN(s), ic) : 0;
}

/* return pointer to first special shell char in p, else NULL */
static char* isshexp(ZCONST char* p) {
    for (; *p; INCSTR(p)) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            continue;
        }
        if (*p == WILDCHAR || *p == '*' || *p == BEG_RANGE)
            return (char*)p;
    }
    return NULL;
}

/* case-insensitive strcmp using safe ToLower on unsigned char */
static int namecmp(ZCONST char* s1, ZCONST char* s2) {
    for (;;) {
        int d = (int)ToLower((uch)*s1) - (int)ToLower((uch)*s2);
        if (d || *s1 == 0 || *s2 == 0)
            return d;
        s1++;
        s2++;
    }
}

/* simple “does it contain any wildcards?” helper for Unix */
int iswild(ZCONST char* p) {
    for (; *p; INCSTR(p)) {
        if (*p == '\\' && *(p + 1)) {
            ++p;
            continue;
        }
        if (*p == '?' || *p == '*' || *p == '[')
            return TRUE;
    }
    return FALSE;
}
