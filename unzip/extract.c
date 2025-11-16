/*
  Copyright (c) 1990-2014 Info-ZIP.  All rights reserved.

  See the accompanying file LICENSE, version 2009-Jan-02 or later
  (the contents of which are also included in unzip.h) for terms of use.
  If, for some reason, all these files are missing, the Info-ZIP license
  also may be found at:  ftp://ftp.info-zip.org/pub/infozip/license.html
*/
/*---------------------------------------------------------------------------

  extract.c

  This file contains the high-level routines ("driver routines") for extrac-
  ting and testing zipfile members.  It calls the low-level routines in files
  explode.c, inflate.c, unreduce.c and unshrink.c.

  Contains:  extract_or_test_files()
             store_info()
             find_compr_idx()
             extract_or_test_entrylist()
             extract_or_test_member()
             TestExtraField()
             test_compr_eb()
             memextract()
             memflush()
             extract_izvms_block()    (VMS or VMS_TEXT_CONV)
             set_deferred_symlink()   (SYMLINKS only)
             fnfilter()
             dircomp()                (SET_DIR_ATTRIB only)
             UZbunzip2()              (USE_BZIP2 only)

  ---------------------------------------------------------------------------*/

#define __EXTRACT_C /* identifies this source module */
#define UNZIP_INTERNAL
#include "unzip.h"
#include "common/crc32.h"
#include "crypt.h"

#define GRRDUMP(buf, len)                                  \
    {                                                      \
        int i, j;                                          \
                                                           \
        for (j = 0; j < (len) / 16; ++j) {                 \
            printf("        ");                            \
            for (i = 0; i < 16; ++i)                       \
                printf("%02x ", (uch)(buf)[i + (j << 4)]); \
            printf("\n        ");                          \
            for (i = 0; i < 16; ++i) {                     \
                char c = (char)(buf)[i + (j << 4)];        \
                                                           \
                if (c == '\n')                             \
                    printf("\\n ");                        \
                else if (c == '\r')                        \
                    printf("\\r ");                        \
                else                                       \
                    printf(" %c ", c);                     \
            }                                              \
            printf("\n");                                  \
        }                                                  \
        if ((len) % 16) {                                  \
            printf("        ");                            \
            for (i = j << 4; i < (len); ++i)               \
                printf("%02x ", (uch)(buf)[i]);            \
            printf("\n        ");                          \
            for (i = j << 4; i < (len); ++i) {             \
                char c = (char)(buf)[i];                   \
                                                           \
                if (c == '\n')                             \
                    printf("\\n ");                        \
                else if (c == '\r')                        \
                    printf("\\r ");                        \
                else                                       \
                    printf(" %c ", c);                     \
            }                                              \
            printf("\n");                                  \
        }                                                  \
    }

static int store_info OF((__GPRO));
#ifdef SET_DIR_ATTRIB
static int extract_or_test_entrylist OF((__GPRO__ unsigned numchunk, ulg* pfilnum, ulg* pnum_bad_pwd, zoff_t* pold_extra_bytes, unsigned* pnum_dirs, direntry** pdirlist, int error_in_archive));
#else
static int extract_or_test_entrylist OF((__GPRO__ unsigned numchunk, ulg* pfilnum, ulg* pnum_bad_pwd, zoff_t* pold_extra_bytes, int error_in_archive));
#endif
static int extract_or_test_member OF((__GPRO));
#ifndef SFX
static int TestExtraField OF((__GPRO__ uch * ef, unsigned ef_len));
static int test_compr_eb OF((__GPRO__ uch * eb, unsigned eb_size, unsigned compr_offset, int (*test_uc_ebdata)(__GPRO__ uch* eb, unsigned eb_size, uch* eb_ucptr, ulg eb_ucsize)));
#endif
#ifdef SYMLINKS
static void set_deferred_symlink OF((__GPRO__ slinkentry * slnk_entry));
#endif
#ifdef SET_DIR_ATTRIB
static int Cdecl dircomp OF((ZCONST zvoid * a, ZCONST zvoid* b));
#endif

/*******************************/
/*  Strings used in extract.c  */
/*******************************/

static ZCONST char Far VersionMsg[] = "   skipping: %-22s  need %s compat. v%u.%u (can do v%u.%u)\n";
static ZCONST char Far ComprMsgNum[] = "   skipping: %-22s  unsupported compression method %u\n";
#ifndef SFX
static ZCONST char Far ComprMsgName[] = "   skipping: %-22s  `%s' method not supported\n";
static ZCONST char Far CmprNone[] = "store";
static ZCONST char Far CmprShrink[] = "shrink";
static ZCONST char Far CmprReduce[] = "reduce";
static ZCONST char Far CmprImplode[] = "implode";
static ZCONST char Far CmprTokenize[] = "tokenize";
static ZCONST char Far CmprDeflate[] = "deflate";
static ZCONST char Far CmprDeflat64[] = "deflate64";
static ZCONST char Far CmprDCLImplode[] = "DCL implode";
static ZCONST char Far CmprBzip[] = "bzip2";
static ZCONST char Far CmprLZMA[] = "LZMA";
static ZCONST char Far CmprIBMTerse[] = "IBM/Terse";
static ZCONST char Far CmprIBMLZ77[] = "IBM LZ77";
static ZCONST char Far CmprWavPack[] = "WavPack";
static ZCONST char Far CmprPPMd[] = "PPMd";
static ZCONST char Far* ComprNames[NUM_METHODS] = {CmprNone,     CmprShrink,     CmprReduce, CmprReduce, CmprReduce,   CmprReduce,  CmprImplode, CmprTokenize, CmprDeflate,
                                                   CmprDeflat64, CmprDCLImplode, CmprBzip,   CmprLZMA,   CmprIBMTerse, CmprIBMLZ77, CmprWavPack, CmprPPMd};
static ZCONST unsigned ComprIDs[NUM_METHODS] = {STORED,      SHRUNK,      REDUCED1, REDUCED2, REDUCED3,  REDUCED4,  IMPLODED,  TOKENIZED, DEFLATED,
                                                ENHDEFLATED, DCLIMPLODED, BZIPPED,  LZMAED,   IBMTERSED, IBMLZ77ED, WAVPACKED, PPMDED};
#endif /* !SFX */
static ZCONST char Far FilNamMsg[] = "%s:  bad filename length (%s)\n";
#ifndef SFX
static ZCONST char Far WarnNoMemCFName[] = "%s:  warning, no memory for comparison with local header\n";
static ZCONST char Far LvsCFNamMsg[] =
    "%s:  mismatching \"local\" filename (%s),\n\
         continuing with \"central\" filename version\n";
#endif /* !SFX */
#if (!defined(SFX) && defined(UNICODE_SUPPORT))
#if defined(__GNUC__)
static ZCONST char Far GP11FlagsDiffer[] __attribute__((unused)) =
#else
static ZCONST char Far GP11FlagsDiffer[] =
#endif
    "file #%lu (%s):\n\
         mismatch between local and central GPF bit 11 (\"UTF-8\"),\n\
         continuing with central flag (IsUTF8 = %d)\n";
#endif /* !SFX && UNICODE_SUPPORT */
static ZCONST char Far WrnStorUCSizCSizDiff[] =
    "%s:  ucsize %s <> csize %s for STORED entry\n\
         continuing with \"compressed\" size value\n";
static ZCONST char Far ExtFieldMsg[] = "%s:  bad extra field length (%s)\n";
static ZCONST char Far OffsetMsg[] = "file #%lu:  bad zipfile offset (%s):  %ld\n";
static ZCONST char Far ExtractMsg[] = "%8sing: %-22s  %s%s";

static ZCONST char Far BadFileCommLength[] = "%s:  bad file comment length\n";
static ZCONST char Far LocalHdrSig[] = "local header sig";
static ZCONST char Far BadLocalHdr[] = "file #%lu:  bad local header\n";
static ZCONST char Far AttemptRecompensate[] = "  (attempting to re-compensate)\n";
#ifndef SFX
static ZCONST char Far BackslashPathSep[] = "warning:  %s appears to use backslashes as path separators\n";
#endif
static ZCONST char Far AbsolutePathWarning[] = "warning:  stripped absolute path spec from %s\n";
static ZCONST char Far SkipVolumeLabel[] = "   skipping: %-22s  %svolume label\n";

#ifdef SET_DIR_ATTRIB /* messages of code for setting directory attributes */
static ZCONST char Far DirlistEntryNoMem[] = "warning:  cannot alloc memory for dir times/permissions/UID/GID\n";
static ZCONST char Far DirlistSortNoMem[] = "warning:  cannot alloc memory to sort dir times/perms/etc.\n";
static ZCONST char Far DirlistSetAttrFailed[] = "warning:  set times/attribs failed for %s\n";
static ZCONST char Far DirlistFailAttrSum[] = "     failed setting times/attribs for %lu dir entries";
#endif

#ifdef SYMLINKS /* messages of the deferred symlinks handler */
static ZCONST char Far SymLnkWarnNoMem[] =
    "warning:  deferred symlink (%s) failed:\n\
          out of memory\n";
static ZCONST char Far SymLnkWarnInvalid[] =
    "warning:  deferred symlink (%s) failed:\n\
          invalid placeholder file\n";
static ZCONST char Far SymLnkDeferred[] = "finishing deferred symbolic links:\n";
static ZCONST char Far SymLnkFinish[] = "  %-22s -> %s\n";
#endif

static ZCONST char Far ReplaceQuery[] = "replace %s? [y]es, [n]o, [A]ll, [N]one, [r]ename: ";
static ZCONST char Far AssumeNone[] = " NULL\n(EOF or read error, treating as \"[N]one\" ...)\n";
static ZCONST char Far NewNameQuery[] = "new name: ";
static ZCONST char Far InvalidResponse[] = "error:  invalid response [%s]\n";

static ZCONST char Far ErrorInArchive[] = "At least one %serror was detected in %s.\n";
static ZCONST char Far ZeroFilesTested[] = "Caution:  zero files tested in %s.\n";

static ZCONST char Far VMSFormatQuery[] = "\n%s:  stored in VMS format.  Extract anyway? (y/n) ";

#if CRYPT
static ZCONST char Far SkipCannotGetPasswd[] = "   skipping: %-22s  unable to get password\n";
static ZCONST char Far SkipIncorrectPasswd[] = "   skipping: %-22s  incorrect password\n";
static ZCONST char Far FilesSkipBadPasswd[] = "%lu file%s skipped because of incorrect password.\n";
#else
static ZCONST char Far SkipEncrypted[] = "   skipping: %-22s  encrypted (not supported)\n";
#endif

static ZCONST char Far NoErrInCompData[] = "No errors detected in compressed data of %s.\n";
static ZCONST char Far NoErrInTestedFiles[] = "No errors detected in %s for the %lu file%s tested.\n";
static ZCONST char Far FilesSkipped[] = "%lu file%s skipped because of unsupported compression or encoding.\n";

static ZCONST char Far ErrUnzipFile[] = "  error:  %s%s %s\n";
static ZCONST char Far ErrUnzipNoFile[] = "\n  error:  %s%s\n";
static ZCONST char Far NotEnoughMem[] = "not enough memory to ";
static ZCONST char Far InvalidComprData[] = "invalid compressed data to ";
static ZCONST char Far Inflate[] = "inflate";

