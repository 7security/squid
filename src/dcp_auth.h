/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef _SQUID_DCP_AUTH_H
#define _SQUID_DCP_AUTH_H

#include "hash.h"
#include "SquidString.h"
#define DCP_BUILD "2214"
#define DCP_PATCH "115"
#define DCP_PLATFORM "win"
#define DCP_AUTH_VALUE "ac4500dd3b7579186c1b0620614fdb1f7d61f944"

void dcp_auth_calculate(String &);

#endif /* _SQUID_DCP_AUTH_H */
