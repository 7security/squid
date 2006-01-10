
/*
 * $Id: http.cc,v 1.481 2006/01/09 20:42:35 wessels Exp $
 *
 * DEBUG: section 11    Hypertext Transfer Protocol (HTTP)
 * AUTHOR: Harvest Derived
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
 */

/*
 * Anonymizing patch by lutz@as-node.jena.thur.de
 * have a look into http-anon.c to get more informations.
 */

#include "squid.h"
#include "MemBuf.h"
#include "http.h"
#include "AuthUserRequest.h"
#include "Store.h"
#include "HttpReply.h"
#include "HttpRequest.h"
#include "MemObject.h"
#include "HttpHdrContRange.h"
#include "ACLChecklist.h"
#include "fde.h"
#if DELAY_POOLS
#include "DelayPools.h"
#endif
#if ICAP_CLIENT
#include "ICAP/ICAPClientRespmodPrecache.h"
#include "ICAP/ICAPConfig.h"
extern ICAPConfig TheICAPConfig;
#endif

CBDATA_CLASS_INIT(HttpStateData);

static const char *const crlf = "\r\n";

static PF httpStateFree;
static PF httpTimeout;
static void httpCacheNegatively(StoreEntry *);
static void httpMakePrivate(StoreEntry *);
static void httpMakePublic(StoreEntry *);
static void httpMaybeRemovePublic(StoreEntry *, http_status);
static void copyOneHeaderFromClientsideRequestToUpstreamRequest(const HttpHeaderEntry *e, String strConnection, HttpRequest * request, HttpRequest * orig_request,
        HttpHeader * hdr_out, int we_do_ranges, http_state_flags);
#if ICAP_CLIENT
static void icapAclCheckDoneWrapper(ICAPServiceRep::Pointer service, void *data);
#endif

HttpStateData::HttpStateData(FwdState *theFwdState)
{
    debugs(11,5,HERE << "HttpStateData " << this << " created");
    ignoreCacheControl = false;
    surrogateNoStore = false;
    fwd = theFwdState;
    entry = fwd->entry;
    storeLockObject(entry);
    fd = fwd->server_fd;
    readBuf = new MemBuf;
    readBuf->init(4096, SQUID_TCP_SO_RCVBUF);
    orig_request = requestLink(fwd->request);

    if (fwd->servers)
        _peer = fwd->servers->_peer;         /* might be NULL */

    if (_peer) {
        const char *url;

        if (_peer->options.originserver)
            url = orig_request->urlpath.buf();
        else
            url = storeUrl(entry);

        HttpRequest * proxy_req = requestCreate(orig_request->method,
                                                orig_request->protocol, url);

        xstrncpy(proxy_req->host, _peer->host, SQUIDHOSTNAMELEN);

        proxy_req->port = _peer->http_port;

        proxy_req->flags = orig_request->flags;

        proxy_req->lastmod = orig_request->lastmod;

        proxy_req->flags.proxying = 1;

        request = requestLink(proxy_req);

        /*
         * This NEIGHBOR_PROXY_ONLY check probably shouldn't be here.
         * We might end up getting the object from somewhere else if,
         * for example, the request to this neighbor fails.
         */
        if (_peer->options.proxy_only)
            storeReleaseRequest(entry);

#if DELAY_POOLS

        entry->setNoDelay(_peer->options.no_delay);

#endif

    } else {
        request = requestLink(orig_request);
    }

    /*
     * register the handler to free HTTP state data when the FD closes
     */
    comm_add_close_handler(fd, httpStateFree, this);
}

HttpStateData::~HttpStateData()
{
    if (request_body_buf) {
        if (orig_request->body_connection.getRaw()) {
            clientAbortBody(orig_request);
        }

        if (request_body_buf) {
            memFree(request_body_buf, MEM_8K_BUF);
            request_body_buf = NULL;
        }
    }

    storeUnlockObject(entry);

    if (!readBuf->isNull())
        readBuf->clean();

    delete readBuf;

    requestUnlink(request);

    requestUnlink(orig_request);

    request = NULL;

    orig_request = NULL;

    fwd = NULL;	// refcounted

    if (reply)
        delete reply;

#if ICAP_CLIENT

    if (icap) {
        delete icap;
        cbdataReferenceDone(icap);
    }

#endif
    debugs(11,5,HERE << "HttpStateData " << this << " destroyed");
}

static void
httpStateFree(int fd, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    debug(11,5)("httpStateFree: FD %d, httpState=%p\n", fd, data);

    if (httpState)
        delete httpState;
}

int
httpCachable(method_t method)
{
    /* GET and HEAD are cachable. Others are not. */

    if (method != METHOD_GET && method != METHOD_HEAD)
        return 0;

    /* else cachable */
    return 1;
}

static void
httpTimeout(int fd, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    StoreEntry *entry = httpState->entry;
    debug(11, 4) ("httpTimeout: FD %d: '%s'\n", fd, storeUrl(entry));

    if (entry->store_status == STORE_PENDING) {
        httpState->fwd->fail(errorCon(ERR_READ_TIMEOUT, HTTP_GATEWAY_TIMEOUT));
    }

    comm_close(fd);
}

/* This object can be cached for a long time */
static void
httpMakePublic(StoreEntry * entry)
{
    if (EBIT_TEST(entry->flags, ENTRY_CACHABLE))
        storeSetPublicKey(entry);
}

/* This object should never be cached at all */
static void
httpMakePrivate(StoreEntry * entry)
{
    storeExpireNow(entry);
    storeReleaseRequest(entry);	/* delete object when not used */
    /* storeReleaseRequest clears ENTRY_CACHABLE flag */
}

/* This object may be negatively cached */
static void
httpCacheNegatively(StoreEntry * entry)
{
    storeNegativeCache(entry);

    if (EBIT_TEST(entry->flags, ENTRY_CACHABLE))
        storeSetPublicKey(entry);
}

static void
httpMaybeRemovePublic(StoreEntry * e, http_status status)
{

    int remove
        = 0;

    int forbidden = 0;

    StoreEntry *pe;

    if (!EBIT_TEST(e->flags, KEY_PRIVATE))
        return;

    switch (status) {

    case HTTP_OK:

    case HTTP_NON_AUTHORITATIVE_INFORMATION:

    case HTTP_MULTIPLE_CHOICES:

    case HTTP_MOVED_PERMANENTLY:

    case HTTP_MOVED_TEMPORARILY:

    case HTTP_GONE:

    case HTTP_NOT_FOUND:

        remove
            = 1;

        break;

    case HTTP_FORBIDDEN:

    case HTTP_METHOD_NOT_ALLOWED:
        forbidden = 1;

        break;

#if WORK_IN_PROGRESS

    case HTTP_UNAUTHORIZED:
        forbidden = 1;

        break;

#endif

    default:
#if QUESTIONABLE
        /*
         * Any 2xx response should eject previously cached entities...
         */

        if (status >= 200 && status < 300)
            remove
                = 1;

#endif

        break;
    }

    if (!remove
            && !forbidden)
        return;

    assert(e->mem_obj);

    if (e->mem_obj->request)
        pe = storeGetPublicByRequest(e->mem_obj->request);
    else
        pe = storeGetPublic(e->mem_obj->url, e->mem_obj->method);

    if (pe != NULL) {
        assert(e != pe);
        storeRelease(pe);
    }

    /*
     * Also remove any cached HEAD response in case the object has
     * changed.
     */
    if (e->mem_obj->request)
        pe = storeGetPublicByRequestMethod(e->mem_obj->request, METHOD_HEAD);
    else
        pe = storeGetPublic(e->mem_obj->url, METHOD_HEAD);

    if (pe != NULL) {
        assert(e != pe);
        storeRelease(pe);
    }

    if (forbidden)
        return;

    switch (e->mem_obj->method) {

    case METHOD_PUT:

    case METHOD_DELETE:

    case METHOD_PROPPATCH:

    case METHOD_MKCOL:

    case METHOD_MOVE:

    case METHOD_BMOVE:

    case METHOD_BDELETE:
        /*
         * Remove any cached GET object if it is beleived that the
         * object may have changed as a result of other methods
         */

        if (e->mem_obj->request)
            pe = storeGetPublicByRequestMethod(e->mem_obj->request, METHOD_GET);
        else
            pe = storeGetPublic(e->mem_obj->url, METHOD_GET);

        if (pe != NULL) {
            assert(e != pe);
            storeRelease(pe);
        }

        break;

    default:
        /* Keep GCC happy. The methods above are all mutating HTTP methods
         */
        break;
    }
}