#ifndef HAVE_UNLINK
static ZCONST char Far FileTruncated[] = "warning:  %s is probably truncated\n";
#endif

static ZCONST char Far FileUnknownCompMethod[] = "%s:  unknown compression method\n";
static ZCONST char Far BadCRC[] = " bad CRC %08lx  (should be %08lx)\n";

/* TruncEAs[] also used in OS/2 mapname(), close_outfile() */
char ZCONST Far TruncEAs[] = " compressed EA data missing (%d bytes)%s";
char ZCONST Far TruncNTSD[] = " compressed WinNT security data missing (%d bytes)%s";

#ifndef SFX
static ZCONST char Far InconsistEFlength[] =
    "bad extra-field entry:\n \
     EF block length (%u bytes) exceeds remaining EF data (%u bytes)\n";
static ZCONST char Far TooSmallEBlength[] =
    "bad extra-field entry:\n \
     EF block length (%u bytes) invalid (< %d)\n";
static ZCONST char Far InvalidComprDataEAs[] = " invalid compressed data for EAs\n";
static ZCONST char Far UnsuppNTSDVersEAs[] = " unsupported NTSD EAs version %d\n";
static ZCONST char Far BadCRC_EAs[] = " bad CRC for extended attributes\n";
static ZCONST char Far UnknComprMethodEAs[] = " unknown compression method for EAs (%u)\n";
static ZCONST char Far NotEnoughMemEAs[] = " out of memory while inflating EAs\n";
static ZCONST char Far UnknErrorEAs[] = " unknown error on extended attributes\n";
#endif /* !SFX */

static ZCONST char Far UnsupportedExtraField[] = "\nerror:  unsupported extra-field compression type (%u)--skipping\n";
static ZCONST char Far BadExtraFieldCRC[] = "error [%s]:  bad extra-field CRC %08lx (should be %08lx)\n";
static ZCONST char Far NotEnoughMemCover[] = "error: not enough memory for bomb detection\n";
static ZCONST char Far OverlappedComponents[] = "error: invalid zip file with overlapped components (possible zip bomb)\n";

/* A growable list of spans. */
typedef zusz_t bound_t;
typedef struct {
    bound_t beg; /* start of the span */
    bound_t end; /* one past the end of the span */
} span_t;
typedef struct {
    span_t* span; /* allocated, distinct, and sorted list of spans */
    size_t num;   /* number of spans in the list */
    size_t max;   /* allocated number of spans (num <= max) */
} cover_t;

static size_t cover_find OF((cover_t*, bound_t));
static int cover_within OF((cover_t*, bound_t));
static int cover_add OF((cover_t*, bound_t, bound_t));

/*
 * Return the index of the first span in cover whose beg is greater than val.
 * If there is no such span, then cover->num is returned.
 */
static size_t cover_find(cover, val)
cover_t* cover;
bound_t val;
{
    size_t lo = 0, hi = cover->num;
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        if (val < cover->span[mid].beg)
            hi = mid;
        else
            lo = mid + 1;
    }
    return hi;
}

/* Return true if val lies within any one of the spans in cover. */
static int cover_within(cover, val)
cover_t* cover;
bound_t val;
{
    size_t pos = cover_find(cover, val);
    return pos > 0 && val < cover->span[pos - 1].end;
}

/*
 * Add a new span to the list, but only if the new span does not overlap any
 * spans already in the list. The new span covers the values beg..end-1. beg
 * must be less than end.
 *
 * Keep the list sorted and merge adjacent spans. Grow the allocated space for
 * the list as needed. On success, 0 is returned. If the new span overlaps any
 * existing spans, then 1 is returned and the new span is not added to the
 * list. If the new span is invalid because beg is greater than or equal to
 * end, then -1 is returned. If the list needs to be grown but the memory
 * allocation fails, then -2 is returned.
 */
static int cover_add(cover, beg, end)
cover_t* cover;
bound_t beg;
bound_t end;
{
    size_t pos;
    int prec, foll;

    if (beg >= end)
        /* The new span is invalid. */
        return -1;

    /* Find where the new span should go, and make sure that it does not
       overlap with any existing spans. */
    pos = cover_find(cover, beg);
    if ((pos > 0 && beg < cover->span[pos - 1].end) || (pos < cover->num && end > cover->span[pos].beg))
        return 1;

    /* Check for adjacencies. */
    prec = pos > 0 && beg == cover->span[pos - 1].end;
    foll = pos < cover->num && end == cover->span[pos].beg;
    if (prec && foll) {
        /* The new span connects the preceding and following spans. Merge the
           following span into the preceding span, and delete the following
           span. */
        cover->span[pos - 1].end = cover->span[pos].end;
        cover->num--;
        memmove(cover->span + pos, cover->span + pos + 1, (cover->num - pos) * sizeof(span_t));
    }
    else if (prec)
        /* The new span is adjacent only to the preceding span. Extend the end
           of the preceding span. */
        cover->span[pos - 1].end = end;
    else if (foll)
        /* The new span is adjacent only to the following span. Extend the
           beginning of the following span. */
        cover->span[pos].beg = beg;
    else {
        /* The new span has gaps between both the preceding and the following
           spans. Assure that there is room and insert the span.  */
        if (cover->num == cover->max) {
            size_t max = cover->max == 0 ? 16 : cover->max << 1;
            span_t* span = realloc(cover->span, max * sizeof(span_t));
            if (span == NULL)
                return -2;
            cover->span = span;
            cover->max = max;
        }
        memmove(cover->span + pos + 1, cover->span + pos, (cover->num - pos) * sizeof(span_t));
        cover->num++;
        cover->span[pos].beg = beg;
        cover->span[pos].end = end;
    }
    return 0;
}

/**************************************/
/*  Function extract_or_test_files()  */
/**************************************/

int extract_or_test_files(__G) /* return PK-type error code */
    __GDEF {
    unsigned i, j;
    zoff_t cd_bufstart = 0;
    uch* cd_inptr = NULL;
    int cd_incnt = 0;
    ulg filnum = 0L, blknum = 0L;
    int reached_end;
#ifndef SFX
    int no_endsig_found;
#endif
    int error, error_in_archive = PK_COOL;
    int *fn_matched = NULL, *xn_matched = NULL;
    zucn_t members_processed;
    ulg num_skipped = 0L, num_bad_pwd = 0L;
    zoff_t old_extra_bytes = 0L;
#ifdef SET_DIR_ATTRIB
    unsigned num_dirs = 0;
    direntry *dirlist = (direntry*)NULL, **sorted_dirlist = (direntry**)NULL;
#endif

    /* ---- Once-only CRC table init (moved here to be “data-path only”). ---- */
    if (CRC_32_TAB == NULL) {
        CRC_32_TAB = get_crc_table();
        if (CRC_32_TAB == NULL)
            return PK_MEM;
    }

#if (!defined(SFX) || defined(SFX_EXDIR))
    /* Optional extraction root existence check. */
    if (uO.exdir != (char*)NULL && G.extract_flag) {
        G.create_dirs = !uO.fflag;
        error = checkdir(__G__ uO.exdir, ROOT);
        if (error > MPN_INF_SKIP) /* OOM or file in the way */
            return (error == MPN_NOMEM) ? PK_MEM : PK_ERR;
    }
#endif /* !SFX || SFX_EXDIR */

    /* ---- Bomb/overlap cover initialization ---- */
    if (G.cover == NULL) {
        G.cover = malloc(sizeof(cover_t));
        if (G.cover == NULL) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(NotEnoughMemCover)));
            return PK_MEM;
        }
        ((cover_t*)G.cover)->span = NULL;
        ((cover_t*)G.cover)->max = 0;
    }
    ((cover_t*)G.cover)->num = 0;

    /* Central directory span */
    if (cover_add((cover_t*)G.cover, G.extra_bytes + G.ecrec.offset_start_central_directory, G.extra_bytes + G.ecrec.offset_start_central_directory + G.ecrec.size_central_directory) != 0) {
        Info(slide, 0x401, ((char*)slide, LoadFarString(NotEnoughMemCover)));
        return PK_MEM;
    }
    /* leading extra bytes, Zip64 EoCD (if any), and EoCD */
    if ((G.extra_bytes != 0 && cover_add((cover_t*)G.cover, (bound_t)0, (bound_t)G.extra_bytes) != 0) ||
        (G.ecrec.have_ecr64 && cover_add((cover_t*)G.cover, G.ecrec.ec64_start, G.ecrec.ec64_end) != 0) || cover_add((cover_t*)G.cover, G.ecrec.ec_start, G.ecrec.ec_end) != 0) {
        Info(slide, 0x401, ((char*)slide, LoadFarString(OverlappedComponents)));
        return PK_BOMB;
    }

    /* ---- General state ---- */
    G.pInfo = G.info;
#if CRYPT
    G.newzip = TRUE;
#endif
#ifndef SFX
    G.reported_backslash = FALSE;
#endif

    /* Track which include/exclude patterns matched (optional). */
    if (G.filespecs > 0) {
        fn_matched = (int*)malloc(G.filespecs * sizeof(int));
        if (fn_matched)
            for (i = 0; i < G.filespecs; ++i)
                fn_matched[i] = FALSE;
    }
    if (G.xfilespecs > 0) {
        xn_matched = (int*)malloc(G.xfilespecs * sizeof(int));
        if (xn_matched)
            for (i = 0; i < G.xfilespecs; ++i)
                xn_matched[i] = FALSE;
    }

    /* ===================== Central directory block loop ===================== */
    members_processed = 0;
#ifndef SFX
    no_endsig_found = FALSE;
