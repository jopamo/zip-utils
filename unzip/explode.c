/* explode.c
 */

#define __EXPLODE_C /* identifies this source module */
#define UNZIP_INTERNAL
#include "unzip.h"  /* must supply slide[] (uch) array, mask_bits[], NEXTBYTE, flush, huft_build, huft_free */
#include <string.h> /* memcpy, memset */

#ifndef WSIZE
#define WSIZE 0x8000 /* window size must be a power of two and at least 8K */
#endif

/* always use the full 32 KiB window on modern Unix */
#define wszimpl WSIZE

/* compiler hints for GCC/Clang */
#define HOT __attribute__((hot))
#define COLD __attribute__((cold))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* forward decls matching UnZip style */
static int get_tree OF((__GPRO__ unsigned* l, unsigned n));
static int HOT explode_lit OF((__GPRO__ struct huft * tb, struct huft* tl, struct huft* td, unsigned bb, unsigned bl, unsigned bd, unsigned bdl));
static int HOT explode_nolit OF((__GPRO__ struct huft * tl, struct huft* td, unsigned bl, unsigned bd, unsigned bdl));
int explode OF((__GPRO));

/* the implode algorithm historically used 4K or 8K windows
 * this implementation uses the 32K inflate window as a circular buffer
 * index wraps with & 0x7fff so it can share the output buffer
 */

/* this must be provided externally, e.g. uch slide[WSIZE] or a malloc'ed buffer
 * in UnZip, slide[] is a 32K area also used by inflate
 */

#define INVALID_CODE 99
#define IS_INVALID_CODE(c) ((c) == INVALID_CODE)

/* tables for length and distance */
static ZCONST ush cplen2[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
                              34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65};
static ZCONST ush cplen3[] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                              35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66};
static ZCONST uch extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8};
static ZCONST ush cpdist4[] = {1,    65,   129,  193,  257,  321,  385,  449,  513,  577,  641,  705,  769,  833,  897,  961,  1025, 1089, 1153, 1217, 1281, 1345,
                               1409, 1473, 1537, 1601, 1665, 1729, 1793, 1857, 1921, 1985, 2049, 2113, 2177, 2241, 2305, 2369, 2433, 2497, 2561, 2625, 2689, 2753,
                               2817, 2881, 2945, 3009, 3073, 3137, 3201, 3265, 3329, 3393, 3457, 3521, 3585, 3649, 3713, 3777, 3841, 3905, 3969, 4033};
static ZCONST ush cpdist8[] = {1,    129,  257,  385,  513,  641,  769,  897,  1025, 1153, 1281, 1409, 1537, 1665, 1793, 1921, 2049, 2177, 2305, 2433, 2561, 2689,
                               2817, 2945, 3073, 3201, 3329, 3457, 3585, 3713, 3841, 3969, 4097, 4225, 4353, 4481, 4609, 4737, 4865, 4993, 5121, 5249, 5377, 5505,
                               5633, 5761, 5889, 6017, 6145, 6273, 6401, 6529, 6657, 6785, 6913, 7041, 7169, 7297, 7425, 7553, 7681, 7809, 7937, 8065};

/* bit buffer helpers */
#define NEEDBITS(n)                    \
    {                                  \
        while (k < (n)) {              \
            b |= ((ulg)NEXTBYTE) << k; \
            k += 8;                    \
        }                              \
    }
#define DUMPBITS(n) \
    {               \
        b >>= (n);  \
        k -= (n);   \
    }

#define DECODEHUFT(htab, bits, mask)                      \
    {                                                     \
        NEEDBITS((unsigned)(bits));                       \
        t = (htab) + ((~(unsigned)b) & (mask));           \
        for (;;) {                                        \
            DUMPBITS(t->b);                               \
            if (LIKELY((e = t->e) <= 32))                 \
                break;                                    \
            if (UNLIKELY(IS_INVALID_CODE(e)))             \
                return 1;                                 \
            e &= 31;                                      \
            NEEDBITS(e);                                  \
            t = t->v.t + ((~(unsigned)b) & mask_bits[e]); \
        }                                                 \
    }

/* read bit lengths for a code representation from the compressed stream
 * returns 0 on success, 4 on malformed input
 */
static int get_tree(__G__ l, n)
__GDEF
unsigned* l; /* bit lengths */
unsigned n;  /* number expected */
{
    unsigned i; /* bytes remaining in list */
    unsigned k; /* lengths entered */
    unsigned j; /* number of codes */
    unsigned b; /* bit length for those codes */

    i = NEXTBYTE + 1; /* length/count pairs to read */
    k = 0;            /* next code */
    do {
        b = ((j = NEXTBYTE) & 0xf) + 1; /* bits in code 1..16 */
        j = ((j & 0xf0) >> 4) + 1;      /* codes with those bits 1..16 */
        if (k + j > n)
            return 4; /* overflow of l[] */
        do {
            l[k++] = b;
        } while (--j);
    } while (--i);
    return k != n ? 4 : 0; /* must read exactly n entries */
}