void
HttpStateData::processSurrogateControl(HttpReply *reply)
{
#if ESI

    if (request->flags.accelerated && reply->surrogate_control) {
        HttpHdrScTarget *sctusable =
            httpHdrScGetMergedTarget(reply->surrogate_control,
                                     Config.Accel.surrogate_id);

        if (sctusable) {
            if (EBIT_TEST(sctusable->mask, SC_NO_STORE) ||
                    (Config.onoff.surrogate_is_remote
                     && EBIT_TEST(sctusable->mask, SC_NO_STORE_REMOTE))) {
                surrogateNoStore = true;
                httpMakePrivate(entry);
            }

            /* The HttpHeader logic cannot tell if the header it's parsing is a reply to an
             * accelerated request or not...
             * Still, this is an abtraction breach. - RC
             */
            if (sctusable->max_age != -1) {
                if (sctusable->max_age < sctusable->max_stale)
                    reply->expires = reply->date + sctusable->max_age;
                else
                    reply->expires = reply->date + sctusable->max_stale;

                /* And update the timestamps */
                storeTimestampsSet(entry);
            }

            /* We ignore cache-control directives as per the Surrogate specification */
            ignoreCacheControl = true;

            httpHdrScTargetDestroy(sctusable);
        }
    }

#endif
}

int
HttpStateData::cacheableReply()
{
    HttpReply const *rep = getReply();
    HttpHeader const *hdr = &rep->header;
    const int cc_mask = (rep->cache_control) ? rep->cache_control->mask : 0;
    const char *v;
#if HTTP_VIOLATIONS

    const refresh_t *R = NULL;
#endif

    if (surrogateNoStore)
        return 0;

    if (!ignoreCacheControl) {
        if (EBIT_TEST(cc_mask, CC_PRIVATE)) {
#if HTTP_VIOLATIONS

            if (!R)
                R = refreshLimits(entry->mem_obj->url);

            if (R && !R->flags.ignore_private)
#endif

                return 0;
        }

        if (EBIT_TEST(cc_mask, CC_NO_CACHE)) {
#if HTTP_VIOLATIONS

            if (!R)
                R = refreshLimits(entry->mem_obj->url);

            if (R && !R->flags.ignore_no_cache)
#endif

                return 0;
        }

        if (EBIT_TEST(cc_mask, CC_NO_STORE)) {
#if HTTP_VIOLATIONS

            if (!R)
                R = refreshLimits(entry->mem_obj->url);

            if (R && !R->flags.ignore_no_store)
#endif

                return 0;
        }
    }

    if (request->flags.auth) {
        /*
         * Responses to requests with authorization may be cached
         * only if a Cache-Control: public reply header is present.
         * RFC 2068, sec 14.9.4
         */

        if (!EBIT_TEST(cc_mask, CC_PUBLIC)) {
#if HTTP_VIOLATIONS

            if (!R)
                R = refreshLimits(entry->mem_obj->url);

            if (R && !R->flags.ignore_auth)
#endif

                return 0;
        }
    }

    /* Pragma: no-cache in _replies_ is not documented in HTTP,
     * but servers like "Active Imaging Webcast/2.0" sure do use it */
    if (httpHeaderHas(hdr, HDR_PRAGMA)) {
        String s = httpHeaderGetList(hdr, HDR_PRAGMA);
        const int no_cache = strListIsMember(&s, "no-cache", ',');
        s.clean();

        if (no_cache) {
#if HTTP_VIOLATIONS

            if (!R)
                R = refreshLimits(entry->mem_obj->url);

            if (R && !R->flags.ignore_no_cache)
#endif

                return 0;
        }
    }

    /*
     * The "multipart/x-mixed-replace" content type is used for
     * continuous push replies.  These are generally dynamic and
     * probably should not be cachable
     */
    if ((v = httpHeaderGetStr(hdr, HDR_CONTENT_TYPE)))
        if (!strncasecmp(v, "multipart/x-mixed-replace", 25))
            return 0;

    switch (getReply()->sline.status) {
        /* Responses that are cacheable */

    case HTTP_OK:

    case HTTP_NON_AUTHORITATIVE_INFORMATION:

    case HTTP_MULTIPLE_CHOICES:

    case HTTP_MOVED_PERMANENTLY:

    case HTTP_GONE:
        /*
         * Don't cache objects that need to be refreshed on next request,
         * unless we know how to refresh it.
         */

        if (!refreshIsCachable(entry))
            return 0;

        /* don't cache objects from peers w/o LMT, Date, or Expires */
        /* check that is it enough to check headers @?@ */
        if (rep->date > -1)
            return 1;
        else if (rep->last_modified > -1)
            return 1;
        else if (!_peer)
            return 1;

        /* @?@ (here and 302): invalid expires header compiles to squid_curtime */
        else if (rep->expires > -1)
            return 1;
        else
            return 0;

        /* NOTREACHED */
        break;

        /* Responses that only are cacheable if the server says so */

    case HTTP_MOVED_TEMPORARILY:
        if (rep->expires > -1)
            return 1;
        else
            return 0;

        /* NOTREACHED */
        break;

        /* Errors can be negatively cached */

    case HTTP_NO_CONTENT:

    case HTTP_USE_PROXY:

    case HTTP_BAD_REQUEST:

    case HTTP_FORBIDDEN:

    case HTTP_NOT_FOUND:

    case HTTP_METHOD_NOT_ALLOWED:

    case HTTP_REQUEST_URI_TOO_LARGE:

    case HTTP_INTERNAL_SERVER_ERROR:

    case HTTP_NOT_IMPLEMENTED:

    case HTTP_BAD_GATEWAY:

    case HTTP_SERVICE_UNAVAILABLE:

    case HTTP_GATEWAY_TIMEOUT:
        return -1;

        /* NOTREACHED */
        break;

        /* Some responses can never be cached */

    case HTTP_PARTIAL_CONTENT:	/* Not yet supported */

    case HTTP_SEE_OTHER:

    case HTTP_NOT_MODIFIED:

    case HTTP_UNAUTHORIZED:

    case HTTP_PROXY_AUTHENTICATION_REQUIRED:

    case HTTP_INVALID_HEADER:	/* Squid header parsing error */

    case HTTP_HEADER_TOO_LARGE:
        return 0;

    default:			/* Unknown status code */
        debug (11,0)("HttpStateData::cacheableReply: unknown http status code in reply\n");

        return 0;

        /* NOTREACHED */
        break;
    }

    /* NOTREACHED */
}

/*
 * For Vary, store the relevant request headers as 
 * virtual headers in the reply
 * Returns false if the variance cannot be stored
 */