#endif
    reached_end = FALSE;

    while (!reached_end) {
        j = 0;

        /* -------- Read a block of central directory entries -------- */
        while (j < DIR_BLKSIZ) {
            G.pInfo = &G.info[j];

            if (readbuf(__G__ G.sig, 4) == 0) {
                error_in_archive = PK_EOF;
                reached_end = TRUE;
                break;
            }

            /* Not a central header? Validate end conditions. */
            if (memcmp(G.sig, central_hdr_sig, 4) != 0) {
                /* Was count consistent with EoCD? */
                if ((members_processed & (G.ecrec.have_ecr64 ? MASK_ZUCN64 : MASK_ZUCN16)) == G.ecrec.total_entries_central_dir) {
#ifndef SFX
                    /* “Should” be at EoCD (Zip64 or classic). */
                    no_endsig_found = ((memcmp(G.sig, (G.ecrec.have_ecr64 ? end_central64_sig : end_central_sig), 4) != 0) && (!G.ecrec.is_zip64_archive) && (memcmp(G.sig, end_central_sig, 4) != 0));
#endif
                }
                else {
                    /* Central directory inconsistent. */
                    Info(slide, 0x401, ((char*)slide, LoadFarString(CentSigMsg), j + blknum * DIR_BLKSIZ + 1));
                    Info(slide, 0x401, ((char*)slide, "%s", LoadFarString(ReportMsg)));
                    error_in_archive = PK_BADERR;
                }
                reached_end = TRUE;
                break;
            }

            /* Parse and validate this CD file header. */
            error = process_cdir_file_hdr(__G);
            if (error != PK_COOL) {
                error_in_archive = error; /* PK_EOF only here */
                reached_end = TRUE;
                break;
            }

            /* Filename */
            error = do_string(__G__ G.crec.filename_length, DS_FN);
            if (error != PK_COOL) {
                if (error > error_in_archive)
                    error_in_archive = error;
                if (error > PK_WARN) { /* fatal */
                    Info(slide, 0x401, ((char*)slide, LoadFarString(FilNamMsg), FnFilter1(G.filename), "central"));
                    reached_end = TRUE;
                    break;
                }
            }

            /* Extra field(s) */
            G.pInfo->zip64 = FALSE;
            error = do_string(__G__ G.crec.extra_field_length, EXTRA_FIELD);
            if (error != PK_COOL) {
                if (error > error_in_archive)
                    error_in_archive = error;
                if (error > PK_WARN) { /* fatal */
                    Info(slide, 0x401, ((char*)slide, LoadFarString(ExtFieldMsg), FnFilter1(G.filename), "central"));
                    reached_end = TRUE;
                    break;
                }
            }

            /* Comment (skip unless Amiga filenotes requested) */
            error = do_string(__G__ G.crec.file_comment_length, SKIP);
            if (error != PK_COOL) {
                if (error > error_in_archive)
                    error_in_archive = error;
                if (error > PK_WARN) { /* fatal */
                    Info(slide, 0x421, ((char*)slide, LoadFarString(BadFileCommLength), FnFilter1(G.filename)));
                    reached_end = TRUE;
                    break;
                }
            }

            /* Selection logic */
            if (G.process_all_files) {
                if (store_info(__G))
                    ++j;
                else
                    ++num_skipped;
            }
            else {
                int do_this_file = (G.filespecs == 0);

                if (!do_this_file) {
                    for (i = 0; i < G.filespecs; i++) {
                        if (match(G.filename, G.pfnames[i], uO.C_flag WISEP)) {
                            do_this_file = TRUE;
                            if (fn_matched)
                                fn_matched[i] = TRUE;
                            break;
                        }
                    }
                }
                if (do_this_file) {
                    for (i = 0; i < G.xfilespecs; i++) {
                        if (match(G.filename, G.pxnames[i], uO.C_flag WISEP)) {
                            do_this_file = FALSE;
                            if (xn_matched)
                                xn_matched[i] = TRUE;
                            break;
                        }
                    }
                }
                if (do_this_file) {
                    if (store_info(__G))
                        ++j;
                    else
                        ++num_skipped;
                }
            }

            members_processed++;
        } /* (collect CD entries into current block) */

        /* Save CD position to resume after extracting the block. */
        cd_bufstart = G.cur_zipfile_bufstart;
        cd_inptr = G.inptr;
        cd_incnt = G.incnt;

        /* ---------------- Extract/test the current block ---------------- */
        error = extract_or_test_entrylist(__G__ j, &filnum, &num_bad_pwd, &old_extra_bytes
#ifdef SET_DIR_ATTRIB
                                          ,
                                          &num_dirs, &dirlist
#endif
                                          ,
                                          error_in_archive);
        if (error != PK_COOL) {
            if (error > error_in_archive)
                error_in_archive = error;

            /* Stop on disk full (hard), user break, or bomb detection. */
            if (G.disk_full > 1 || error_in_archive == IZ_CTRLC || error == PK_BOMB) {
                reached_end = FALSE; /* signal premature stop */
                break;               /* abort scanning CD */
            }
        }

        /* -------- Restore CD input buffer for next batch -------- */
#ifdef USE_STRM_INPUT
        if (zfseeko(G.zipfd, cd_bufstart, SEEK_SET) != 0) {
            error_in_archive = (error_in_archive > PK_ERR) ? error_in_archive : PK_ERR;
            break;
        }
        G.cur_zipfile_bufstart = zftello(G.zipfd);
#else
        G.cur_zipfile_bufstart = zlseek(G.zipfd, cd_bufstart, SEEK_SET);
        if (G.cur_zipfile_bufstart != cd_bufstart) {
            error_in_archive = (error_in_archive > PK_ERR) ? error_in_archive : PK_ERR;
            break;
        }
#endif
        {
            /* Refill inbuf; ensure we got something reasonable back. */
            int r = read(G.zipfd, (char*)G.inbuf, INBUFSIZ);
            if (r < 0) {
                error_in_archive = (error_in_archive > PK_ERR) ? error_in_archive : PK_ERR;
                break;
            }
        }
        G.inptr = cd_inptr;
        G.incnt = cd_incnt;
        ++blknum;

#ifdef TEST
        printf("\ncd_bufstart = %ld (%.8lXh)\n", cd_bufstart, cd_bufstart);
        printf("cur_zipfile_bufstart = %ld (%.8lXh)\n", cur_zipfile_bufstart, cur_zipfile_bufstart);
        printf("inptr-inbuf = %d\n", (int)(G.inptr - G.inbuf));
        printf("incnt = %d\n\n", G.incnt);
#endif
    } /* while blocks */

    /* ===================== Deferred symlink completion ===================== */
#ifdef SYMLINKS
    if (G.slink_last != NULL) {
        if (QCOND2)
            Info(slide, 0, ((char*)slide, LoadFarString(SymLnkDeferred)));
        while (G.slink_head != NULL) {
            set_deferred_symlink(__G__ G.slink_head);
            G.slink_last = G.slink_head;
            G.slink_head = G.slink_last->next;
            free(G.slink_last);
        }
        G.slink_last = NULL;
    }
#endif /* SYMLINKS */

    /* ===================== Directory attributes (deepest first) ===================== */
#ifdef SET_DIR_ATTRIB
    if (num_dirs > 0) {
        sorted_dirlist = (direntry**)malloc(num_dirs * sizeof(direntry*));
        if (sorted_dirlist == (direntry**)NULL) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(DirlistSortNoMem)));
            while (dirlist != (direntry*)NULL) {
                direntry* d = dirlist;
                dirlist = dirlist->next;
                free(d);
            }
        }
        else {
            ulg ndirs_fail = 0;

            if (num_dirs == 1) {
                sorted_dirlist[0] = dirlist;
            }
            else {
                for (i = 0; i < num_dirs; ++i) {
                    sorted_dirlist[i] = dirlist;
                    dirlist = dirlist->next;
                }
                qsort((char*)sorted_dirlist, num_dirs, sizeof(direntry*), dircomp);
            }

            Trace((stderr, "setting directory times/perms/attributes\n"));
            for (i = 0; i < num_dirs; ++i) {
                direntry* d = sorted_dirlist[i];
                Trace((stderr, "dir = %s\n", d->fn));
                error = set_direc_attribs(__G__ d);
                if (error != PK_OK) {
                    ndirs_fail++;
                    Info(slide, 0x201, ((char*)slide, LoadFarString(DirlistSetAttrFailed), d->fn));
                    if (!error_in_archive)
                        error_in_archive = error;
                }
                free(d);
            }
            free(sorted_dirlist);

            if (!uO.tflag && QCOND2 && ndirs_fail > 0)
                Info(slide, 0, ((char*)slide, LoadFarString(DirlistFailAttrSum), ndirs_fail));
        }
    }
#endif /* SET_DIR_ATTRIB */

    /* ===================== Report unmatched patterns (if completed) ===================== */
    if (fn_matched) {
        if (reached_end) {
            for (i = 0; i < G.filespecs; ++i) {
                if (!fn_matched[i]) {
#ifdef DLL
                    if (!G.redirect_data && !G.redirect_text)
                        Info(slide, 0x401, ((char*)slide, LoadFarString(FilenameNotMatched), G.pfnames[i]));
                    else
                        setFileNotFound(__G);
#else
                    Info(slide, 1, ((char*)slide, LoadFarString(FilenameNotMatched), G.pfnames[i]));
#endif
                    if (error_in_archive <= PK_WARN)
                        error_in_archive = PK_FIND;
                }
            }
        }
        free((zvoid*)fn_matched);
    }

    if (xn_matched) {
        if (reached_end) {
            for (i = 0; i < G.xfilespecs; ++i) {
                if (!xn_matched[i])
                    Info(slide, 0x401, ((char*)slide, LoadFarString(ExclFilenameNotMatched), G.pxnames[i]));
            }
        }
        free((zvoid*)xn_matched);
    }

    /* If we bailed out early, skip completeness checks/summary. */
    if (!reached_end)
        return error_in_archive;

    /* ===================== Sanity check we really hit EoCD ===================== */
#ifndef SFX
    if (no_endsig_found) {
        Info(slide, 0x401, ((char*)slide, "%s", LoadFarString(EndSigMsg)));
        Info(slide, 0x401, ((char*)slide, "%s", LoadFarString(ReportMsg)));
        if (!error_in_archive)
            error_in_archive = PK_WARN;
    }
#endif /* !SFX */

    /* ===================== Test-mode summary ===================== */
    if (uO.tflag) {
        ulg num = filnum - num_bad_pwd;

        if (uO.qflag < 2) {
            if (error_in_archive) {
                Info(slide, 0, ((char*)slide, LoadFarString(ErrorInArchive), (error_in_archive == PK_WARN) ? "warning-" : "", G.zipfn));
            }
            else if (num == 0L) {
                Info(slide, 0, ((char*)slide, LoadFarString(ZeroFilesTested), G.zipfn));
            }
            else if (G.process_all_files && (num_skipped + num_bad_pwd == 0L)) {
                Info(slide, 0, ((char*)slide, LoadFarString(NoErrInCompData), G.zipfn));
            }
            else {
                Info(slide, 0, ((char*)slide, LoadFarString(NoErrInTestedFiles), G.zipfn, num, (num == 1L) ? "" : "s"));
            }
            if (num_skipped > 0L)
                Info(slide, 0, ((char*)slide, LoadFarString(FilesSkipped), num_skipped, (num_skipped == 1L) ? "" : "s"));
#if CRYPT
            if (num_bad_pwd > 0L)
                Info(slide, 0, ((char*)slide, LoadFarString(FilesSkipBadPasswd), num_bad_pwd, (num_bad_pwd == 1L) ? "" : "s"));
#endif
        }
    }

    /* Final status normalization. */
    if ((filnum == 0) && error_in_archive <= PK_WARN) {
        error_in_archive = (num_skipped > 0L) ? IZ_UNSUP : PK_FIND;
    }
#if CRYPT
    else if ((filnum == num_bad_pwd) && error_in_archive <= PK_WARN) {
        error_in_archive = IZ_BADPWD;
    }
#endif
    else if ((num_skipped > 0L) && error_in_archive <= PK_WARN) {
        error_in_archive = IZ_UNSUP; /* (was PK_WARN) */
    }
#if CRYPT
    else if ((num_bad_pwd > 0L) && !error_in_archive) {
        error_in_archive = PK_WARN;
    }
#endif

    return error_in_archive;
} /* extract_or_test_files() */

/***************************/
/*  Function store_info()  */
/***************************/