/* coded literals variant */
static int explode_lit(__G__ tb, tl, td, bb, bl, bd, bdl)
__GDEF
struct huft *tb, *tl, *td; /* literal, length, and distance tables */
unsigned bb, bl, bd;       /* bits decoded by those tables */
unsigned bdl;              /* number of distance low bits */
{
    zusz_t s;            /* bytes left to output */
    register unsigned e; /* table entry flag or number of extra bits */
    unsigned n, d;       /* copy length and distance */
    unsigned w;          /* window position */
    struct huft* t;      /* pointer to table entry */
    unsigned mb, ml, md; /* masks for bb, bl, bd */
    unsigned mdl;        /* mask for distance lower bits */
    register ulg b;      /* bit buffer */
    register unsigned k; /* valid bits in b */
    unsigned u;          /* true if unflushed */
    int retval = 0;      /* 0 ok, else error */

    b = k = w = 0;
    u = 1; /* buffer unflushed */
    mb = mask_bits[bb];
    ml = mask_bits[bl];
    md = mask_bits[bd];
    mdl = mask_bits[bdl];
    s = G.lrec.ucsize;

    while (s > 0) {
        NEEDBITS(1);
        if (LIKELY(b & 1)) {
            /* literal path */
            DUMPBITS(1);
            s--;
            DECODEHUFT(tb, bb, mb); /* coded literal */
            redirSlide[w++] = (uch)t->v.n;
            if (w == wszimpl) {
                if ((retval = flush(__G__ redirSlide, (ulg)w, 0)) != 0)
                    return retval;
                w = u = 0;
            }
        }
        else {
            /* length/distance path */
            DUMPBITS(1);
            NEEDBITS(bdl);
            d = (unsigned)b & mdl; /* low distance bits */
            DUMPBITS(bdl);
            DECODEHUFT(td, bd, md); /* high distance bits */
            d = w - d - t->v.n;     /* full distance */

            DECODEHUFT(tl, bl, ml); /* length code */
            n = t->v.n;
            if (UNLIKELY(e)) { /* extra length byte present */
                NEEDBITS(8);
                n += (unsigned)b & 0xff;
                DUMPBITS(8);
            }

            /* copy sequence */
            s = (s > (zusz_t)n ? s - (zusz_t)n : 0);
            do {
                e = wszimpl - ((d &= wszimpl - 1) > w ? d : w);
                if (e > n)
                    e = n;
                n -= e;

                if (u && w <= d) {
                    memset(redirSlide + w, 0, e);
                    w += e;
                    d += e;
                }
                else
#ifndef NOMEMCPY
                    if (w - d >= e) { /* non-overlap fast path */
                    memcpy(redirSlide + w, redirSlide + d, e);
                    w += e;
                    d += e;
                }
                else /* overlap slow path */
#endif
                {
                    do {
                        redirSlide[w++] = redirSlide[d++];
                    } while (--e);
                }

                if (w == wszimpl) {
                    if ((retval = flush(__G__ redirSlide, (ulg)w, 0)) != 0)
                        return retval;
                    w = u = 0;
                }
            } while (n);
        }
    }

    /* final flush */
    if ((retval = flush(__G__ redirSlide, (ulg)w, 0)) != 0)
        return retval;

    /* account for possible bit over-read as in original code */
    if (G.csize + G.incnt + (k >> 3)) {
        G.used_csize = G.lrec.csize - G.csize - G.incnt - (k >> 3);
        return 5;
    }
    return 0;
}