const char *
httpMakeVaryMark(HttpRequest * request, HttpReply const * reply)
{
    String vary, hdr;
    const char *pos = NULL;
    const char *item;
    const char *value;
    int ilen;
    static String vstr;

    vstr.clean();
    vary = httpHeaderGetList(&reply->header, HDR_VARY);

    while (strListGetItem(&vary, ',', &item, &ilen, &pos)) {
        char *name = (char *)xmalloc(ilen + 1);
        xstrncpy(name, item, ilen + 1);
        Tolower(name);

        if (strcmp(name, "*") == 0) {
            /* Can not handle "Vary: *" withtout ETag support */
            safe_free(name);
            vstr.clean();
            break;
        }

        strListAdd(&vstr, name, ',');
        hdr = httpHeaderGetByName(&request->header, name);
        safe_free(name);
        value = hdr.buf();

        if (value) {
            value = rfc1738_escape_part(value);
            vstr.append("=\"", 2);
            vstr.append(value);
            vstr.append("\"", 1);
        }

        hdr.clean();
    }

    vary.clean();
#if X_ACCELERATOR_VARY

    pos = NULL;
    vary = httpHeaderGetList(&reply->header, HDR_X_ACCELERATOR_VARY);

    while (strListGetItem(&vary, ',', &item, &ilen, &pos)) {
        char *name = (char *)xmalloc(ilen + 1);
        xstrncpy(name, item, ilen + 1);
        Tolower(name);
        strListAdd(&vstr, name, ',');
        hdr = httpHeaderGetByName(&request->header, name);
        safe_free(name);
        value = hdr.buf();

        if (value) {
            value = rfc1738_escape_part(value);
            vstr.append("=\"", 2);
            vstr.append(value);
            vstr.append("\"", 1);
        }

        hdr.clean();
    }

    vary.clean();
#endif

    debug(11, 3) ("httpMakeVaryMark: %s\n", vstr.buf());
    return vstr.buf();
}

void
HttpStateData::failReply(HttpReply *reply, http_status const & status)
{
    reply->sline.version = HttpVersion(1, 0);
    reply->sline.status = status;
    storeEntryReplaceObject (entry, reply);

    if (eof == 1) {
        transactionComplete();
    }
}

void
HttpStateData::keepaliveAccounting(HttpReply *reply)
{
    if (flags.keepalive)
        if (_peer)
            _peer->stats.n_keepalives_sent++;

    if (reply->keep_alive) {
        if (_peer)
            _peer->stats.n_keepalives_recv++;

        if (Config.onoff.detect_broken_server_pconns && reply->bodySize(request->method) == -1) {
            debug(11, 1) ("keepaliveAccounting: Impossible keep-alive header from '%s'\n", storeUrl(entry));
            // debug(11, 2) ("GOT HTTP REPLY HDR:\n---------\n%s\n----------\n", readBuf->content());
            flags.keepalive_broken = 1;
        }
    }
}

void
HttpStateData::checkDateSkew(HttpReply *reply)
{
    if (reply->date > -1 && !_peer) {
        int skew = abs((int)(reply->date - squid_curtime));

        if (skew > 86400)
            debug(11, 3) ("%s's clock is skewed by %d seconds!\n",
                          request->host, skew);
    }
}

/*
 * This creates the error page itself.. its likely
 * that the forward ported reply header max size patch
 * generates non http conformant error pages - in which
 * case the errors where should be 'BAD_GATEWAY' etc
 */
void
HttpStateData::processReplyHeader()
{
    /* Creates a blank header. If this routine is made incremental, this will
     * not do 
     */
    reply = new HttpReply;
    Ctx ctx = ctx_enter(entry->mem_obj->url);
    debug(11, 3) ("processReplyHeader: key '%s'\n", entry->getMD5Text());

    assert(!flags.headers_parsed);

    http_status error = HTTP_STATUS_NONE;

    const bool parsed = reply->parse(readBuf, eof, &error);

    if (!parsed && error > 0) { // unrecoverable parsing error
        debugs(11, 3, "processReplyHeader: Non-HTTP-compliant header: '" <<  readBuf->content() << "'");
        flags.headers_parsed = 1;
        // negated result yields http_status
        failReply (reply, error);	// consumes reply
        reply = NULL;
        ctx_exit(ctx);
        return;
    }

    if (!parsed) { // need more data
        assert(!error);
        assert(!eof);
        delete reply;
        reply = NULL;
        ctx_exit(ctx);
        return;
    }

    debug(11, 9) ("GOT HTTP REPLY HDR:\n---------\n%s\n----------\n",
                  readBuf->content());

    readBuf->consume(headersEnd(readBuf->content(), readBuf->contentSize()));
    flags.headers_parsed = 1;

    keepaliveAccounting(reply);

    checkDateSkew(reply);

    processSurrogateControl (reply);

    /* TODO: IF the reply is a 1.0 reply, AND it has a Connection: Header
     * Parse the header and remove all referenced headers
     */

#if ICAP_CLIENT

    if (TheICAPConfig.onoff) {
        ICAPAccessCheck *icap_access_check =
            new ICAPAccessCheck(ICAP::methodRespmod, ICAP::pointPreCache, request, reply, icapAclCheckDoneWrapper, this);

        icapAccessCheckPending = true;
        icap_access_check->check(); // will eventually delete self
        ctx_exit(ctx);
        return;
    }

#endif

    storeEntryReplaceObject(entry, reply);

    /* Note storeEntryReplaceObject() consumes reply, so we cannot use it */
    reply = NULL;

    haveParsedReplyHeaders();

    if (eof == 1) {
        transactionComplete();
    }

    ctx_exit(ctx);
}

/*
 * This function used to be joined with processReplyHeader(), but
 * we split it for ICAP.
 */
void
HttpStateData::haveParsedReplyHeaders()
{
    Ctx ctx = ctx_enter(entry->mem_obj->url);

    if (getReply()->sline.status == HTTP_PARTIAL_CONTENT &&
            getReply()->content_range)
        currentOffset = getReply()->content_range->spec.offset;

    storeTimestampsSet(entry);

    /* Check if object is cacheable or not based on reply code */
    debug(11, 3) ("haveParsedReplyHeaders: HTTP CODE: %d\n", getReply()->sline.status);

    if (neighbors_do_private_keys)
        httpMaybeRemovePublic(entry, getReply()->sline.status);

    if (httpHeaderHas(&getReply()->header, HDR_VARY)
#if X_ACCELERATOR_VARY
            || httpHeaderHas(&getReply()->header, HDR_X_ACCELERATOR_VARY)
#endif
       ) {
        const char *vary = httpMakeVaryMark(orig_request, getReply());

        if (!vary) {
            httpMakePrivate(entry);
            goto no_cache;

        }

        entry->mem_obj->vary_headers = xstrdup(vary);
    }

#if WIP_FWD_LOG
    fwdStatus(fwd, s);

#endif
    /*
     * If its not a reply that we will re-forward, then
     * allow the client to get it.
     */
    if (!fwd->reforwardableStatus(getReply()->sline.status))
        EBIT_CLR(entry->flags, ENTRY_FWD_HDR_WAIT);

    switch (cacheableReply()) {

    case 1:
        httpMakePublic(entry);
        break;

    case 0:
        httpMakePrivate(entry);
        break;

    case -1:

        if (Config.negativeTtl > 0)
            httpCacheNegatively(entry);
        else
            httpMakePrivate(entry);

        break;

    default:
        assert(0);

        break;
    }

no_cache:

    if (!ignoreCacheControl && getReply()->cache_control) {
        if (EBIT_TEST(getReply()->cache_control->mask, CC_PROXY_REVALIDATE))
            EBIT_SET(entry->flags, ENTRY_REVALIDATE);
        else if (EBIT_TEST(getReply()->cache_control->mask, CC_MUST_REVALIDATE))
            EBIT_SET(entry->flags, ENTRY_REVALIDATE);
    }

    ctx_exit(ctx);
#if HEADERS_LOG

    headersLog(1, 0, request->method, getReply());
#endif
}