static int store_info(__G) /* return 0 if skipping, 1 if OK */
    __GDEF {
    /* -------------------------
       Compression support gates
       ------------------------- */
#ifdef USE_BZIP2
#define KNOWN_BZ2 (G.crec.compression_method == BZIPPED)
#else
#define KNOWN_BZ2 0
#endif

#ifdef USE_LZMA
#define KNOWN_LZMA (G.crec.compression_method == LZMAED)
#else
#define KNOWN_LZMA 0
#endif

#ifdef USE_WAVP
#define KNOWN_WAVP (G.crec.compression_method == WAVPACKED)
#else
#define KNOWN_WAVP 0
#endif

#ifdef USE_PPMD
#define KNOWN_PPMD (G.crec.compression_method == PPMDED)
#else
#define KNOWN_PPMD 0
#endif

/* Avoid undefined-symbol build breaks: gate optional methods. */
#ifdef ENHDEFLATED
#define IS_ENHDEFL (G.crec.compression_method == ENHDEFLATED)
#else
#define IS_ENHDEFL 0
#endif

#ifdef DEFLATE64
#define IS_DEFLATE64 (G.crec.compression_method == DEFLATE64)
#else
#define IS_DEFLATE64 0
#endif

#define KNOWN_DEFLATE_OR_BETTER (G.crec.compression_method == STORED || G.crec.compression_method == DEFLATED || IS_ENHDEFL || IS_DEFLATE64)

#ifdef SFX
    /* SFX builds: core set plus optional plugins. */
#define METHOD_KNOWN (KNOWN_DEFLATE_OR_BETTER || KNOWN_BZ2 || KNOWN_LZMA || KNOWN_WAVP || KNOWN_PPMD)
#else
    /* Full builds may include legacy methods depending on license flags. */
#ifdef COPYRIGHT_CLEAN
#define ALLOW_REDUCED 0
#else
#define ALLOW_REDUCED 1
#endif
#ifdef LZW_CLEAN
#define ALLOW_SHRUNK 0
#else
#define ALLOW_SHRUNK 1
#endif
#define IS_REDUCED (G.crec.compression_method >= REDUCED1 && G.crec.compression_method <= REDUCED4)
#define IS_SHRUNK (G.crec.compression_method == SHRUNK)
#define IS_TOKEN (G.crec.compression_method == TOKENIZED)
#define METHOD_KNOWN ((ALLOW_REDUCED && IS_REDUCED) || (ALLOW_SHRUNK && IS_SHRUNK) || (!IS_TOKEN && KNOWN_DEFLATE_OR_BETTER) || KNOWN_BZ2 || KNOWN_LZMA || KNOWN_WAVP || KNOWN_PPMD)
#endif /* !SFX */

    /* Effective “unzip version supported” (bzip2 may raise this). */
    {
        const int unzvers_support =
#if defined(USE_BZIP2) && (UNZIP_VERSION < UNZIP_BZ2VERS)
            (KNOWN_BZ2 ? UNZIP_BZ2VERS : UNZIP_VERSION);
#else
            UNZIP_VERSION;
#endif

        /* -------------------------------------------
           Fill per-entry info and basic mode decisions
           ------------------------------------------- */
        G.pInfo->encrypted = (G.crec.general_purpose_bit_flag & 1) != 0;
        G.pInfo->ExtLocHdr = (G.crec.general_purpose_bit_flag & 8) == 8;
        G.pInfo->textfile = (G.crec.internal_file_attributes & 1) != 0;
        G.pInfo->crc = G.crec.crc32;
        G.pInfo->compr_size = G.crec.csize;
        G.pInfo->uncompr_size = G.crec.ucsize;

        /* textmode: 0=never, 1=auto from header, 2=always */
        G.pInfo->textmode = (uO.aflag == 2) ? TRUE : (uO.aflag == 1) ? G.pInfo->textfile : FALSE;

        /* --------------------------------
           Version-needed compatibility gate
           -------------------------------- */
        if (G.crec.version_needed_to_extract[1] == VMS_) {
            /* VMS-specific features required */
            if (G.crec.version_needed_to_extract[0] > VMS_UNZIP_VERSION) {
                if (!((uO.tflag && uO.qflag) || (!uO.tflag && !QCOND2))) {
                    Info(slide, 0x401,
                         ((char*)slide, LoadFarString(VersionMsg), FnFilter1(G.filename), "VMS", G.crec.version_needed_to_extract[0] / 10, G.crec.version_needed_to_extract[0] % 10,
                          VMS_UNZIP_VERSION / 10, VMS_UNZIP_VERSION % 10));
                }
                return 0;
            }
            /* Non-VMS: ask before extracting when VMS attributes present (unless -o or -t) */
            if (!uO.tflag && !IS_OVERWRT_ALL) {
                Info(slide, 0x481, ((char*)slide, LoadFarString(VMSFormatQuery), FnFilter1(G.filename)));
                if (fgets(G.answerbuf, sizeof(G.answerbuf), stdin) == NULL || (G.answerbuf[0] != 'y' && G.answerbuf[0] != 'Y'))
                    return 0;
            }
        }
        else {
            /* Generic PK version-needed */
            if (G.crec.version_needed_to_extract[0] > unzvers_support) {
                if (!((uO.tflag && uO.qflag) || (!uO.tflag && !QCOND2))) {
                    Info(slide, 0x401,
                         ((char*)slide, LoadFarString(VersionMsg), FnFilter1(G.filename), "PK", G.crec.version_needed_to_extract[0] / 10, G.crec.version_needed_to_extract[0] % 10,
                          unzvers_support / 10, unzvers_support % 10));
                }
                return 0;
            }
        }
    } /* end local scope for unzvers_support */

    /* -------------------------
       Unknown/unsupported method
       ------------------------- */
    if (!METHOD_KNOWN) {
        if (!((uO.tflag && uO.qflag) || (!uO.tflag && !QCOND2))) {
#ifndef SFX
            {
                unsigned cmpridx = find_compr_idx(G.crec.compression_method);
                if (cmpridx < NUM_METHODS)
                    Info(slide, 0x401, ((char*)slide, LoadFarString(ComprMsgName), FnFilter1(G.filename), LoadFarStringSmall(ComprNames[cmpridx])));
                else
#endif
                    Info(slide, 0x401, ((char*)slide, LoadFarString(ComprMsgNum), FnFilter1(G.filename), G.crec.compression_method));
#ifndef SFX
            }
#endif
        }
        return 0;
    }

#if (!CRYPT)
    if (G.pInfo->encrypted) {
        if (!((uO.tflag && uO.qflag) || (!uO.tflag && !QCOND2)))
            Info(slide, 0x401, ((char*)slide, LoadFarString(SkipEncrypted), FnFilter1(G.filename)));
        return 0;
    }
#endif /* !CRYPT */

#ifndef SFX
    /* Keep a copy of the name from the central header for later comparison. */
    G.pInfo->cfilname = zfmalloc(strlen(G.filename) + 1);
    if (G.pInfo->cfilname == NULL) {
        Info(slide, 0x401, ((char*)slide, LoadFarString(WarnNoMemCFName), FnFilter1(G.filename)));
    }
    else {
        zfstrcpy(G.pInfo->cfilname, G.filename);
    }
#endif /* !SFX */

    /* Map attributes to local format (ignore return for now, matches legacy). */
    mapattr(__G);

    /* Persist location for later local header seek. */
    G.pInfo->diskstart = G.crec.disk_number_start;
    G.pInfo->offset = (zoff_t)G.crec.relative_offset_local_header;

    return 1;
} /* end function store_info() */

#ifndef SFX
/*******************************/
/*  Function find_compr_idx()  */
/*******************************/

unsigned find_compr_idx(compr_methodnum)
unsigned compr_methodnum;
{
    unsigned i;

    for (i = 0; i < NUM_METHODS; i++) {
        if (ComprIDs[i] == compr_methodnum)
            break;
    }
    return i;
}
#endif /* !SFX */

/******************************************/
/*  Function extract_or_test_entrylist()  */
/******************************************/

static int extract_or_test_entrylist(__G__ numchunk,
                                     pfilnum,
                                     pnum_bad_pwd,
                                     pold_extra_bytes,
#ifdef SET_DIR_ATTRIB
                                     pnum_dirs,
                                     pdirlist,
#endif
                                     error_in_archive) /* return PK-type error code */
__GDEF
unsigned numchunk;
ulg* pfilnum;
ulg* pnum_bad_pwd;
zoff_t* pold_extra_bytes;
#ifdef SET_DIR_ATTRIB
unsigned* pnum_dirs;
direntry** pdirlist;
#endif
int error_in_archive;
{
    unsigned i;
    int renamed, query;
    int skip_entry;
    zoff_t bufstart, inbuf_offset, request;
    int error, errcode;

    /* skip_entry states */
    enum { SKIP_NO = 0, SKIP_Y_EXISTING = 1, SKIP_Y_NONEXIST = 2 };

    /* ------------------------------
       Process each entry in the chunk
       ------------------------------ */
    for (i = 0; i < numchunk; ++i) {
        (*pfilnum)++;
        G.pInfo = &G.info[i];

        /* Compute absolute request offset into the zip stream. */
        request = G.pInfo->offset + G.extra_bytes;

        /* Guard against overlapping/bomb conditions. */
        if (cover_within((cover_t*)G.cover, (bound_t)request)) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(OverlappedComponents)));
            return PK_BOMB;
        }

        inbuf_offset = request % INBUFSIZ;
        bufstart = request - inbuf_offset;

        if (request < 0) {
            Info(slide, 0x401, ((char*)slide, LoadFarStringSmall(SeekMsg), G.zipfn, LoadFarString(ReportMsg)));
            error_in_archive = PK_ERR;

            /* First entry with nonzero extra_bytes? Try compensating once. */
            if (*pfilnum == 1 && G.extra_bytes != 0L) {
                Info(slide, 0x401, ((char*)slide, LoadFarString(AttemptRecompensate)));
                *pold_extra_bytes = G.extra_bytes;
                G.extra_bytes = 0L;
                request = G.pInfo->offset;
                inbuf_offset = request % INBUFSIZ;
                bufstart = request - inbuf_offset;
                if (request < 0) {
                    Info(slide, 0x401, ((char*)slide, LoadFarStringSmall(SeekMsg), G.zipfn, LoadFarString(ReportMsg)));
                    error_in_archive = PK_BADERR;
                    continue;
                }
            }
            else {
                error_in_archive = PK_BADERR;
                continue;
            }
        }

        /* Refill buffer around target position if needed. */
        if (bufstart != G.cur_zipfile_bufstart) {
#ifdef USE_STRM_INPUT
            zfseeko(G.zipfd, bufstart, SEEK_SET);
            G.cur_zipfile_bufstart = zftello(G.zipfd);
#else
            G.cur_zipfile_bufstart = zlseek(G.zipfd, bufstart, SEEK_SET);
#endif
            if ((G.incnt = read(G.zipfd, (char*)G.inbuf, INBUFSIZ)) <= 0) {
                Info(slide, 0x401, ((char*)slide, LoadFarString(OffsetMsg), *pfilnum, "lseek", (long)bufstart));
                error_in_archive = PK_BADERR;
                continue;
            }
            G.inptr = G.inbuf + (int)inbuf_offset;
            G.incnt -= (int)inbuf_offset;
        }
        else {
            /* Target still in current buffer: just reposition inptr/incnt. */
            G.incnt += (int)(G.inptr - G.inbuf) - (int)inbuf_offset;
            G.inptr = G.inbuf + (int)inbuf_offset;
        }

        /* Verify local header signature at current position. */
        if (readbuf(__G__ G.sig, 4) == 0) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(OffsetMsg), *pfilnum, "EOF", (long)request));
            error_in_archive = PK_BADERR;
            continue;
        }
        if (memcmp(G.sig, local_hdr_sig, 4)) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(OffsetMsg), *pfilnum, LoadFarStringSmall(LocalHdrSig), (long)request));
            error_in_archive = PK_ERR;

            /* Try one-time compensation swap between extra_bytes and old value. */
            if ((*pfilnum == 1 && G.extra_bytes != 0L) || (G.extra_bytes == 0L && *pold_extra_bytes != 0L)) {
                Info(slide, 0x401, ((char*)slide, LoadFarString(AttemptRecompensate)));
                if (G.extra_bytes) {
                    *pold_extra_bytes = G.extra_bytes;
                    G.extra_bytes = 0L;
                }
                else {
                    G.extra_bytes = *pold_extra_bytes;
                }
                if ((seek_zipf(__G__ G.pInfo->offset) != PK_OK) || (readbuf(__G__ G.sig, 4) == 0)) {
                    Info(slide, 0x401, ((char*)slide, LoadFarString(OffsetMsg), *pfilnum, "EOF", (long)request));
                    error_in_archive = PK_BADERR;
                    continue;
                }
                if (memcmp(G.sig, local_hdr_sig, 4)) {
                    Info(slide, 0x401, ((char*)slide, LoadFarString(OffsetMsg), *pfilnum, LoadFarStringSmall(LocalHdrSig), (long)request));
                    error_in_archive = PK_BADERR;
                    continue;
                }
            }
            else {
                continue;
            }
        }

        if ((error = process_local_file_hdr(__G)) != PK_COOL) {
            Info(slide, 0x421, ((char*)slide, LoadFarString(BadLocalHdr), *pfilnum));
            error_in_archive = error;
            continue;
        }

        /* Read local filename & extra (Zip64 may adjust sizes). */
        if ((error = do_string(__G__ G.lrec.filename_length, DS_FN_L)) != PK_COOL) {
            if (error > error_in_archive)
                error_in_archive = error;
            if (error > PK_WARN) {
                Info(slide, 0x401, ((char*)slide, LoadFarString(FilNamMsg), FnFilter1(G.filename), "local"));
                continue;
            }
        }
        if (G.extra_field != (uch*)NULL) {
            free(G.extra_field);
            G.extra_field = (uch*)NULL;
        }
        if ((error = do_string(__G__ G.lrec.extra_field_length, EXTRA_FIELD)) != 0) {
            if (error > error_in_archive)
                error_in_archive = error;
            if (error > PK_WARN) {
                Info(slide, 0x401, ((char*)slide, LoadFarString(ExtFieldMsg), FnFilter1(G.filename), "local"));
                continue;
            }
        }

