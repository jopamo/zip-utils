/*
  unix/osdep.h - Zip 3

  Copyright (c) 1990-2005 Info-ZIP.  All rights reserved.

  See the accompanying file LICENSE, version 2005-Feb-10 or later
  (the contents of which are also included in zip.h) for terms of use.
  If, for some reason, both of these files are missing, the Info-ZIP license
  also may be found at:  ftp://ftp.info-zip.org/pub/infozip/license.html
*/

#include "platform.h"

/* printf format size prefix for zoff_t values */
#ifdef LARGE_FILE_SUPPORT
#define ZOFF_T_FORMAT_SIZE_PREFIX "ll"
#else
#define ZOFF_T_FORMAT_SIZE_PREFIX "l"
#endif

/* Automatically set ZIP64_SUPPORT if LFS */

#ifdef LARGE_FILE_SUPPORT
#ifndef NO_ZIP64_SUPPORT
#ifndef ZIP64_SUPPORT
#define ZIP64_SUPPORT
#endif
#else
#ifdef ZIP64_SUPPORT
#undef ZIP64_SUPPORT
#endif
#endif
#endif

/* Process files in binary mode */

/* Enable the "UT" extra field (time info) */
#if !defined(NO_EF_UT_TIME) && !defined(USE_EF_UT_TIME)
#define USE_EF_UT_TIME
#endif