HttpStateData::ConnectionStatus
HttpStateData::statusIfComplete() const
{
    HttpReply const *rep = getReply();
    /* If the reply wants to close the connection, it takes precedence */

    if (httpHeaderHasConnDir(&rep->header, "close"))
        return COMPLETE_NONPERSISTENT_MSG;

    /* If we didn't send a keep-alive request header, then this
     * can not be a persistent connection.
     */
    if (!flags.keepalive)
        return COMPLETE_NONPERSISTENT_MSG;

    /*
     * If we haven't sent the whole request then this can not be a persistent
     * connection.
     */
    if (!flags.request_sent) {
        debug(11, 1) ("statusIfComplete: Request not yet fully sent \"%s %s\"\n",
                      RequestMethodStr[orig_request->method],
                      storeUrl(entry));
        return COMPLETE_NONPERSISTENT_MSG;
    }

    /*
     * What does the reply have to say about keep-alive?
     */
    /*
     * XXX BUG?
     * If the origin server (HTTP/1.0) does not send a keep-alive
     * header, but keeps the connection open anyway, what happens?
     * We'll return here and http.c waits for an EOF before changing
     * store_status to STORE_OK.   Combine this with ENTRY_FWD_HDR_WAIT
     * and an error status code, and we might have to wait until
     * the server times out the socket.
     */
    if (!rep->keep_alive)
        return COMPLETE_NONPERSISTENT_MSG;

    return COMPLETE_PERSISTENT_MSG;
}

HttpStateData::ConnectionStatus
HttpStateData::persistentConnStatus() const
{
    HttpReply const *reply = getReply();
    int clen;
    debug(11, 3) ("persistentConnStatus: FD %d\n", fd);
    ConnectionStatus result = statusIfComplete();
    debug(11, 5) ("persistentConnStatus: content_length=%d\n",
                  reply->content_length);
    /* If we haven't seen the end of reply headers, we are not done */

    debug(11,5)("persistentConnStatus: flags.headers_parsed=%d\n", flags.headers_parsed);

    if (!flags.headers_parsed)
        return INCOMPLETE_MSG;

    clen = reply->bodySize(request->method);

    debug(11,5)("persistentConnStatus: clen=%d\n", clen);

    /* If there is no message body, we can be persistent */
    if (0 == clen)
        return result;

    /* If the body size is unknown we must wait for EOF */
    if (clen < 0)
        return INCOMPLETE_MSG;

    /* If the body size is known, we must wait until we've gotten all of it.  */
    /* old technique:
     * if (entry->mem_obj->endOffset() < reply->content_length + reply->hdr_sz) */
    debug(11,5)("persistentConnStatus: body_bytes_read=%d, content_length=%d\n",
                body_bytes_read, reply->content_length);

    if (body_bytes_read < reply->content_length)
        return INCOMPLETE_MSG;

    /* We got it all */
    return result;
}

/*
 * This is the callback after some data has been read from the network
 */
void
HttpStateData::ReadReplyWrapper(int fd, char *buf, size_t len, comm_err_t flag, int xerrno, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    assert (fd == httpState->fd);
    // assert(buf == readBuf->content());
    PROF_start(HttpStateData_readReply);
    httpState->readReply (len, flag, xerrno);
    PROF_stop(HttpStateData_readReply);
}

/* XXX this function is too long! */
void
HttpStateData::readReply (size_t len, comm_err_t flag, int xerrno)
{
    int bin;
    int clen;
    flags.do_next_read = 0;

    /*
     * Bail out early on COMM_ERR_CLOSING - close handlers will tidy up for us
     */

    if (flag == COMM_ERR_CLOSING) {
        debug (11,3)("http socket closing\n");
        return;
    }

    if (EBIT_TEST(entry->flags, ENTRY_ABORTED)) {
        maybeReadData();
        return;
    }

    errno = 0;
    /* prepare the read size for the next read (if any) */

    debug(11, 5) ("httpReadReply: FD %d: len %d.\n", fd, (int)len);

    if (flag == COMM_OK && len > 0) {
        readBuf->appended(len);
#if DELAY_POOLS

        DelayId delayId = entry->mem_obj->mostBytesAllowed();
        delayId.bytesIn(len);
#endif

        kb_incr(&statCounter.server.all.kbytes_in, len);
        kb_incr(&statCounter.server.http.kbytes_in, len);
        IOStats.Http.reads++;

        for (clen = len - 1, bin = 0; clen; bin++)
            clen >>= 1;

        IOStats.Http.read_hist[bin]++;
    }

    /* here the RFC says we should ignore whitespace between replies, but we can't as
     * doing so breaks HTTP/0.9 replies beginning with witespace, and in addition
     * the response splitting countermeasures is extremely likely to trigger on this,
     * not allowing connection reuse in the first place.
     */
#if DONT_DO_THIS
    if (!flags.headers_parsed && flag == COMM_OK && len > 0 && fd_table[fd].uses > 1) {
        /* Skip whitespace between replies */

        while (len > 0 && isspace(*buf))
            xmemmove(buf, buf + 1, len--);

        if (len == 0) {
            /* Continue to read... */
            /* Timeout NOT increased. This whitespace was from previous reply */
            flags.do_next_read = 1;
            maybeReadData();
            return;
        }
    }

#endif

    if (flag != COMM_OK || len < 0) {
        debug(50, 2) ("httpReadReply: FD %d: read failure: %s.\n",
                      fd, xstrerror());

        if (ignoreErrno(errno)) {
            flags.do_next_read = 1;
        } else {
            ErrorState *err;
            err = errorCon(ERR_READ_ERROR, HTTP_BAD_GATEWAY);
            err->xerrno = errno;
            fwd->fail(err);
            flags.do_next_read = 0;
            comm_close(fd);
        }
    } else if (flag == COMM_OK && len == 0 && !flags.headers_parsed) {
        fwd->fail(errorCon(ERR_ZERO_SIZE_OBJECT, HTTP_BAD_GATEWAY));
        eof = 1;
        flags.do_next_read = 0;
        comm_close(fd);
    } else if (flag == COMM_OK && len == 0) {
        /* Connection closed; retrieval done. */
        eof = 1;

        if (!flags.headers_parsed)
            /*
            * When we called processReplyHeader() before, we
            * didn't find the end of headers, but now we are
            * definately at EOF, so we want to process the reply
            * headers.
             */
            processReplyHeader();
        else if (getReply()->sline.status == HTTP_INVALID_HEADER && HttpVersion(0,9) != getReply()->sline.version) {
            fwd->fail(errorCon(ERR_INVALID_RESP, HTTP_BAD_GATEWAY));
            flags.do_next_read = 0;
        } else {
            if (entry->mem_obj->getReply()->sline.status == HTTP_HEADER_TOO_LARGE) {
                storeEntryReset(entry);
                fwd->fail( errorCon(ERR_TOO_BIG, HTTP_BAD_GATEWAY));
                fwd->dontRetry(true);
                flags.do_next_read = 0;
                comm_close(fd);
            } else {
                transactionComplete();
            }
        }
    } else {
        if (!flags.headers_parsed) {
            processReplyHeader();

            if (flags.headers_parsed) {
                http_status s = getReply()->sline.status;
                HttpVersion httpver = getReply()->sline.version;

                if (s == HTTP_INVALID_HEADER && httpver != HttpVersion(0,9)) {
                    storeEntryReset(entry);
                    fwd->fail( errorCon(ERR_INVALID_RESP, HTTP_BAD_GATEWAY));
                    comm_close(fd);
                    return;
                }

            }
        }

        PROF_start(HttpStateData_processReplyBody);
        processReplyBody();
        PROF_stop(HttpStateData_processReplyBody);
    }
}

/*
 * Call this when there is data from the origin server
 * which should be sent to either StoreEntry, or to ICAP...
 */
void
HttpStateData::writeReplyBody(const char *data, int len)
{
#if ICAP_CLIENT

    if (icap)  {
        icap->sendMoreData (StoreIOBuffer(len, 0, (char*)data));
        return;
    }

#endif

    entry->write (StoreIOBuffer(len, currentOffset, (char*)data));

    currentOffset += len;
}

/*
 * processReplyBody has two purposes:
 *  1 - take the reply body data, if any, and put it into either
 *      the StoreEntry, or give it over to ICAP.
 *  2 - see if we made it to the end of the response (persistent
 *      connections and such)
 */