#ifndef SFX
        /* Now that UTF-8 name extra (if any) is applied, ensure name matches central. */
        if (G.pInfo->cfilname != (char Far*)NULL) {
            if (zfstrcmp(G.pInfo->cfilname, G.filename) != 0) {
#ifdef SMALL_MEM
                char* temp_cfilnam = slide + (7 * (WSIZE >> 3));
                zfstrcpy((char Far*)temp_cfilnam, G.pInfo->cfilname);
#define cFile_PrintBuf temp_cfilnam
#else
#define cFile_PrintBuf G.pInfo->cfilname
#endif
                Info(slide, 0x401, ((char*)slide, LoadFarStringSmall2(LvsCFNamMsg), FnFilter2(cFile_PrintBuf), FnFilter1(G.filename)));
#undef cFile_PrintBuf
                zfstrcpy(G.filename, G.pInfo->cfilname);
                if (error_in_archive < PK_WARN)
                    error_in_archive = PK_WARN;
            }
            zffree(G.pInfo->cfilname);
            G.pInfo->cfilname = (char Far*)NULL;
        }
#endif /* !SFX */

        /* Stored-method size sanity (accounting for 12-byte crypto header). */
        if (G.lrec.compression_method == STORED) {
            zusz_t csiz_dec = G.lrec.csize;
            if (G.pInfo->encrypted) {
                if (csiz_dec < 12) {
                    Info(slide, 0x401, ((char*)slide, LoadFarStringSmall(ErrUnzipNoFile), LoadFarString(InvalidComprData), LoadFarStringSmall2(Inflate)));
                    return PK_ERR;
                }
                csiz_dec -= 12;
            }
            if (G.lrec.ucsize != csiz_dec) {
                Info(slide, 0x401, ((char*)slide, LoadFarStringSmall2(WrnStorUCSizCSizDiff), FnFilter1(G.filename), FmZofft(G.lrec.ucsize, NULL, "u"), FmZofft(csiz_dec, NULL, "u")));
                G.lrec.ucsize = csiz_dec;
                if (error_in_archive < PK_WARN)
                    error_in_archive = PK_WARN;
            }
        }

#if CRYPT
        if (G.pInfo->encrypted && (error = decrypt(__G__ uO.pwdarg)) != PK_COOL) {
            if (error == PK_WARN) {
                if (!((uO.tflag && uO.qflag) || (!uO.tflag && !QCOND2)))
                    Info(slide, 0x401, ((char*)slide, LoadFarString(SkipIncorrectPasswd), FnFilter1(G.filename)));
                ++(*pnum_bad_pwd);
            }
            else {
                if (error > error_in_archive)
                    error_in_archive = error;
                Info(slide, 0x401, ((char*)slide, LoadFarString(SkipCannotGetPasswd), FnFilter1(G.filename)));
            }
            continue;
        }
#endif /* CRYPT */

        /* -------------------------------------------
           Extraction to disk: overwrite & path handling
           ------------------------------------------- */
        if (!uO.tflag && !uO.cflag) {
            renamed = FALSE;
        startover:
            query = FALSE;
            skip_entry = SKIP_NO;

#ifndef SFX
            /* Convert backslashes to slashes for FAT-origin paths. */
            if (G.pInfo->hostnum == FS_FAT_ && !MBSCHR(G.filename, '/')) {
                char* p = G.filename;
                while (*p) {
                    if (*p == '\\') {
                        if (!G.reported_backslash) {
                            Info(slide, 0x21, ((char*)slide, LoadFarString(BackslashPathSep), G.zipfn));
                            G.reported_backslash = TRUE;
                            if (!error_in_archive)
                                error_in_archive = PK_WARN;
                        }
                        *p = '/';
                    }
                    ++p;
                }
            }
#endif /* !SFX */

            /* Strip leading '/' to avoid absolute path extraction. */
            if (!renamed && G.filename[0] == '/') {
                Info(slide, 0x401, ((char*)slide, LoadFarString(AbsolutePathWarning), FnFilter1(G.filename)));
                if (!error_in_archive)
                    error_in_archive = PK_WARN;
                do {
                    char* p = G.filename + 1;
                    do {
                        *(p - 1) = *p;
                    } while (*p++ != '\0');
                } while (G.filename[0] == '/');
            }

            /* mapname may create dirs (if allowed) or return status. */
            error = mapname(__G__ renamed);
            if ((errcode = (error & ~MPN_MASK)) != PK_OK && error_in_archive < errcode)
                error_in_archive = errcode;

            if ((errcode = (error & MPN_MASK)) > MPN_INF_TRUNC) {
                if (errcode == MPN_CREATED_DIR) {
#ifdef SET_DIR_ATTRIB
                    direntry* d_entry;
                    error = defer_dir_attribs(__G__ & d_entry);
                    if (d_entry == (direntry*)NULL) {
                        if (error) {
                            Info(slide, 0x401, ((char*)slide, LoadFarString(DirlistEntryNoMem)));
                            if (!error_in_archive)
                                error_in_archive = PK_WARN;
                        }
                    }
                    else {
                        d_entry->next = (*pdirlist);
                        (*pdirlist) = d_entry;
                        ++(*pnum_dirs);
                    }
#endif /* SET_DIR_ATTRIB */
                }
                else if (errcode == MPN_VOL_LABEL) {
                    Info(slide, 1, ((char*)slide, LoadFarString(SkipVolumeLabel), FnFilter1(G.filename), ""));
                }
                else if (errcode > MPN_INF_SKIP && error_in_archive < PK_ERR) {
                    error_in_archive = PK_ERR;
                }
                continue;
            }

            /* Overwrite policy / freshness checks */
            switch (check_for_newer(__G__ G.filename)) {
                case DOES_NOT_EXIST:
                    if (uO.fflag && !renamed) /* freshen only */
                        skip_entry = SKIP_Y_NONEXIST;
                    break;

                case EXISTS_AND_OLDER:
#ifdef UNIXBACKUP
                    if (!uO.B_flag)
#endif
                    {
                        if (IS_OVERWRT_NONE)
                            skip_entry = SKIP_Y_EXISTING;
                        else if (!IS_OVERWRT_ALL)
                            query = TRUE;
                    }
                    break;

                case EXISTS_AND_NEWER: /* or equal */
#ifdef UNIXBACKUP
                    if ((!uO.B_flag && IS_OVERWRT_NONE) ||
#else
                    if (IS_OVERWRT_NONE ||
#endif
                        (uO.uflag && !renamed)) {
                        skip_entry = SKIP_Y_EXISTING;
                    }
                    else {
#ifdef UNIXBACKUP
                        if (!IS_OVERWRT_ALL && !uO.B_flag)
#else
                        if (!IS_OVERWRT_ALL)
#endif
                            query = TRUE;
                    }
                    break;
            }

            if (query) {
                extent fnlen;
            reprompt:
                Info(slide, 0x81, ((char*)slide, LoadFarString(ReplaceQuery), FnFilter1(G.filename)));
                if (fgets(G.answerbuf, sizeof(G.answerbuf), stdin) == (char*)NULL) {
                    Info(slide, 1, ((char*)slide, LoadFarString(AssumeNone)));
                    *G.answerbuf = 'N';
                    if (!error_in_archive)
                        error_in_archive = PK_WARN;
                }
                switch (*G.answerbuf) {
                    case 'r':
                    case 'R':
                        do {
                            Info(slide, 0x81, ((char*)slide, LoadFarString(NewNameQuery)));
                            if (fgets(G.filename, FILNAMSIZ, stdin) == NULL)
                                G.filename[0] = '\0';
                            fnlen = strlen(G.filename);
                            if (fnlen && G.filename[fnlen - 1] == '\n')
                                G.filename[--fnlen] = '\0';
                        } while (fnlen == 0);
                        renamed = TRUE;
                        goto startover;

                    case 'A':
                        G.overwrite_mode = OVERWRT_ALWAYS; /* fallthrough */
                    case 'y':
                    case 'Y':
                        break;

                    case 'N':
                        G.overwrite_mode = OVERWRT_NEVER; /* fallthrough */
                    case 'n':
                        skip_entry = SKIP_Y_EXISTING;
                        break;

                    default: {
                        fnlen = strlen(G.answerbuf);
                        if (fnlen && G.answerbuf[fnlen - 1] == '\n')
                            G.answerbuf[--fnlen] = '\0';
                        Info(slide, 1, ((char*)slide, LoadFarString(InvalidResponse), G.answerbuf));
                        goto reprompt;
                    }
                }
            }

            if (skip_entry != SKIP_NO)
                continue;
        } /* end: to-disk path */

        G.disk_full = 0;
        if ((error = extract_or_test_member(__G)) != PK_COOL) {
            if (error > error_in_archive)
                error_in_archive = error;
            if (G.disk_full > 1)
                return error_in_archive;
        }

        /* Record consumed span for bomb detection. */
        error = cover_add((cover_t*)G.cover, request, G.cur_zipfile_bufstart + (G.inptr - G.inbuf));
        if (error < 0) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(NotEnoughMemCover)));
            return PK_MEM;
        }
        if (error != 0) {
            Info(slide, 0x401, ((char*)slide, LoadFarString(OverlappedComponents)));
            return PK_BOMB;
        }
    }

    return error_in_archive;
} /* end function extract_or_test_entrylist() */

