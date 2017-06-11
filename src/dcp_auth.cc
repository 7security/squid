/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "md5.h"
#include "rfc2617.h"
#include "dcp_auth.h"

// Chrome-Proxy: ps=1439961190-0-0-0, sid=9fb96126616582c4be88ab7fe26ef593, b=2214, p=115, c=win

void dcp_auth_calculate(String &sb) {
	HASH md5bin;
	HASHHEX md5hex;
	char ts_buf[16], auth_buf[64], buf[128];
	
	SquidMD5_CTX M;
	time_t timestamp = time(NULL);

	snprintf(ts_buf, 16, "%d", static_cast<int>(timestamp));
	snprintf(auth_buf, 64, "%s%s%s", ts_buf, DCP_AUTH_VALUE, ts_buf);
	SquidMD5Init(&M);
	SquidMD5Update(&M, auth_buf, strlen(auth_buf));
	SquidMD5Final((unsigned char *) md5bin, &M);
	CvtHex(md5bin, md5hex);
	snprintf(buf, 128, "ps=%s-0-0-0, sid=%s, b=%s, p=%s, c=%s", ts_buf, md5hex, DCP_BUILD, DCP_PATCH, DCP_PLATFORM);
	sb.append(buf);
}