void
HttpStateData::processReplyBody()
{
    if (!flags.headers_parsed) {
        flags.do_next_read = 1;
        maybeReadData();
        return;
    }

#if ICAP_CLIENT
    if (icapAccessCheckPending)
        return;

#endif

    /*
     * At this point the reply headers have been parsed and consumed.
     * That means header content has been removed from readBuf and
     * it contains only body data.
     */
    writeReplyBody(readBuf->content(), readBuf->contentSize());

    body_bytes_read += readBuf->contentSize();

    readBuf->consume(readBuf->contentSize());

    if (EBIT_TEST(entry->flags, ENTRY_ABORTED)) {
        /*
         * the above writeReplyBody() call could ABORT this entry,
         * in that case, the server FD should already be closed.
         * there's nothing for us to do.
         */
        (void) 0;
    } else
        switch (persistentConnStatus()) {

        case INCOMPLETE_MSG:
            debug(11,5)("processReplyBody: INCOMPLETE_MSG\n");
            /* Wait for more data or EOF condition */

            if (flags.keepalive_broken) {
                commSetTimeout(fd, 10, NULL, NULL);
            } else {
                commSetTimeout(fd, Config.Timeout.read, NULL, NULL);
            }

            flags.do_next_read = 1;
            break;

        case COMPLETE_PERSISTENT_MSG:
            debug(11,5)("processReplyBody: COMPLETE_PERSISTENT_MSG\n");
            /* yes we have to clear all these! */
            commSetTimeout(fd, -1, NULL, NULL);
            flags.do_next_read = 0;

            comm_remove_close_handler(fd, httpStateFree, this);
            fwd->unregister(fd);

            if (_peer) {
                if (_peer->options.originserver)
                    fwd->pconnPush(fd, _peer->name, orig_request->port, orig_request->host);
                else
                    fwd->pconnPush(fd, _peer->name, _peer->http_port, NULL);
            } else {
                fwd->pconnPush(fd, request->host, request->port, NULL);
            }

            fd = -1;

            transactionComplete();
            return;

        case COMPLETE_NONPERSISTENT_MSG:
            debug(11,5)("processReplyBody: COMPLETE_NONPERSISTENT_MSG\n");
            transactionComplete();
            return;
        }

    maybeReadData();
}

void
HttpStateData::maybeReadData()
{
    int read_sz = readBuf->spaceSize();
#if ICAP_CLIENT

    if (icap) {
        /*
         * Our ICAP message pipes have a finite size limit.  We
         * should not read more data from the network than will fit
         * into the pipe buffer.  If totally full, don't register
         * the read handler at all.  The ICAP side will call our
         * icapSpaceAvailable() method when it has free space again.
         */
        int icap_space = icap->potentialSpaceSize();

        debugs(11,9, "HttpStateData may read up to min(" << icap_space <<
               ", " << read_sz << ") bytes");

        if (icap_space < read_sz)
            read_sz = icap_space;
    }

#endif

    debugs(11,9, "HttpStateData may read up to " << read_sz << " bytes");

    /*
     * why <2? Because delayAwareRead() won't actually read if
     * you ask it to read 1 byte.  The delayed read request
     * just gets re-queued until the client side drains, then
     * the I/O thread hangs.  Better to not register any read
     * handler until we get a notification from someone that
     * its okay to read again.
     */
    if (read_sz < 2)
        return;

    if (flags.do_next_read) {
        flags.do_next_read = 0;
        entry->delayAwareRead(fd, readBuf->space(), read_sz, ReadReplyWrapper, this);
    }
}

/*
 * This will be called when request write is complete.
 */
void
HttpStateData::SendComplete(int fd, char *bufnotused, size_t size, comm_err_t errflag, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    debug(11, 5) ("httpSendComplete: FD %d: size %d: errflag %d.\n",
                  fd, (int) size, errflag);
#if URL_CHECKSUM_DEBUG

    entry->mem_obj->checkUrlChecksum();
#endif

    if (size > 0) {
        fd_bytes(fd, size, FD_WRITE);
        kb_incr(&statCounter.server.all.kbytes_out, size);
        kb_incr(&statCounter.server.http.kbytes_out, size);
    }

    if (errflag == COMM_ERR_CLOSING)
        return;

    if (errflag) {
        ErrorState *err;
        err = errorCon(ERR_WRITE_ERROR, HTTP_BAD_GATEWAY);
        err->xerrno = errno;
        httpState->fwd->fail(err);
        comm_close(fd);
        return;
    }

    /*
     * Set the read timeout here because it hasn't been set yet.
     * We only set the read timeout after the request has been
     * fully written to the server-side.  If we start the timeout
     * after connection establishment, then we are likely to hit
     * the timeout for POST/PUT requests that have very large
     * request bodies.
     */
    commSetTimeout(fd, Config.Timeout.read, httpTimeout, httpState);

    httpState->flags.request_sent = 1;
}

/*
 * Calling this function marks the end of the HTTP transaction.
 * i.e., done talking to the HTTP server.  With ICAP, however, that
 * does not mean that we're done with HttpStateData and the StoreEntry.
 * We'll be expecting adapted data to come back from the ICAP
 * routines.
 */
void
HttpStateData::transactionComplete()
{
    debugs(11,5,HERE << "transactionComplete FD " << fd << " this " << this);

    if (fd >= 0) {
        fwd->unregister(fd);
        comm_remove_close_handler(fd, httpStateFree, this);
        comm_close(fd);
        fd = -1;
    }

#if ICAP_CLIENT
    if (icap) {
        icap->doneSending();
        return;
    }

#endif

    fwd->complete();

    httpStateFree(-1, this);
}

/*
 * build request headers and append them to a given MemBuf 
 * used by buildRequestPrefix()
 * note: initialised the HttpHeader, the caller is responsible for Clean()-ing
 */