/* wsize is used in extract_or_test_member() and UZbunzip2() */
#if (defined(DLL) && !defined(NO_SLIDE_REDIR))
#define wsize G._wsize /* wsize is a variable */
#else
#define wsize WSIZE /* wsize is a constant */
#endif

/*******************************************/
/*  Function extract_or_test_member (UNIX) */
/*******************************************/

static int extract_or_test_member(__G) /* return PK-type error code */
    __GDEF {
    const char* nul = "[empty] ";
    const char* txt = "[text]  ";
    const char* bin = "[binary]";
    int r, error = PK_COOL;

    /* initialize per-entry state */
    G.bits_left = 0;
    G.bitbuf = 0L;
    G.zipeof = 0;
    G.newfile = TRUE;
    G.crc32val = CRCVAL_INITIAL;

#ifdef SYMLINKS
    G.symlnk = (G.pInfo->symlink && !uO.tflag && !uO.cflag && (G.lrec.ucsize > 0));
#else
    G.symlnk = FALSE;
#endif

    /* announce intent and open output if needed */
    if (uO.tflag) {
        if (!uO.qflag)
            Info(slide, 0, ((char*)slide, LoadFarString(ExtractMsg), "test", FnFilter1(G.filename), "", ""));
    }
    else {
        if (uO.cflag) {
            G.outfile = stdout;
#define NEWLINE "\n"
        }
        else if (open_outfile(__G)) {
            return PK_DISK;
        }
    }

    /* prepare input stream state */
    defer_leftover_input(__G);

    switch (G.lrec.compression_method) {
        case STORED: {
            if (!uO.tflag && QCOND2) {
#ifdef SYMLINKS
                if (G.symlnk)
                    Info(slide, 0, ((char*)slide, LoadFarString(ExtractMsg), "link", FnFilter1(G.filename), "", ""));
                else
#endif
                    Info(slide, 0,
                         ((char*)slide, LoadFarString(ExtractMsg), "extract", FnFilter1(G.filename), (uO.aflag != 1) ? "" : (G.lrec.ucsize == 0L ? nul : (G.pInfo->textfile ? txt : bin)),
                          uO.cflag ? NEWLINE : ""));
            }

            /* fast bulk copy for STORED */
            G.outptr = slide; /* slide is the WSIZE scratch buffer */
            G.outcnt = 0L;

            zusz_t remaining = G.lrec.ucsize;

            while (remaining > 0) {
                if (G.incnt <= 0) {
                    if (fillinbuf(__G) == 0) {
                        error = PK_ERR;
                        break;
                    }
                }

                zusz_t take = MIN((zusz_t)G.incnt, remaining);
                zusz_t space = (zusz_t)(WSIZE - G.outcnt);

                if (space == 0) {
                    int fr = flush(__G__ slide, G.outcnt, 0);
                    if (error < fr)
                        error = fr;
                    if (error != PK_COOL || G.disk_full)
                        break;
                    G.outptr = slide;
                    G.outcnt = 0L;
                    space = (zusz_t)WSIZE;
                }

                if (take > space)
                    take = space;

                memcpy(G.outptr, G.inptr, (size_t)take);
                G.outptr += take;
                G.outcnt += take;
                G.inptr += take;
                G.incnt -= (int)take;
                remaining -= take;

                if (G.outcnt == WSIZE) {
                    int fr = flush(__G__ slide, G.outcnt, 0);
                    if (error < fr)
                        error = fr;
                    if (error != PK_COOL || G.disk_full)
                        break;
                    G.outptr = slide;
                    G.outcnt = 0L;
                }
            }

            if (G.outcnt && error == PK_COOL && !G.disk_full) {
                int fr = flush(__G__ slide, G.outcnt, 0);
                if (error < fr)
                    error = fr;
            }
            break;
        }

        case DEFLATED:
#ifdef USE_DEFLATE64
        case ENHDEFLATED:
#endif
            if (!uO.tflag && QCOND2)
                Info(slide, 0, ((char*)slide, LoadFarString(ExtractMsg), "inflat", FnFilter1(G.filename), (uO.aflag != 1) ? "" : (G.pInfo->textfile ? txt : bin), uO.cflag ? NEWLINE : ""));
#ifndef USE_ZLIB
#define UZinflate inflate
#endif
            r = UZinflate(__G__(G.lrec.compression_method == ENHDEFLATED));
            if (r != 0) {
                if (r < PK_DISK)
                    Info(slide, 0x401,
                         ((char*)slide, LoadFarStringSmall(ErrUnzipFile), r == 3 ? LoadFarString(NotEnoughMem) : LoadFarString(InvalidComprData), LoadFarStringSmall2(Inflate), FnFilter1(G.filename)));
                error = (r == 3) ? PK_MEM3 : PK_ERR;
            }
            break;

#ifdef USE_BZIP2
        case BZIPPED:
            if (!uO.tflag && QCOND2)
                Info(slide, 0, ((char*)slide, LoadFarString(ExtractMsg), "bunzipp", FnFilter1(G.filename), (uO.aflag != 1) ? "" : (G.pInfo->textfile ? txt : bin), uO.cflag ? NEWLINE : ""));
            r = UZbunzip2(__G);
            if (r != 0)
                error = (r == 3) ? PK_MEM3 : PK_ERR;
            break;
#endif

        default:
            Info(slide, 0x401, ((char*)slide, LoadFarString(FileUnknownCompMethod), FnFilter1(G.filename)));
            undefer_input(__G);
            return PK_WARN;
    }

    /* close output on UNIX paths */
    if (!uO.tflag && !uO.cflag)
        close_outfile(__G);

    /* handle disk full conditions */
    if (G.disk_full) {
        if (G.disk_full > 1) {
#if defined(HAVE_UNLINK)
            unlink(G.filename);
#else
            Info(slide, 0x421, ((char*)slide, LoadFarString(FileTruncated), FnFilter1(G.filename)));
#endif
            error = PK_DISK;
        }
        else {
            error = PK_WARN;
        }
    }

    if (error > PK_WARN) {
        undefer_input(__G);
        return error;
    }

    /* CRC verification and test-mode messaging */
    if (G.crc32val != G.lrec.crc32) {
        if ((uO.tflag && uO.qflag) || (!uO.tflag && !QCOND2))
            Info(slide, 0x401, ((char*)slide, "%-22s ", FnFilter1(G.filename)));
        Info(slide, 0x401, ((char*)slide, LoadFarString(BadCRC), G.crc32val, G.lrec.crc32));
        error = PK_ERR;
    }
    else if (uO.tflag) {
#ifndef SFX
        if (G.extra_field) {
            r = TestExtraField(__G__ G.extra_field, G.lrec.extra_field_length);
            if (r > error)
                error = r;
        }
        else
#endif
            if (!uO.qflag)
            Info(slide, 0, ((char*)slide, " OK\n"));
    }
    else {
        if (QCOND2 && !error)
            Info(slide, 0, ((char*)slide, "\n"));
    }

    undefer_input(__G);

    /* skip optional data descriptor */
    if ((G.lrec.general_purpose_bit_flag & 8) != 0) {
#define SIG 0x08074b50
        uch peek[24];
        int len = 0;
        const int need = (int)sizeof(peek);
        int got = 0;

        if (G.incnt >= need) {
            memcpy(peek, G.inptr, (size_t)need);
            got = need;
        }
        else {
            if (G.incnt > 0) {
                memcpy(peek, G.inptr, (size_t)G.incnt);
                got = G.incnt;
            }
            if (fillinbuf(__G) != 0) {
                int add = MIN(need - got, G.incnt);
                memcpy(peek + got, G.inptr, (size_t)add);
                got += add;
            }
        }

        if (got >= 24 && makelong(peek) == SIG && makelong(peek + 4) == G.lrec.crc32 && makeint64(peek + 8) == G.lrec.csize && makeint64(peek + 16) == G.lrec.ucsize)
            len = 24;
        else if (got >= 20 && makelong(peek) == G.lrec.crc32 && makeint64(peek + 4) == G.lrec.csize && makeint64(peek + 12) == G.lrec.ucsize)
            len = 20;
        else if (got >= 16 && makelong(peek) == SIG && makelong(peek + 4) == G.lrec.crc32 && makelong(peek + 8) == (ulg)G.lrec.csize && makelong(peek + 12) == (ulg)G.lrec.ucsize)
            len = 16;
        else if (got >= 12 && makelong(peek) == G.lrec.crc32 && makelong(peek + 4) == (ulg)G.lrec.csize && makelong(peek + 8) == (ulg)G.lrec.ucsize)
            len = 12;

        if (len == 0) {
            error = PK_ERR;
        }
        else {
            int adv = len;
            while (adv > 0) {
                if (G.incnt == 0) {
                    if (fillinbuf(__G) == 0)
                        break;
                }
                int step = MIN(adv, G.incnt);
                G.inptr += step;
                G.incnt -= step;
                adv -= step;
            }
        }
    }

    return error;
} /* end function extract_or_test_member */

#ifndef SFX

/*******************************/
/*  Function TestExtraField()  */
/*******************************/

