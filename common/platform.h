/*
  common/platform.h

  Shared POSIX/LFS plumbing used by both zip and unzip.
*/
#ifndef __common_platform_h
#define __common_platform_h

/* Normalize LARGE_FILE_SUPPORT toggles before pulling in system headers */
#if defined(NO_LARGE_FILE_SUPPORT) && defined(LARGE_FILE_SUPPORT)
#undef LARGE_FILE_SUPPORT
#endif

#ifdef LARGE_FILE_SUPPORT
/* Large File Summit switches: must be defined before <sys/types.h> */
#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _LARGE_FILES
#define _LARGE_FILES
#endif
#ifndef __USE_LARGEFILE64
#define __USE_LARGEFILE64
#endif
#endif /* LARGE_FILE_SUPPORT */

#include <sys/types.h>
#include <sys/stat.h>

/* Common file offset typedefs */
#ifndef ZOFF_T_DEFINED
#ifdef NO_OFF_T
typedef long zoff_t;
typedef unsigned long uzoff_t;
#else
typedef off_t zoff_t;
#if defined(LARGE_FILE_SUPPORT) && !(defined(__alpha) && defined(__osf__))
typedef unsigned long long uzoff_t;
#else
typedef unsigned long uzoff_t;
#endif
#endif
#define ZOFF_T_DEFINED
#endif /* ZOFF_T_DEFINED */

#ifndef Z_STAT_DEFINED
typedef struct stat z_stat;
#define Z_STAT_DEFINED
#endif

#endif /* __common_platform_h */