void
HttpStateData::httpBuildRequestHeader(HttpRequest * request,
                                      HttpRequest * orig_request,
                                      StoreEntry * entry,
                                      HttpHeader * hdr_out,
                                      http_state_flags flags)
{
    /* building buffer for complex strings */
#define BBUF_SZ (MAX_URL+32)
    LOCAL_ARRAY(char, bbuf, BBUF_SZ);
    const HttpHeader *hdr_in = &orig_request->header;
    const HttpHeaderEntry *e;
    String strFwd;
    HttpHeaderPos pos = HttpHeaderInitPos;
    assert (hdr_out->owner == hoRequest);
    /* append our IMS header */

    if (request->lastmod > -1)
        httpHeaderPutTime(hdr_out, HDR_IF_MODIFIED_SINCE, request->lastmod);

    bool we_do_ranges = decideIfWeDoRanges (orig_request);

    String strConnection (httpHeaderGetList(hdr_in, HDR_CONNECTION));

    while ((e = httpHeaderGetEntry(hdr_in, &pos)))
        copyOneHeaderFromClientsideRequestToUpstreamRequest(e, strConnection, request, orig_request, hdr_out, we_do_ranges, flags);

    /* Abstraction break: We should interpret multipart/byterange responses
     * into offset-length data, and this works around our inability to do so.
     */
    if (!we_do_ranges && orig_request->multipartRangeRequest()) {
        /* don't cache the result */
        orig_request->flags.cachable = 0;
        /* pretend it's not a range request */
        delete orig_request->range;
        orig_request->range = NULL;
        orig_request->flags.range = 0;
    }

    /* append Via */
    if (Config.onoff.via) {
        String strVia;
        strVia = httpHeaderGetList(hdr_in, HDR_VIA);
        snprintf(bbuf, BBUF_SZ, "%d.%d %s",
                 orig_request->http_ver.major,
                 orig_request->http_ver.minor, ThisCache);
        strListAdd(&strVia, bbuf, ',');
        httpHeaderPutStr(hdr_out, HDR_VIA, strVia.buf());
        strVia.clean();
    }

#if ESI
    {
        /* Append Surrogate-Capabilities */
        String strSurrogate (httpHeaderGetList(hdr_in, HDR_SURROGATE_CAPABILITY));
        snprintf(bbuf, BBUF_SZ, "%s=\"Surrogate/1.0 ESI/1.0\"",
                 Config.Accel.surrogate_id);
        strListAdd(&strSurrogate, bbuf, ',');
        httpHeaderPutStr(hdr_out, HDR_SURROGATE_CAPABILITY, strSurrogate.buf());
    }
#endif

    /* append X-Forwarded-For */
    strFwd = httpHeaderGetList(hdr_in, HDR_X_FORWARDED_FOR);

    if (opt_forwarded_for && orig_request->client_addr.s_addr != no_addr.s_addr)
        strListAdd(&strFwd, inet_ntoa(orig_request->client_addr), ',');
    else
        strListAdd(&strFwd, "unknown", ',');

    httpHeaderPutStr(hdr_out, HDR_X_FORWARDED_FOR, strFwd.buf());

    strFwd.clean();

    /* append Host if not there already */
    if (!httpHeaderHas(hdr_out, HDR_HOST)) {
        if (orig_request->peer_domain) {
            httpHeaderPutStr(hdr_out, HDR_HOST, orig_request->peer_domain);
        } else if (orig_request->port == urlDefaultPort(orig_request->protocol)) {
            /* use port# only if not default */
            httpHeaderPutStr(hdr_out, HDR_HOST, orig_request->host);
        } else {
            httpHeaderPutStrf(hdr_out, HDR_HOST, "%s:%d",
                              orig_request->host, (int) orig_request->port);
        }
    }

    /* append Authorization if known in URL, not in header and going direct */
    if (!httpHeaderHas(hdr_out, HDR_AUTHORIZATION)) {
        if (!request->flags.proxying && *request->login) {
            httpHeaderPutStrf(hdr_out, HDR_AUTHORIZATION, "Basic %s",
                              base64_encode(request->login));
        }
    }

    /* append Proxy-Authorization if configured for peer, and proxying */
    if (request->flags.proxying && orig_request->peer_login &&
            !httpHeaderHas(hdr_out, HDR_PROXY_AUTHORIZATION)) {
        if (*orig_request->peer_login == '*') {
            /* Special mode, to pass the username to the upstream cache */
            char loginbuf[256];
            const char *username = "-";

            if (orig_request->auth_user_request)
                username = orig_request->auth_user_request->username();
            else if (orig_request->extacl_user.size())
                username = orig_request->extacl_user.buf();

            snprintf(loginbuf, sizeof(loginbuf), "%s%s", username, orig_request->peer_login + 1);

            httpHeaderPutStrf(hdr_out, HDR_PROXY_AUTHORIZATION, "Basic %s",
                              base64_encode(loginbuf));
        } else if (strcmp(orig_request->peer_login, "PASS") == 0) {
            if (orig_request->extacl_user.size() && orig_request->extacl_passwd.size()) {
                char loginbuf[256];
                snprintf(loginbuf, sizeof(loginbuf), "%s:%s", orig_request->extacl_user.buf(), orig_request->extacl_passwd.buf());
                httpHeaderPutStrf(hdr_out, HDR_PROXY_AUTHORIZATION, "Basic %s",
                                  base64_encode(loginbuf));
            }
        } else if (strcmp(orig_request->peer_login, "PROXYPASS") == 0) {
            /* Nothing to do */
        } else {
            httpHeaderPutStrf(hdr_out, HDR_PROXY_AUTHORIZATION, "Basic %s",
                              base64_encode(orig_request->peer_login));
        }
    }

    /* append WWW-Authorization if configured for peer */
    if (flags.originpeer && orig_request->peer_login &&
            !httpHeaderHas(hdr_out, HDR_AUTHORIZATION)) {
        if (strcmp(orig_request->peer_login, "PASS") == 0) {
            /* No credentials to forward.. (should have been done above if available) */
        } else if (strcmp(orig_request->peer_login, "PROXYPASS") == 0) {
            /* Special mode, convert proxy authentication to WWW authentication
            * (also applies to authentication provided by external acl)
             */
            const char *auth = httpHeaderGetStr(hdr_in, HDR_PROXY_AUTHORIZATION);

            if (auth && strncasecmp(auth, "basic ", 6) == 0) {
                httpHeaderPutStr(hdr_out, HDR_AUTHORIZATION, auth);
            } else if (orig_request->extacl_user.size() && orig_request->extacl_passwd.size()) {
                char loginbuf[256];
                snprintf(loginbuf, sizeof(loginbuf), "%s:%s", orig_request->extacl_user.buf(), orig_request->extacl_passwd.buf());
                httpHeaderPutStrf(hdr_out, HDR_AUTHORIZATION, "Basic %s",
                                  base64_encode(loginbuf));
            }
        } else if (*orig_request->peer_login == '*') {
            /* Special mode, to pass the username to the upstream cache */
            char loginbuf[256];
            const char *username = "-";

            if (orig_request->auth_user_request)
                username = orig_request->auth_user_request->username();
            else if (orig_request->extacl_user.size())
                username = orig_request->extacl_user.buf();

            snprintf(loginbuf, sizeof(loginbuf), "%s%s", username, orig_request->peer_login + 1);

            httpHeaderPutStrf(hdr_out, HDR_AUTHORIZATION, "Basic %s",
                              base64_encode(loginbuf));
        } else {
            /* Fixed login string */
            httpHeaderPutStrf(hdr_out, HDR_AUTHORIZATION, "Basic %s",
                              base64_encode(orig_request->peer_login));
        }
    }

    /* append Cache-Control, add max-age if not there already */ {
        HttpHdrCc *cc = httpHeaderGetCc(hdr_in);

        if (!cc)
            cc = httpHdrCcCreate();

        if (!EBIT_TEST(cc->mask, CC_MAX_AGE)) {
            const char *url =
                entry ? storeUrl(entry) : urlCanonical(orig_request);
            httpHdrCcSetMaxAge(cc, getMaxAge(url));

            if (request->urlpath.size())
                assert(strstr(url, request->urlpath.buf()));
        }

        /* Set no-cache if determined needed but not found */
        if (orig_request->flags.nocache && !httpHeaderHas(hdr_in, HDR_PRAGMA))
            EBIT_SET(cc->mask, CC_NO_CACHE);

        /* Enforce sibling relations */
        if (flags.only_if_cached)
            EBIT_SET(cc->mask, CC_ONLY_IF_CACHED);

        httpHeaderPutCc(hdr_out, cc);

        httpHdrCcDestroy(cc);
    }

    /* maybe append Connection: keep-alive */
    if (flags.keepalive) {
        if (flags.proxying) {
            httpHeaderPutStr(hdr_out, HDR_PROXY_CONNECTION, "keep-alive");
        } else {
            httpHeaderPutStr(hdr_out, HDR_CONNECTION, "keep-alive");
        }
    }

    /* append Front-End-Https */
    if (flags.front_end_https) {
        if (flags.front_end_https == 1 || request->protocol == PROTO_HTTPS)
            httpHeaderPutStr(hdr_out, HDR_FRONT_END_HTTPS, "On");
    }

    /* Now mangle the headers. */
    if (Config2.onoff.mangle_request_headers)
        httpHdrMangleList(hdr_out, request, ROR_REQUEST);

    strConnection.clean();
}