/* uncoded literals variant */
static int explode_nolit(__G__ tl, td, bl, bd, bdl)
__GDEF
struct huft *tl, *td; /* length and distance decoder tables */
unsigned bl, bd;      /* bits decoded by tl and td */
unsigned bdl;         /* number of distance low bits */
{
    zusz_t s;            /* bytes left to output */
    register unsigned e; /* table entry flag or number of extra bits */
    unsigned n, d;       /* copy length and distance */
    unsigned w;          /* window position */
    struct huft* t;      /* pointer to table entry */
    unsigned ml, md;     /* masks for bl and bd */
    unsigned mdl;        /* mask for distance lower bits */
    register ulg b;      /* bit buffer */
    register unsigned k; /* valid bits in b */
    unsigned u;          /* true if unflushed */
    int retval = 0;      /* 0 ok, else error */

    b = k = w = 0;
    u = 1;
    ml = mask_bits[bl];
    md = mask_bits[bd];
    mdl = mask_bits[bdl];
    s = G.lrec.ucsize;

    while (s > 0) {
        NEEDBITS(1);
        if (LIKELY(b & 1)) {
            /* literal byte read */
            DUMPBITS(1);
            s--;
            NEEDBITS(8);
            redirSlide[w++] = (uch)b;
            if (w == wszimpl) {
                if ((retval = flush(__G__ redirSlide, (ulg)w, 0)) != 0)
                    return retval;
                w = u = 0;
            }
            DUMPBITS(8);
        }
        else {
            /* length/distance path */
            DUMPBITS(1);
            NEEDBITS(bdl);
            d = (unsigned)b & mdl;
            DUMPBITS(bdl);
            DECODEHUFT(td, bd, md);
            d = w - d - t->v.n;

            DECODEHUFT(tl, bl, ml);
            n = t->v.n;
            if (UNLIKELY(e)) {
                NEEDBITS(8);
                n += (unsigned)b & 0xff;
                DUMPBITS(8);
            }

            /* copy sequence */
            s = (s > (zusz_t)n ? s - (zusz_t)n : 0);
            do {
                e = wszimpl - ((d &= wszimpl - 1) > w ? d : w);
                if (e > n)
                    e = n;
                n -= e;

                if (u && w <= d) {
                    memset(redirSlide + w, 0, e);
                    w += e;
                    d += e;
                }
                else
#ifndef NOMEMCPY
                    if (w - d >= e) { /* non-overlap fast path */
                    memcpy(redirSlide + w, redirSlide + d, e);
                    w += e;
                    d += e;
                }
                else /* overlap slow path */
#endif
                {
                    do {
                        redirSlide[w++] = redirSlide[d++];
                    } while (--e);
                }

                if (w == wszimpl) {
                    if ((retval = flush(__G__ redirSlide, (ulg)w, 0)) != 0)
                        return retval;
                    w = u = 0;
                }
            } while (n);
        }
    }

    /* final flush */
    if ((retval = flush(__G__ redirSlide, (ulg)w, 0)) != 0)
        return retval;

    if (G.csize + G.incnt + (k >> 3)) {
        G.used_csize = G.lrec.csize - G.csize - G.incnt - (k >> 3);
        return 5;
    }
    return 0;
}

/* top-level explode entry point
 * selects literal coding variant based on general purpose bit flags
 * builds Huffman tables and dispatches to the appropriate worker
 */
int explode(__G) __GDEF {
    unsigned r;      /* return codes */
    struct huft* tb; /* literal code table */
    struct huft* tl; /* length code table */
    struct huft* td; /* distance code table */
    unsigned bb;     /* bits for tb */
    unsigned bl;     /* bits for tl */
    unsigned bd;     /* bits for td */
    unsigned bdl;    /* number of uncoded lower distance bits */
    unsigned l[256]; /* bit lengths for codes */

    /* base table sizes tuned for speed vs memory */
    bl = 7;
    bd = (G.csize + G.incnt) > 200000L ? 8 : 7;

#ifdef DEBUG
    G.hufts = 0; /* initialize huft allocation counter */
#endif

    if (G.lrec.general_purpose_bit_flag & 4) {
        /* with literal tree, minimum match length is 3 */
        bb = 9; /* base size for literal table */
        if ((r = get_tree(__G__ l, 256)) != 0)
            return (int)r;
        if ((r = huft_build(__G__ l, 256, 256, NULL, NULL, &tb, &bb)) != 0) {
            if (r == 1)
                huft_free(tb);
            return (int)r;
        }
        if ((r = get_tree(__G__ l, 64)) != 0) {
            huft_free(tb);
            return (int)r;
        }
        if ((r = huft_build(__G__ l, 64, 0, cplen3, extra, &tl, &bl)) != 0) {
            if (r == 1)
                huft_free(tl);
            huft_free(tb);
            return (int)r;
        }
    }
    else {
        /* no literal tree, minimum match length is 2 */
        tb = (struct huft*)NULL;
        if ((r = get_tree(__G__ l, 64)) != 0)
            return (int)r;
        if ((r = huft_build(__G__ l, 64, 0, cplen2, extra, &tl, &bl)) != 0) {
            if (r == 1)
                huft_free(tl);
            return (int)r;
        }
    }

    if ((r = get_tree(__G__ l, 64)) != 0) {
        huft_free(tl);
        if (tb != (struct huft*)NULL)
            huft_free(tb);
        return (int)r;
    }

    if (G.lrec.general_purpose_bit_flag & 2) {
        /* 8K historic window, but we still use 32K buffer */
        bdl = 7;
        r = huft_build(__G__ l, 64, 0, cpdist8, extra, &td, &bd);
    }
    else {
        /* 4K historic window */
        bdl = 6;
        r = huft_build(__G__ l, 64, 0, cpdist4, extra, &td, &bd);
    }
    if (r != 0) {
        if (r == 1)
            huft_free(td);
        huft_free(tl);
        if (tb != (struct huft*)NULL)
            huft_free(tb);
        return (int)r;
    }

    if (tb != NULL) {
        r = explode_lit(__G__ tb, tl, td, bb, bl, bd, bdl);
        huft_free(tb);
    }
    else {
        r = explode_nolit(__G__ tl, td, bl, bd, bdl);
    }

    huft_free(td);
    huft_free(tl);
    Trace((stderr, "<%u > ", G.hufts));
    return (int)r;
}

/* keep macro namespace clean when compiling alongside inflate.c */
#undef DECODEHUFT
#undef NEEDBITS
#undef DUMPBITS
#undef wszimpl
