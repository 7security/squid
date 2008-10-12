
/*
 * $Id: ICAPConfig.cc,v 1.21 2008/02/12 23:12:45 rousskov Exp $
 *
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
 *
 * Copyright (c) 2003, Robert Collins <robertc@squid-cache.org>
 */

#include "squid.h"

#include "ConfigParser.h"
#include "ACL.h"
#include "Store.h"
#include "Array.h"	// really Vector
#include "ICAPConfig.h"
#include "ICAPServiceRep.h"
#include "HttpRequest.h"
#include "HttpReply.h"
#include "ACLChecklist.h"
#include "wordlist.h"

ICAPConfig TheICAPConfig;

ICAPConfig::ICAPConfig(): preview_enable(0), preview_size(0),
        connect_timeout_raw(0), io_timeout_raw(0), reuse_connections(0),
        client_username_header(NULL), client_username_encode(0)
{
}

ICAPConfig::~ICAPConfig()
{
    // TODO: delete client_username_header?
}

Adaptation::ServicePointer
ICAPConfig::createService(const Adaptation::ServiceConfig &cfg)
{
    ICAPServiceRep::Pointer s = new ICAPServiceRep(cfg);
    s->setSelf(s);
    return s.getRaw();
}

time_t ICAPConfig::connect_timeout(bool bypassable) const
{
    if (connect_timeout_raw > 0)
        return connect_timeout_raw; // explicitly configured

    return bypassable ? ::Config.Timeout.peer_connect : ::Config.Timeout.connect;
}

time_t ICAPConfig::io_timeout(bool) const
{
    if (io_timeout_raw > 0)
        return io_timeout_raw; // explicitly configured
    // TODO: provide a different default for an ICAP transaction that
    // can still be bypassed
    return ::Config.Timeout.read;
}