void
copyOneHeaderFromClientsideRequestToUpstreamRequest(const HttpHeaderEntry *e, String strConnection, HttpRequest * request, HttpRequest * orig_request, HttpHeader * hdr_out, int we_do_ranges, http_state_flags flags)
{
    debug(11, 5) ("httpBuildRequestHeader: %s: %s\n",
                  e->name.buf(), e->value.buf());

    if (!httpRequestHdrAllowed(e, &strConnection)) {
        debug(11, 2) ("'%s' header denied by anonymize_headers configuration\n",+       e->name.buf());
        return;
    }

    switch (e->id) {

    case HDR_PROXY_AUTHORIZATION:
        /* Only pass on proxy authentication to peers for which
         * authentication forwarding is explicitly enabled
         */

        if (flags.proxying && orig_request->peer_login &&
                (strcmp(orig_request->peer_login, "PASS") == 0 ||
                 strcmp(orig_request->peer_login, "PROXYPASS") == 0)) {
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));
        }

        break;

    case HDR_AUTHORIZATION:
        /* Pass on WWW authentication */

        if (!flags.originpeer) {
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));
        } else {
            /* In accelerators, only forward authentication if enabled
             * (see also below for proxy->server authentication)
             */

            if (orig_request->peer_login &&
                    (strcmp(orig_request->peer_login, "PASS") == 0 ||
                     strcmp(orig_request->peer_login, "PROXYPASS") == 0)) {
                httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));
            }
        }

        break;

    case HDR_HOST:
        /*
         * Normally Squid rewrites the Host: header.
         * However, there is one case when we don't: If the URL
         * went through our redirector and the admin configured
         * 'redir_rewrites_host' to be off.
         */

        if (request->flags.redirected && !Config.onoff.redir_rewrites_host)
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));
        else {
            /* use port# only if not default */

            if (orig_request->port == urlDefaultPort(orig_request->protocol)) {
                httpHeaderPutStr(hdr_out, HDR_HOST, orig_request->host);
            } else {
                httpHeaderPutStrf(hdr_out, HDR_HOST, "%s:%d",
                                  orig_request->host, (int) orig_request->port);
            }
        }

        break;

    case HDR_IF_MODIFIED_SINCE:
        /* append unless we added our own;
         * note: at most one client's ims header can pass through */

        if (!httpHeaderHas(hdr_out, HDR_IF_MODIFIED_SINCE))
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));

        break;

    case HDR_MAX_FORWARDS:
        if (orig_request->method == METHOD_TRACE) {
            const int hops = httpHeaderEntryGetInt(e);

            if (hops > 0)
                httpHeaderPutInt(hdr_out, HDR_MAX_FORWARDS, hops - 1);
        }

        break;

    case HDR_VIA:
        /* If Via is disabled then forward any received header as-is */

        if (!Config.onoff.via)
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));

        break;

    case HDR_RANGE:

    case HDR_IF_RANGE:

    case HDR_REQUEST_RANGE:
        if (!we_do_ranges)
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));

        break;

    case HDR_PROXY_CONNECTION:

    case HDR_CONNECTION:

    case HDR_X_FORWARDED_FOR:

    case HDR_CACHE_CONTROL:
        /* append these after the loop if needed */
        break;

    case HDR_FRONT_END_HTTPS:
        if (!flags.front_end_https)
            httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));

        break;

    default:
        /* pass on all other header fields */
        httpHeaderAddEntry(hdr_out, httpHeaderEntryClone(e));
    }
}

bool
HttpStateData::decideIfWeDoRanges (HttpRequest * orig_request)
{
    bool result = true;
    /* decide if we want to do Ranges ourselves
     * and fetch the whole object now)
     * We want to handle Ranges ourselves iff
     *    - we can actually parse client Range specs
     *    - the specs are expected to be simple enough (e.g. no out-of-order ranges)
     *    - reply will be cachable
     * (If the reply will be uncachable we have to throw it away after
     *  serving this request, so it is better to forward ranges to
     *  the server and fetch only the requested content)
     */

    if (NULL == orig_request->range || !orig_request->flags.cachable
            || orig_request->range->offsetLimitExceeded())
        result = false;

    debug(11, 8) ("decideIfWeDoRanges: range specs: %p, cachable: %d; we_do_ranges: %d\n",
                  orig_request->range, orig_request->flags.cachable, result);

    return result;
}

/* build request prefix and append it to a given MemBuf;
 * return the length of the prefix */
mb_size_t
HttpStateData::buildRequestPrefix(HttpRequest * request,
                                  HttpRequest * orig_request,
                                  StoreEntry * entry,
                                  MemBuf * mb,
                                  http_state_flags flags)
{
    const int offset = mb->size;
    HttpVersion httpver(1, 0);
    mb->Printf("%s %s HTTP/%d.%d\r\n",
               RequestMethodStr[request->method],
               request->urlpath.size() ? request->urlpath.buf() : "/",
               httpver.major,httpver.minor);
    /* build and pack headers */
    {
        HttpHeader hdr(hoRequest);
        Packer p;
        httpBuildRequestHeader(request, orig_request, entry, &hdr, flags);
        packerToMemInit(&p, mb);
        httpHeaderPackInto(&hdr, &p);
        httpHeaderClean(&hdr);
        packerClean(&p);
    }
    /* append header terminator */
    mb->append(crlf, 2);
    return mb->size - offset;
}

/* This will be called when connect completes. Write request. */
void
HttpStateData::sendRequest()
{
    MemBuf mb;
    CWCB *sendHeaderDone;

    debug(11, 5) ("httpSendRequest: FD %d: this %p.\n", fd, this);

    commSetTimeout(fd, Config.Timeout.lifetime, httpTimeout, this);
    flags.do_next_read = 1;
    maybeReadData();

    if (orig_request->body_connection.getRaw() != NULL)
        sendHeaderDone = HttpStateData::SendRequestEntityWrapper;
    else
        sendHeaderDone = HttpStateData::SendComplete;

    if (_peer != NULL) {
        if (_peer->options.originserver) {
            flags.proxying = 0;
            flags.originpeer = 1;
        } else {
            flags.proxying = 1;
            flags.originpeer = 0;
        }
    } else {
        flags.proxying = 0;
        flags.originpeer = 0;
    }

    /*
     * Is keep-alive okay for all request methods?
     */
    if (!Config.onoff.server_pconns)
        flags.keepalive = 0;
    else if (_peer == NULL)
        flags.keepalive = 1;
    else if (_peer->stats.n_keepalives_sent < 10)
        flags.keepalive = 1;
    else if ((double) _peer->stats.n_keepalives_recv /
             (double) _peer->stats.n_keepalives_sent > 0.50)
        flags.keepalive = 1;

    if (_peer) {
        if (neighborType(_peer, request) == PEER_SIBLING &&
                !_peer->options.allow_miss)
            flags.only_if_cached = 1;

        flags.front_end_https = _peer->front_end_https;
    }

    mb.init();
    buildRequestPrefix(request, orig_request, entry, &mb, flags);
    debug(11, 6) ("httpSendRequest: FD %d:\n%s\n", fd, mb.buf);
    comm_old_write_mbuf(fd, &mb, sendHeaderDone, this);
}

void
httpStart(FwdState *fwd)
{
    debug(11, 3) ("httpStart: \"%s %s\"\n",
                  RequestMethodStr[fwd->request->method],
                  storeUrl(fwd->entry));
    HttpStateData *httpState = new HttpStateData(fwd);

    statCounter.server.all.requests++;

    statCounter.server.http.requests++;

    httpState->sendRequest();

    /*
     * We used to set the read timeout here, but not any more.
     * Now its set in httpSendComplete() after the full request,
     * including request body, has been written to the server.
     */
}

void
HttpStateData::SendRequestEntityDoneWrapper(int fd, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    httpState->sendRequestEntityDone(fd);
}

