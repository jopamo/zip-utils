/*
  Copyright (c) 1990-2009 Info-ZIP.  All rights reserved.

  See the accompanying file LICENSE, version 2009-Jan-02 or later
  (the contents of which are also included in unzip.h) for terms of use.
  If, for some reason, all these files are missing, the Info-ZIP license
  also may be found at:  ftp://ftp.info-zip.org/pub/infozip/license.html
*/
/*---------------------------------------------------------------------------
    Unix specific configuration section:
  ---------------------------------------------------------------------------*/

#ifndef __unxcfg_h
#define __unxcfg_h

#include "platform.h"

/* Automatically set ZIP64_SUPPORT if LFS */
#ifdef LARGE_FILE_SUPPORT
#if (!defined(NO_ZIP64_SUPPORT) && !defined(ZIP64_SUPPORT))
#define ZIP64_SUPPORT
#endif
#endif

/* NO_ZIP64_SUPPORT takes precedence over ZIP64_SUPPORT */
#if defined(NO_ZIP64_SUPPORT) && defined(ZIP64_SUPPORT)
#undef ZIP64_SUPPORT
#endif

#include <unistd.h>

#include <fcntl.h> /* O_BINARY for open() w/o CR/LF translation */

#ifndef NO_PARAM_H
#ifdef NGROUPS_MAX
#undef NGROUPS_MAX /* SCO bug:  defined again in <sys/param.h> */
#endif
#ifdef BSD
#define TEMP_BSD /* may be defined again in <sys/param.h> */
#undef BSD
#endif
#include <sys/param.h> /* conflict with <sys/types.h>, some systems? */
#ifdef TEMP_BSD
#undef TEMP_BSD
#ifndef BSD
#define BSD
#endif
#endif
#endif /* !NO_PARAM_H */

#ifdef BSD
#include <sys/time.h>
#include <sys/timeb.h>
#if (defined(_AIX) || defined(__GLIBC__) || defined(__GNU__))
#include <time.h>
#endif
#else
#include <time.h>
#endif

#if (defined(BSD4_4) || (defined(SYSV) && defined(MODERN)))
#include <unistd.h> /* this includes utime.h on SGIs */
#include <utime.h>
#define GOT_UTIMBUF
#if (!defined(GOT_UTIMBUF) && defined(__GNU__))
#include <utime.h>
#define GOT_UTIMBUF
#endif
#endif

#if (defined(V7) || defined(pyr_bsd))
#define strchr index
#define strrchr rindex
#endif
#ifdef V7
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#endif

#if defined(NO_UNICODE_SUPPORT) && defined(UNICODE_SUPPORT)
/* disable Unicode (UTF-8) support when requested */
#undef UNICODE_SUPPORT
#endif

#if (defined(_MBCS) && defined(NO_MBCS))
/* disable MBCS support when requested */
#undef _MBCS
#endif

#if (!defined(NO_SETLOCALE) && !defined(_MBCS))
#if (!defined(UNICODE_SUPPORT) || !defined(UTF8_MAYBE_NATIVE))
/* enable setlocale here, unless this happens later for UTF-8 and/or
 * MBCS support */
#include <locale.h>
#ifndef SETLOCALE
#define SETLOCALE(category, locale) setlocale(category, locale)
#endif
#endif
#endif
#ifndef NO_SETLOCALE
#if (!defined(NO_WORKING_ISPRINT) && !defined(HAVE_WORKING_ISPRINT))
/* enable "enhanced" unprintable chars detection in fnfilter() */
#define HAVE_WORKING_ISPRINT
#endif
#endif

#ifdef MINIX
#include <stdio.h>
#endif
#if (!defined(HAVE_STRNICMP) & !defined(NO_STRNICMP))
#define NO_STRNICMP
#endif
#ifndef DATE_FORMAT
#define DATE_FORMAT DF_MDY /* GRR:  customize with locale.h somehow? */
#endif
#define lenEOL 1
#ifdef EBCDIC
#define PutNativeEOL *q++ = '\n';
#else
#define PutNativeEOL *q++ = native(LF);
#endif
#define SCREENSIZE(ttrows, ttcols) screensize(ttrows, ttcols)
#define SCREENWIDTH 80
#define SCREENLWRAP 1
#define USE_EF_UT_TIME
#if (!defined(NO_LCHOWN) || !defined(NO_LCHMOD))
#define SET_SYMLINK_ATTRIBS
#endif
#define SET_DIR_ATTRIB
#if (!defined(NOTIMESTAMP) && !defined(TIMESTAMP)) /* GRR 970513 */
#define TIMESTAMP
#endif
#define RESTORE_UIDGID

/* Static variables that we have to add to Uz_Globs: */
#define SYSTEM_SPECIFIC_GLOBALS                          \
    int created_dir, renamed_fullpath;                   \
    char *rootpath, *buildpath, *end;                    \
    ZCONST char* wildname;                               \
    char *dirname, matchname[FILNAMSIZ];                 \
    int rootlen, have_dirname, dirnamelen, notfirstcall; \
    zvoid* wild_dir;

/* created_dir, and renamed_fullpath are used by both mapname() and    */
/*    checkdir().                                                      */
/* rootlen, rootpath, buildpath and end are used by checkdir().        */
/* wild_dir, dirname, wildname, matchname[], dirnamelen, have_dirname, */
/*    and notfirstcall are used by do_wild().                          */

#endif /* !__unxcfg_h */