static int TestExtraField(__G__ ef, ef_len)
__GDEF
uch* ef;
unsigned ef_len;
{
    /* Print the file name once if we hit any EF error. */
#define EF_ERR_PREFIX()                                                      \
    do {                                                                     \
        if (uO.qflag)                                                        \
            Info(slide, 1, ((char*)slide, "%-22s ", FnFilter1(G.filename))); \
    } while (0)

    while (ef_len >= EB_HEADSIZE) {
        const ush ebID = makeword(ef);
        const unsigned ebLen = (unsigned)makeword(ef + EB_LEN);

        /* Basic structural check: header + body must fit. */
        if (ebLen > ef_len - EB_HEADSIZE) {
            EF_ERR_PREFIX();
            Info(slide, 1, ((char*)slide, LoadFarString(InconsistEFlength), ebLen, (ef_len - EB_HEADSIZE)));
            return PK_ERR;
        }

        /* Pointer to start of EF payload. */
        uch* ep = ef + EB_HEADSIZE;

        switch (ebID) {
            /* Blocks that may contain compressed sub-blobs we can verify. */
            case EF_OS2:
            case EF_ACL:
            case EF_MAC3:
            case EF_BEOS:
            case EF_ATHEOS: {
                unsigned cmpr_offs = 0;

                switch (ebID) {
                    case EF_OS2:
                    case EF_ACL:
                        /* These have a fixed header preceding the compressed payload. */
                        cmpr_offs = EB_OS2_HLEN;
                        break;

                    case EF_MAC3:
                        /* If uncompressed flag set and length matches, payload starts immediately. */
                        if (ebLen >= EB_MAC3_HLEN && (makeword(ef + (EB_HEADSIZE + EB_FLGS_OFFS)) & EB_M3_FL_UNCMPR) && (makelong(ep) == ebLen - EB_MAC3_HLEN))
                            cmpr_offs = 0;
                        else
                            cmpr_offs = EB_MAC3_HLEN;
                        break;

                    case EF_BEOS:
                    case EF_ATHEOS:
                        if (ebLen >= EB_BEOS_HLEN && (*(ef + (EB_HEADSIZE + EB_FLGS_OFFS)) & EB_BE_FL_UNCMPR) && (makelong(ep) == ebLen - EB_BEOS_HLEN))
                            cmpr_offs = 0;
                        else
                            cmpr_offs = EB_BEOS_HLEN;
                        break;
                }

                /* Validate the (possibly) compressed EA block. */
                {
                    const int r = test_compr_eb(__G__ ef, ebLen, cmpr_offs, NULL);
                    if (r != PK_OK) {
                        EF_ERR_PREFIX();
                        switch (r) {
                            case IZ_EF_TRUNC:
                                Info(slide, 1, ((char*)slide, LoadFarString(TruncEAs), ebLen - (cmpr_offs + EB_CMPRHEADLEN), "\n"));
                                break;
                            case PK_ERR:
                                Info(slide, 1, ((char*)slide, LoadFarString(InvalidComprDataEAs)));
                                break;
                            case PK_MEM3:
                            case PK_MEM4:
                                Info(slide, 1, ((char*)slide, LoadFarString(NotEnoughMemEAs)));
                                break;
                            default:
                                if ((r & 0xff) != PK_ERR) {
                                    Info(slide, 1, ((char*)slide, LoadFarString(UnknErrorEAs)));
                                }
                                else {
                                    const ush m = (ush)(r >> 8);
                                    if (m == DEFLATED) /* historical KLUDGE */
                                        Info(slide, 1, ((char*)slide, LoadFarString(BadCRC_EAs)));
                                    else
                                        Info(slide, 1, ((char*)slide, LoadFarString(UnknComprMethodEAs), m));
                                }
                                break;
                        }
                        return r;
                    }
                }
                break;
            }

            case EF_NTSD: {
                /* Check minimal size and supported version, then test payload. */
                int r;
                Trace((stderr, "ebID: %i / ebLen: %u\n", ebID, ebLen));

                if (ebLen < EB_NTSD_L_LEN) {
                    r = IZ_EF_TRUNC;
                }
                else if (ef[EB_HEADSIZE + EB_NTSD_VERSION] > EB_NTSD_MAX_VER) {
                    r = (PK_WARN | 0x4000); /* mark as “unsupported version” */
                }
                else {
                    r = test_compr_eb(__G__ ef, ebLen, EB_NTSD_L_LEN, TEST_NTSD);
                }

                if (r != PK_OK) {
                    EF_ERR_PREFIX();
                    switch (r) {
                        case IZ_EF_TRUNC:
                            Info(slide, 1, ((char*)slide, LoadFarString(TruncNTSD), ebLen - (EB_NTSD_L_LEN + EB_CMPRHEADLEN), "\n"));
                            break;
                        case PK_ERR:
                            Info(slide, 1, ((char*)slide, LoadFarString(InvalidComprDataEAs)));
                            break;
                        case PK_MEM3:
                        case PK_MEM4:
                            Info(slide, 1, ((char*)slide, LoadFarString(NotEnoughMemEAs)));
                            break;
                        case (PK_WARN | 0x4000):
                            Info(slide, 1, ((char*)slide, LoadFarString(UnsuppNTSDVersEAs), (int)ef[EB_HEADSIZE + EB_NTSD_VERSION]));
                            r = PK_WARN; /* normalize */
                            break;
                        default:
                            if ((r & 0xff) != PK_ERR) {
                                Info(slide, 1, ((char*)slide, LoadFarString(UnknErrorEAs)));
                            }
                            else {
                                const ush m = (ush)(r >> 8);
                                if (m == DEFLATED)
                                    Info(slide, 1, ((char*)slide, LoadFarString(BadCRC_EAs)));
                                else
                                    Info(slide, 1, ((char*)slide, LoadFarString(UnknComprMethodEAs), m));
                            }
                            break;
                    }
                    return r;
                }
                break;
            }

            case EF_PKVMS:
                if (ebLen < 4) {
                    Info(slide, 1, ((char*)slide, LoadFarString(TooSmallEBlength), ebLen, 4));
                }
                else {
                    const ulg stored_crc = makelong(ep);
                    const extent datalen = (extent)(ebLen - 4);
                    const ulg calc_crc = crc32(CRCVAL_INITIAL, ep + 4, datalen);
                    if (stored_crc != calc_crc)
                        Info(slide, 1, ((char*)slide, LoadFarString(BadCRC_EAs)));
                }
                break;

            /* Known-but-unvalidated blocks: silently skip. */
            case EF_PKW32:
            case EF_PKUNIX:
            case EF_ASIUNIX:
            case EF_IZVMS:
            case EF_IZUNIX:
            case EF_VMCMS:
            case EF_MVS:
            case EF_SPARK:
            case EF_TANDEM:
            case EF_THEOS:
            case EF_AV:
            default:
                break;
        }

        /* Advance to next EF block. */
        ef += EB_HEADSIZE + ebLen;
        ef_len -= EB_HEADSIZE + ebLen;
    }

    if (!uO.qflag)
        Info(slide, 0, ((char*)slide, " OK\n"));

    return PK_COOL;

#undef EF_ERR_PREFIX
} /* end function TestExtraField() */

/******************************/
/*  Function test_compr_eb()  */
/******************************/

#ifdef PROTO
static int test_compr_eb(__GPRO__ uch* eb, unsigned eb_size, unsigned compr_offset, int (*test_uc_ebdata)(__GPRO__ uch* eb, unsigned eb_size, uch* eb_ucptr, ulg eb_ucsize))
#else  /* !PROTO */
static int test_compr_eb(__G__ eb, eb_size, compr_offset, test_uc_ebdata)
__GDEF
uch* eb;
unsigned eb_size;
unsigned compr_offset;
int (*test_uc_ebdata)();
#endif /* ?PROTO */
{
    ulg eb_ucsize;
    uch* eb_ucptr;
    int r;
    ush eb_compr_method;

    if (compr_offset < 4) /* field is not compressed: */
        return PK_OK;     /* do nothing and signal OK */

    /* Return no/bad-data error status if any problem is found:
     *    1. eb_size is too small to hold the uncompressed size
     *       (eb_ucsize).  (Else extract eb_ucsize.)
     *    2. eb_ucsize is zero (invalid).  2014-12-04 SMS.
     *    3. eb_ucsize is positive, but eb_size is too small to hold
     *       the compressed data header.
     */
    if ((eb_size < (EB_UCSIZE_P + 4)) || ((eb_ucsize = makelong(eb + (EB_HEADSIZE + EB_UCSIZE_P))) == 0L) || ((eb_ucsize > 0L) && (eb_size <= (compr_offset + EB_CMPRHEADLEN))))
        return IZ_EF_TRUNC; /* no/bad compressed data! */

    /* 2015-02-10 Mancha(?), Michal Zalewski, Tomas Hoger, SMS.
     * For STORE method, compressed and uncompressed sizes must agree.
     * http://www.info-zip.org/phpBB3/viewtopic.php?f=7&t=450
     */
    eb_compr_method = makeword(eb + (EB_HEADSIZE + compr_offset));
    if ((eb_compr_method == STORED) && (eb_size != compr_offset + EB_CMPRHEADLEN + eb_ucsize))
        return PK_ERR;

    if (
#ifdef INT_16BIT
        (((ulg)(extent)eb_ucsize) != eb_ucsize) ||
#endif
        (eb_ucptr = (uch*)malloc((extent)eb_ucsize)) == (uch*)NULL)
        return PK_MEM4;

    r = memextract(__G__ eb_ucptr, eb_ucsize, eb + (EB_HEADSIZE + compr_offset), (ulg)(eb_size - compr_offset));

    if (r == PK_OK && test_uc_ebdata != NULL)
        r = (*test_uc_ebdata)(__G__ eb, eb_size, eb_ucptr, eb_ucsize);

    free(eb_ucptr);
    return r;

} /* end function test_compr_eb() */

#endif /* !SFX */

/***************************/
/*  Function memextract()  */
/***************************/

int memextract(__G__ tgt, tgtsize, src, srcsize) /* extract compressed */
__GDEF                                           /*  extra field block; */
    uch* tgt;                                    /*  return PK-type error */
ulg tgtsize;                                     /*  level */
ZCONST uch* src;
ulg srcsize;
{
    zoff_t old_csize = G.csize;
    uch* old_inptr = G.inptr;
    int old_incnt = G.incnt;
    int r, error = PK_OK;
    ush method;
    ulg extra_field_crc;

    method = makeword(src);
    extra_field_crc = makelong(src + 2);

    /* compressed extra field exists completely in memory at this location: */
    G.inptr = (uch*)src + (2 + 4); /* method and extra_field_crc */
    G.incnt = (int)(G.csize = (long)(srcsize - (2 + 4)));
    G.mem_mode = TRUE;
    G.outbufptr = tgt;
    G.outsize = tgtsize;

    switch (method) {
        case STORED:
            memcpy((char*)tgt, (char*)G.inptr, (extent)G.incnt);
            G.outcnt = (ulg)G.csize; /* for CRC calculation */
            break;
        case DEFLATED:
#ifdef USE_DEFLATE64
        case ENHDEFLATED:
#endif
            G.outcnt = 0L;
            if ((r = UZinflate(__G__(method == ENHDEFLATED))) != 0) {
                if (!uO.tflag)
                    Info(slide, 0x401, ((char*)slide, LoadFarStringSmall(ErrUnzipNoFile), r == 3 ? LoadFarString(NotEnoughMem) : LoadFarString(InvalidComprData), LoadFarStringSmall2(Inflate)));
                error = (r == 3) ? PK_MEM3 : PK_ERR;
            }
            if (G.outcnt == 0L) /* inflate's final FLUSH sets outcnt */
                break;
            break;
        default:
            if (uO.tflag)
                error = PK_ERR | ((int)method << 8);
            else {
                Info(slide, 0x401, ((char*)slide, LoadFarString(UnsupportedExtraField), method));
                error = PK_ERR; /* GRR:  should be passed on up via SetEAs() */
            }
            break;
    }

    G.inptr = old_inptr;
    G.incnt = old_incnt;
    G.csize = old_csize;
    G.mem_mode = FALSE;

    if (!error) {
        register ulg crcval = crc32(CRCVAL_INITIAL, tgt, (extent)G.outcnt);

        if (crcval != extra_field_crc) {
            if (uO.tflag)
                error = PK_ERR | (DEFLATED << 8); /* kludge for now */
            else {
                Info(slide, 0x401, ((char*)slide, LoadFarString(BadExtraFieldCRC), G.zipfn, crcval, extra_field_crc));
                error = PK_ERR;
            }
        }
    }
    return error;

} /* end function memextract() */

/*************************/
/*  Function memflush()  */
/*************************/

int memflush(__G__ rawbuf, size)
__GDEF
ZCONST uch* rawbuf;
ulg size;
{
    if (size > G.outsize)
        /* Here, PK_DISK is a bit off-topic, but in the sense of marking
           "overflow of output space", its use may be tolerated. */
        return PK_DISK; /* more data than output buffer can hold */

    memcpy((char*)G.outbufptr, (char*)rawbuf, (extent)size);
    G.outbufptr += (unsigned int)size;
    G.outsize -= size;
    G.outcnt += size;

    return 0;

} /* end function memflush() */

