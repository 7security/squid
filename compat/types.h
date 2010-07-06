#ifndef SQUID_CONFIG_H
#include "config.h"
#endif

/*
 * * * * * * * * Legal stuff * * * * * * *
 *
 * (C) 2000 Francesco Chemolli <kinkie@kame.usr.dsi.unimi.it>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 * * * * * * * * Declaration of intents * * * * * * *
 *
 * Here are defined several known-width types, obtained via autoconf
 * from system locations or various attempts. This is just a convenience
 * header to include which takes care of proper preprocessor stuff
 *
 * This file is only intended to be included via compat/compat.h, do
 * not include directly.
 */

#ifndef SQUID_TYPES_H
#define SQUID_TYPES_H

/* This should be in synch with what we have in acinclude.m4 */
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#if HAVE_LINUX_TYPES_H
#include <linux/types.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_STDDEF_H
#include <stddef.h>
#endif
#if HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#if HAVE_SYS_BITYPES_H
#include <sys/bitypes.h>
#endif
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#if HAVE_NETINET_IN_SYSTM_H
/* Several OS require types declared by in_systm.h without including it themselves. */
#include <netinet/in_systm.h>
#endif


/******************************************************/
/* Typedefs for missing entries on a system           */
/******************************************************/


/*
 * ISO C99 Standard printf() macros for 64 bit integers
 * On some 64 bit platform, HP Tru64 is one, for printf must be used
 * "%lx" instead of "%llx"
 */
#ifndef PRId64
#ifdef _SQUID_MSWIN_		/* Windows native port using MSVCRT */
#define PRId64 "I64d"
#elif SIZEOF_INT64_T > SIZEOF_LONG
#define PRId64 "lld"
#else
#define PRId64 "ld"
#endif
#endif

#ifndef PRIu64
#ifdef _SQUID_MSWIN_		/* Windows native port using MSVCRT */
#define PRIu64 "I64u"
#elif SIZEOF_INT64_T > SIZEOF_LONG
#define PRIu64 "llu"
#else
#define PRIu64 "lu"
#endif
#endif

/* int64_t */
#ifndef HAVE_INT64_T
#if HAVE___INT64
typedef __int64 int64_t;
#elif HAVE_LONG && SIZEOF_LONG == 8
typedef long int64_t;
#elif HAVE_LONG_LONG && SIZEOF_LONG_LONG == 8
typedef long long int64_t;
#else
#error NO 64 bit signed type available
#endif
#endif

/* uint64_t */
#if !HAVE_UINT64_T
#if HAVE_U_INT64_T
typedef u_int64_t uint64_t;
#define HAVE_UINT64_T 1
#elif HAVE_INT64_T
typedef unsigned int64_t uint64_t;
#define HAVE_UINT64_T 1
#endif /* HAVE_U_INT64_T */
#endif /* HAVE_UINT64_T */

/* int32_t */
#ifndef HAVE_INT32_T
#if HAVE_INT && SIZEOF_INT == 4
typedef int int32_t;
#elif HAVE_LONG && SIZEOF_LONG == 4
typedef long int32_t;
#else
#error NO 32 bit signed type available
#endif
#endif

/* uint32_t */
#if !HAVE_UINT32_T
#if HAVE_U_INT32_T
typedef u_int32_t uint32_t;
#define HAVE_UINT32_T 1
#elif HAVE_INT32_T
typedef unsigned int32_t uint32_t;
#define HAVE_UINT32_T 1
#endif /* HAVE_U_INT32_T */
#endif /* HAVE_UINT32_T */

/* int16_t */
#ifndef HAVE_INT16_T
#if HAVE_SHORT && SIZEOF_SHORT == 2
typedef short int16_t;
#elif HAVE_INT && SIZEOF_INT == 2
typedef int int16_t;
#else
#error NO 16 bit signed type available
#endif
#endif

/* uint16_t */
#if !HAVE_UINT16_T
#if HAVE_U_INT16_T
typedef u_int16_t uint16_t;
#define HAVE_UINT16_T 1
#elif HAVE_INT16_T
typedef unsigned int16_t uint16_t;
#define HAVE_UINT16_T 1
#endif /* HAVE_U_INT16_T */
#endif /* HAVE_UINT16_T */

/* int8_t */
#ifndef HAVE_INT8_T
#if HAVE_CHAR && SIZEOF_CHAR == 1
typedef char int8_t;
#else
#error NO 8 bit signed type available
#endif
#endif

/* uint8_t */
#if !HAVE_UINT8_T
#if HAVE_U_INT8_T
typedef u_int8_t uint8_t;
#define HAVE_UINT8_T 1
#elif HAVE_INT8_T
typedef unsigned int8_t uint8_t;
#define HAVE_UINT8_T 1
#endif /* HAVE_U_INT8_T */
#endif /* HAVE_UINT8_T */

#ifndef HAVE_PID_T
#if defined(_MSC_VER) /* Microsoft C Compiler ONLY */
typedef long pid_t;
#else
typedef int pid_t;
#endif
#endif

#ifndef HAVE_SIZE_T
typedef unsigned int size_t;
#endif

#ifndef HAVE_SSIZE_T
typedef int ssize_t;
#endif

#ifndef HAVE_OFF_T
#if defined(_MSC_VER) /* Microsoft C Compiler ONLY */
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
typedef int64_t off_t;
#else
typedef long off_t;
#endif
#else
typedef int off_t;
#endif
#endif

#ifndef HAVE_MODE_T
typedef unsigned short mode_t;
#endif

#ifndef HAVE_FD_MASK
typedef unsigned long fd_mask;
#endif

#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

#ifndef HAVE_MTYP_T
typedef long mtyp_t;
#endif

#endif /* SQUID_TYPES_H */