void
HttpStateData::sendRequestEntityDone(int fd)
{
    ACLChecklist ch;
    debug(11, 5) ("httpSendRequestEntityDone: FD %d\n", fd);
    ch.request = requestLink(request);

    if (Config.accessList.brokenPosts)
        ch.accessList = cbdataReference(Config.accessList.brokenPosts);

    /* cbdataReferenceDone() happens in either fastCheck() or ~ACLCheckList */

    if (!Config.accessList.brokenPosts) {
        debug(11, 5) ("httpSendRequestEntityDone: No brokenPosts list\n");
        HttpStateData::SendComplete(fd, NULL, 0, COMM_OK, this);
    } else if (!ch.fastCheck()) {
        debug(11, 5) ("httpSendRequestEntityDone: didn't match brokenPosts\n");
        HttpStateData::SendComplete(fd, NULL, 0, COMM_OK, this);
    } else {
        debug(11, 2) ("httpSendRequestEntityDone: matched brokenPosts\n");
        comm_old_write(fd, "\r\n", 2, HttpStateData::SendComplete, this, NULL);
    }
}

void
HttpStateData::RequestBodyHandlerWrapper(char *buf, ssize_t size, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    httpState->requestBodyHandler(buf, size);
}

void
HttpStateData::requestBodyHandler(char *buf, ssize_t size)
{
    request_body_buf = NULL;

    if (eof || fd < 0) {
        debugs(11, 1, HERE << "Transaction aborted while reading HTTP body");
        memFree8K(buf);
        return;
    }

    if (size > 0) {
        if (flags.headers_parsed && !flags.abuse_detected) {
            flags.abuse_detected = 1;
            debug(11, 1) ("httpSendRequestEntryDone: Likely proxy abuse detected '%s' -> '%s'\n",
                          inet_ntoa(orig_request->client_addr),
                          storeUrl(entry));

            if (getReply()->sline.status == HTTP_INVALID_HEADER) {
                memFree8K(buf);
                comm_close(fd);
                return;
            }
        }

        comm_old_write(fd, buf, size, SendRequestEntityWrapper, this, memFree8K);
    } else if (size == 0) {
        /* End of body */
        memFree8K(buf);
        sendRequestEntityDone(fd);
    } else {
        /* Failed to get whole body, probably aborted */
        memFree8K(buf);
        HttpStateData::SendComplete(fd, NULL, 0, COMM_ERR_CLOSING, this);
    }
}

void
HttpStateData::SendRequestEntityWrapper(int fd, char *bufnotused, size_t size, comm_err_t errflag, void *data)
{
    HttpStateData *httpState = static_cast<HttpStateData *>(data);
    httpState->sendRequestEntity(fd, size, errflag);
}

void
HttpStateData::sendRequestEntity(int fd, size_t size, comm_err_t errflag)
{
    debug(11, 5) ("httpSendRequestEntity: FD %d: size %d: errflag %d.\n",
                  fd, (int) size, errflag);

    if (size > 0) {
        fd_bytes(fd, size, FD_WRITE);
        kb_incr(&statCounter.server.all.kbytes_out, size);
        kb_incr(&statCounter.server.http.kbytes_out, size);
    }

    if (errflag == COMM_ERR_CLOSING)
        return;

    if (errflag) {
        ErrorState *err;
        err = errorCon(ERR_WRITE_ERROR, HTTP_BAD_GATEWAY);
        err->xerrno = errno;
        fwd->fail(err);
        comm_close(fd);
        return;
    }

    if (EBIT_TEST(entry->flags, ENTRY_ABORTED)) {
        comm_close(fd);
        return;
    }

    request_body_buf = (char *)memAllocate(MEM_8K_BUF);
    clientReadBody(orig_request, request_body_buf, 8192, RequestBodyHandlerWrapper, this);
}

void
httpBuildVersion(HttpVersion * version, unsigned int major, unsigned int minor)
{
    version->major = major;
    version->minor = minor;
}

#if ICAP_CLIENT

static void
icapAclCheckDoneWrapper(ICAPServiceRep::Pointer service, void *data)
{
    HttpStateData *http = (HttpStateData *)data;
    http->icapAclCheckDone(service);
}

void
HttpStateData::icapAclCheckDone(ICAPServiceRep::Pointer service)
{
    icapAccessCheckPending = false;

    if (service == NULL) {
        // handle case where no service is selected;
        storeEntryReplaceObject(entry, reply);

        /* Note storeEntryReplaceObject() consumes reply, so we cannot use it */
        reply = NULL;

        haveParsedReplyHeaders();
        processReplyBody();

        if (eof == 1)
            transactionComplete();

        return;
    }

    if (doIcap(service) < 0) {
        /*
         * XXX Maybe instead of an error page we should
         * handle the reply normally (without ICAP).
         */
        ErrorState *err = errorCon(ERR_ICAP_FAILURE, HTTP_INTERNAL_SERVER_ERROR);
        err->xerrno = errno;
        err->request = requestLink(orig_request);
        errorAppendEntry(entry, err);
        comm_close(fd);
        return;
    }

    icap->startRespMod(this, request, reply);
    processReplyBody();
}

/*
 * Initiate an ICAP transaction.  Return 0 if all is well, or -1 upon error.
 * Caller will handle error condition by generating a Squid error message
 * or take other action.
 */
int
HttpStateData::doIcap(ICAPServiceRep::Pointer service)
{
    debug(11,5)("HttpStateData::doIcap() called\n");
    assert(NULL == icap);
    icap = new ICAPClientRespmodPrecache(service);
    (void) cbdataReference(icap);
    return 0;
}

/*
 * Called by ICAPClientRespmodPrecache when it has space available for us.
 */
void
HttpStateData::icapSpaceAvailable()
{
    debug(11,5)("HttpStateData::icapSpaceAvailable() called\n");
    maybeReadData();
}

void
HttpStateData::takeAdaptedHeaders(HttpReply *rep)
{
    debug(11,5)("HttpStateData::takeAdaptedHeaders() called\n");

    if (!entry->isAccepting()) {
        debug(11,5)("\toops, entry is not Accepting!\n");
        icap->ownerAbort();
        return;
    }

    assert (rep);
    storeEntryReplaceObject(entry, rep);

    /*
     * After calling storeEntryReplaceObject() we give up control
     * of the rep and this->reply pointers.
     */
    rep = NULL;

    haveParsedReplyHeaders();

    debug(11,5)("HttpStateData::takeAdaptedHeaders() finished\n");
}

void
HttpStateData::takeAdaptedBody(MemBuf *buf)
{
    debug(11,5)("HttpStateData::takeAdaptedBody() called\n");
    debug(11,5)("\t%d bytes\n", (int) buf->contentSize());
    debug(11,5)("\t%d is current offset\n", (int)currentOffset);

    if (!entry->isAccepting()) {
        debug(11,5)("\toops, entry is not Accepting!\n");
        icap->ownerAbort();
        return;
    }

    entry->write(StoreIOBuffer(buf, currentOffset)); // write everything
    currentOffset += buf->contentSize();
    buf->consume(buf->contentSize()); // consume everything written
}

void
HttpStateData::doneAdapting()
{
    debug(11,5)("HttpStateData::doneAdapting() called\n");

    if (!entry->isAccepting()) {
        debug(11,5)("\toops, entry is not Accepting!\n");
        icap->ownerAbort();
    } else {
        fwd->complete();
    }

    /*
     * ICAP is done, so we don't need this any more.
     */
    delete icap;

    cbdataReferenceDone(icap);

    if (fd >= 0)
        comm_close(fd);
    else
        httpStateFree(fd, this);
}

void
HttpStateData::abortAdapting()
{
    debug(11,5)("HttpStateData::abortAdapting() called\n");

    /*
     * ICAP has given up, we're done with it too
     */
    delete icap;
    cbdataReferenceDone(icap);

    if (entry->isEmpty()) {
        ErrorState *err;
        err = errorCon(ERR_ICAP_FAILURE, HTTP_INTERNAL_SERVER_ERROR);
        err->request = requestLink((HttpRequest *) request);
        err->xerrno = errno;
        fwd->fail( err);
        fwd->dontRetry(true);
        flags.do_next_read = 0;
    }

    if (fd >= 0) {
        comm_close(fd);
    } else {
        httpStateFree(-1, this);	// deletes this
    }
}

#endif