#ifdef SYMLINKS
/***********************************/
/* Function set_deferred_symlink() */
/***********************************/

static void set_deferred_symlink(__G__ slnk_entry) __GDEF slinkentry* slnk_entry;
{
    extent ucsize = slnk_entry->targetlen;
    char* linkfname = slnk_entry->fname;
    char* linktarget = (char*)malloc(ucsize + 1);

    if (!linktarget) {
        Info(slide, 0x201, ((char*)slide, LoadFarString(SymLnkWarnNoMem), FnFilter1(linkfname)));
        return;
    }
    linktarget[ucsize] = '\0';
    G.outfile = zfopen(linkfname, FOPR); /* open link placeholder for reading */
    /* Check that the following conditions are all fulfilled:
     * a) the placeholder file exists,
     * b) the placeholder file contains exactly "ucsize" bytes
     *    (read the expected placeholder content length + 1 extra byte, this
     *    should return the expected content length),
     * c) the placeholder content matches the link target specification as
     *    stored in the symlink control structure.
     */
    if (!G.outfile || fread(linktarget, 1, ucsize + 1, G.outfile) != ucsize || strcmp(slnk_entry->target, linktarget)) {
        Info(slide, 0x201, ((char*)slide, LoadFarString(SymLnkWarnInvalid), FnFilter1(linkfname)));
        free(linktarget);
        if (G.outfile)
            fclose(G.outfile);
        return;
    }
    fclose(G.outfile); /* close "data" file for good... */
    unlink(linkfname); /* ...and delete it */
    if (QCOND2)
        Info(slide, 0, ((char*)slide, LoadFarString(SymLnkFinish), FnFilter1(linkfname), FnFilter2(linktarget)));
    if (symlink(linktarget, linkfname)) /* create the real link */
        perror("symlink error");
    free(linktarget);
#ifdef SET_SYMLINK_ATTRIBS
    set_symlnk_attribs(__G__ slnk_entry);
#endif
    return; /* can't set time on symlinks */

} /* end function set_deferred_symlink() */
#endif /* SYMLINKS */

/*************************/
/*  Function fnfilter()  */ /* here instead of in list.c for SFX */
/*************************/

char* fnfilter(raw, space, size) /* convert name to safely printable form */
ZCONST char* raw;
uch* space;
extent size;
{
#ifndef NATIVE /* ASCII:  filter ANSI escape codes, etc. */
    ZCONST uch* r = (ZCONST uch*)raw;
    uch* s = space;
    uch* slim = NULL;
    uch* se = NULL;
    int have_overflow = FALSE;

    if (size > 0) {
        slim = space + size
#ifdef _MBCS
               - (MB_CUR_MAX - 1)
#endif
               - 4;
    }
    while (*r) {
        if (size > 0 && s >= slim && se == NULL) {
            se = s;
        }
#ifdef HAVE_WORKING_ISPRINT
#ifndef UZ_FNFILTER_REPLACECHAR
        /* A convenient choice for the replacement of unprintable char codes is
         * the "single char wildcard", as this character is quite unlikely to
         * appear in filenames by itself.  The following default definition
         * sets the replacement char to a question mark as the most common
         * "single char wildcard"; this setting should be overridden in the
         * appropiate system-specific configuration header when needed.
         */
#define UZ_FNFILTER_REPLACECHAR '?'
#endif
        if (!isprint(*r)) {
            if (*r < 32) {
                /* ASCII control codes are escaped as "^{letter}". */
                if (se != NULL && (s > (space + (size - 4)))) {
                    have_overflow = TRUE;
                    break;
                }
                *s++ = '^', *s++ = (uch)(64 + *r++);
            }
            else {
                /* Other unprintable codes are replaced by the
                 * placeholder character. */
                if (se != NULL && (s > (space + (size - 3)))) {
                    have_overflow = TRUE;
                    break;
                }
                *s++ = UZ_FNFILTER_REPLACECHAR;
                INCSTR(r);
            }
#else  /* !HAVE_WORKING_ISPRINT */
        if (*r < 32) {
            /* ASCII control codes are escaped as "^{letter}". */
            if (se != NULL && (s > (space + (size - 4)))) {
                have_overflow = TRUE;
                break;
            }
            *s++ = '^', *s++ = (uch)(64 + *r++);
#endif /* ?HAVE_WORKING_ISPRINT */
        }
        else {
#ifdef _MBCS
            unsigned i = CLEN(r);
            if (se != NULL && (s > (space + (size - i - 2)))) {
                have_overflow = TRUE;
                break;
            }
            for (; i > 0; i--)
                *s++ = *r++;
#else
            if (se != NULL && (s > (space + (size - 3)))) {
                have_overflow = TRUE;
                break;
            }
            *s++ = *r++;
#endif
        }
    }
    if (have_overflow) {
        strcpy((char*)se, "...");
    }
    else {
        *s = '\0';
    }

    return (char*)space;

#else /* NATIVE:  EBCDIC or whatever */
    return (char*)raw;
#endif

} /* end function fnfilter() */

#ifdef SET_DIR_ATTRIB
/* must sort saved directories so can set perms from bottom up */

/************************/
/*  Function dircomp()  */
/************************/

static int Cdecl dircomp(a, b) /* used by qsort(); swiped from Zip */
ZCONST zvoid *a, *b;
{
    /* order is significant:  this sorts in reverse order (deepest first) */
    return strcmp((*(direntry**)b)->fn, (*(direntry**)a)->fn);
    /* return namecmp((*(direntry **)b)->fn, (*(direntry **)a)->fn); */
}

#endif /* SET_DIR_ATTRIB */

#ifdef USE_BZIP2

/**************************/
/*  Function UZbunzip2()  */
/**************************/

int UZbunzip2(__G) __GDEF
/* decompress a bzipped entry using the libbz2 routines */
{
    int retval = 0; /* return code: 0 = "no error" */
    int err = BZ_OK;
    bz_stream bstrm;

    if (G.incnt <= 0 && G.csize <= 0L) {
        /* avoid an infinite loop */
        Trace((stderr, "UZbunzip2() got empty input\n"));
        return 2;
    }

#if (defined(DLL) && !defined(NO_SLIDE_REDIR))
    if (G.redirect_slide)
        wsize = G.redirect_size, redirSlide = G.redirect_buffer;
    else
        wsize = WSIZE, redirSlide = slide;
#endif

    bstrm.next_out = (char*)redirSlide;
    bstrm.avail_out = wsize;

    bstrm.next_in = (char*)G.inptr;
    bstrm.avail_in = G.incnt;

    {
        /* local buffer for efficiency */
        /* $TODO Check for BZIP LIB version? */

        bstrm.bzalloc = NULL;
        bstrm.bzfree = NULL;
        bstrm.opaque = NULL;

        Trace((stderr, "initializing bzlib()\n"));
        err = BZ2_bzDecompressInit(&bstrm, 0, 0);

        if (err == BZ_MEM_ERROR)
            return 3;
        else if (err != BZ_OK) {
            Trace((stderr, "oops!  (BZ2_bzDecompressInit() err = %d)\n", err));
        }
    }

#ifdef FUNZIP
    while (err != BZ_STREAM_END) {
#else  /* !FUNZIP */
    while (G.csize > 0) {
        Trace((stderr, "first loop:  G.csize = %ld\n", G.csize));
#endif /* ?FUNZIP */
        while (bstrm.avail_out > 0) {
            err = BZ2_bzDecompress(&bstrm);

            if (err == BZ_DATA_ERROR) {
                retval = 2;
                goto uzbunzip_cleanup_exit;
            }
            else if (err == BZ_MEM_ERROR) {
                retval = 3;
                goto uzbunzip_cleanup_exit;
            }
            else if (err != BZ_OK && err != BZ_STREAM_END) {
                Trace((stderr, "oops!  (bzip(first loop) err = %d)\n", err));
            }

#ifdef FUNZIP
            if (err == BZ_STREAM_END) /* "END-of-entry-condition" ? */
#else                                 /* !FUNZIP */
            if (G.csize <= 0L) /* "END-of-entry-condition" ? */
#endif                                /* ?FUNZIP */
                break;

            if (bstrm.avail_in == 0) {
                if (fillinbuf(__G) == 0) {
                    /* no "END-condition" yet, but no more data */
                    retval = 2;
                    goto uzbunzip_cleanup_exit;
                }

                bstrm.next_in = (char*)G.inptr;
                bstrm.avail_in = G.incnt;
            }
            Trace((stderr, "     avail_in = %u\n", bstrm.avail_in));
        }
        /* flush slide[] */
        if ((retval = FLUSH(wsize - bstrm.avail_out)) != 0)
            goto uzbunzip_cleanup_exit;
        Trace((stderr, "inside loop:  flushing %ld bytes (ptr diff = %ld)\n", (long)(wsize - bstrm.avail_out), (long)(bstrm.next_out - (char*)redirSlide)));
        bstrm.next_out = (char*)redirSlide;
        bstrm.avail_out = wsize;
    }

    /* no more input, so loop until we have all output */
    Trace((stderr, "beginning final loop:  err = %d\n", err));
    while (err != BZ_STREAM_END) {
        err = BZ2_bzDecompress(&bstrm);
        if (err == BZ_DATA_ERROR) {
            retval = 2;
            goto uzbunzip_cleanup_exit;
        }
        else if (err == BZ_MEM_ERROR) {
            retval = 3;
            goto uzbunzip_cleanup_exit;
        }
        else if (err != BZ_OK && err != BZ_STREAM_END) {
            Trace((stderr, "oops!  (bzip(final loop) err = %d)\n", err));
            DESTROYGLOBALS();
            EXIT(PK_MEM3);
        }
        /* final flush of slide[] */
        if ((retval = FLUSH(wsize - bstrm.avail_out)) != 0)
            goto uzbunzip_cleanup_exit;
        Trace((stderr, "final loop:  flushing %ld bytes (ptr diff = %ld)\n", (long)(wsize - bstrm.avail_out), (long)(bstrm.next_out - (char*)redirSlide)));
        bstrm.next_out = (char*)redirSlide;
        bstrm.avail_out = wsize;
    }
#ifdef LARGE_FILE_SUPPORT
    Trace((stderr, "total in = %llu, total out = %llu\n", (zusz_t)(bstrm.total_in_lo32) + ((zusz_t)(bstrm.total_in_hi32)) << 32,
           (zusz_t)(bstrm.total_out_lo32) + ((zusz_t)(bstrm.total_out_hi32)) << 32));
#else
    Trace((stderr, "total in = %lu, total out = %lu\n", bstrm.total_in_lo32, bstrm.total_out_lo32));
#endif

    G.inptr = (uch*)bstrm.next_in;
    G.incnt -= G.inptr - G.inbuf; /* reset for other routines */

uzbunzip_cleanup_exit:
    err = BZ2_bzDecompressEnd(&bstrm);
    if (err != BZ_OK) {
        Trace((stderr, "oops!  (BZ2_bzDecompressEnd() err = %d)\n", err));
    }

    return retval;
} /* end function UZbunzip2() */
#endif /* USE_BZIP2 */
