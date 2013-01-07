
/*
 * DEBUG: section 20    Storage Manager Swapout Functions
 * AUTHOR: Duane Wessels
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

#include "squid.h"
#include "cbdata.h"
#include "StoreClient.h"
#include "globals.h"
#include "Store.h"
/* FIXME: Abstract the use of this more */
#include "mem_node.h"
#include "MemObject.h"
#include "SquidConfig.h"
#include "SwapDir.h"
#include "StatCounters.h"
#include "store_log.h"
#include "swap_log_op.h"

static void storeSwapOutStart(StoreEntry * e);
static StoreIOState::STIOCB storeSwapOutFileClosed;
static StoreIOState::STFNCB storeSwapOutFileNotify;

// wrapper to cross C/C++ ABI boundary. xfree is extern "C" for libraries.
static void xfree_cppwrapper(void *x)
{
    xfree(x);
}

/* start swapping object to disk */
static void
storeSwapOutStart(StoreEntry * e)
{
    MemObject *mem = e->mem_obj;
    StoreIOState::Pointer sio;
    assert(mem);
    /* Build the swap metadata, so the filesystem will know how much
     * metadata there is to store
     */
    debugs(20, 5, "storeSwapOutStart: Begin SwapOut '" << e->url() << "' to dirno " <<
           e->swap_dirn << ", fileno " << std::hex << std::setw(8) << std::setfill('0') <<
           std::uppercase << e->swap_filen);
    e->swap_status = SWAPOUT_WRITING;
    /* If we start swapping out objects with OutOfBand Metadata,
     * then this code needs changing
     */

    /* TODO: make some sort of data,size refcounted immutable buffer
     * and stop fooling ourselves with "const char*" buffers.
     */

    // Create metadata now, possibly in vain: storeCreate needs swap_hdr_sz.
    const char *buf = e->getSerialisedMetaData ();
    assert(buf);

    /* Create the swap file */
    generic_cbdata *c = new generic_cbdata(e);
    sio = storeCreate(e, storeSwapOutFileNotify, storeSwapOutFileClosed, c);

    if (sio == NULL) {
        e->swap_status = SWAPOUT_NONE;
        mem->swapout.decision = MemObject::SwapOut::swImpossible;
        delete c;
        xfree((char*)buf);
        storeLog(STORE_LOG_SWAPOUTFAIL, e);
        return;
    }

    mem->swapout.sio = sio;
    /* Don't lock until after create, or the replacement
     * code might get confused */

    e->lock();
    /* Pick up the file number if it was assigned immediately */
    e->swap_filen = mem->swapout.sio->swap_filen;

    e->swap_dirn = mem->swapout.sio->swap_dirn;

    /* write out the swap metadata */
    storeIOWrite(mem->swapout.sio, buf, mem->swap_hdr_sz, 0, xfree_cppwrapper);
}

static void
storeSwapOutFileNotify(void *data, int errflag, StoreIOState::Pointer self)
{
    generic_cbdata *c = (generic_cbdata *)data;
    StoreEntry *e = (StoreEntry *)c->data;
    MemObject *mem = e->mem_obj;
    assert(e->swap_status == SWAPOUT_WRITING);
    assert(mem);
    assert(mem->swapout.sio == self);
    assert(errflag == 0);
    assert(e->swap_filen < 0); // if this fails, call SwapDir::disconnect(e)
    e->swap_filen = mem->swapout.sio->swap_filen;
    e->swap_dirn = mem->swapout.sio->swap_dirn;
}

static bool
doPages(StoreEntry *anEntry)
{
    MemObject *mem = anEntry->mem_obj;

    do {
        // find the page containing the first byte we have not swapped out yet
        mem_node *page =
            mem->data_hdr.getBlockContainingLocation(mem->swapout.queue_offset);

        if (!page)
            break; // wait for more data to become available

        // memNodeWriteComplete() and absence of buffer offset math below
        // imply that we always write from the very beginning of the page
        assert(page->start() == mem->swapout.queue_offset);

        /*
         * Get the length of this buffer. We are assuming(!) that the buffer
         * length won't change on this buffer, or things are going to be very
         * strange. I think that after the copy to a buffer is done, the buffer
         * size should stay fixed regardless so that this code isn't confused,
         * but we can look at this at a later date or whenever the code results
         * in bad swapouts, whichever happens first. :-)
         */
        ssize_t swap_buf_len = page->nodeBuffer.length;

        debugs(20, 3, "storeSwapOut: swap_buf_len = " << swap_buf_len);

        assert(swap_buf_len > 0);

        debugs(20, 3, "storeSwapOut: swapping out " << swap_buf_len << " bytes from " << mem->swapout.queue_offset);

        mem->swapout.queue_offset += swap_buf_len;

        // Quit if write() fails. Sio is going to call our callback, and that
        // will cleanup, but, depending on the fs, that call may be async.
        const bool ok = mem->swapout.sio->write(
                     mem->data_hdr.NodeGet(page),
                     swap_buf_len,
                     -1,
                     memNodeWriteComplete);

        if (!ok || anEntry->swap_status != SWAPOUT_WRITING)
            return false;

        int64_t swapout_size = mem->endOffset() - mem->swapout.queue_offset;

        if (anEntry->store_status == STORE_PENDING)
            if (swapout_size < SM_PAGE_SIZE)
                break;

        if (swapout_size <= 0)
            break;
    } while (true);

    // either wait for more data or call swapOutFileClose()
    return true;
}

/* This routine is called every time data is sent to the client side.
 * It's overhead is therefor, significant.
 */
void
StoreEntry::swapOut()
{
    if (!mem_obj)
        return;

    // this flag may change so we must check even if we are swappingOut
    if (EBIT_TEST(flags, ENTRY_ABORTED)) {
        assert(EBIT_TEST(flags, RELEASE_REQUEST));
        // StoreEntry::abort() already closed the swap out file, if any
        // no trimming: data producer must stop production if ENTRY_ABORTED
        return;
    }

    const bool weAreOrMayBeSwappingOut = swappingOut() || mayStartSwapOut();

    Store::Root().maybeTrimMemory(*this, weAreOrMayBeSwappingOut);

    if (!weAreOrMayBeSwappingOut)
        return; // nothing else to do

    // Aborted entries have STORE_OK, but swapoutPossible rejects them. Thus,
    // store_status == STORE_OK below means we got everything we wanted.

    debugs(20, 7, HERE << "storeSwapOut: mem->inmem_lo = " << mem_obj->inmem_lo);
    debugs(20, 7, HERE << "storeSwapOut: mem->endOffset() = " << mem_obj->endOffset());
    debugs(20, 7, HERE << "storeSwapOut: swapout.queue_offset = " << mem_obj->swapout.queue_offset);

    if (mem_obj->swapout.sio != NULL)
        debugs(20, 7, "storeSwapOut: storeOffset() = " << mem_obj->swapout.sio->offset()  );

    int64_t const lowest_offset = mem_obj->lowestMemReaderOffset();

    debugs(20, 7, HERE << "storeSwapOut: lowest_offset = " << lowest_offset);

#if SIZEOF_OFF_T <= 4

    if (mem_obj->endOffset() > 0x7FFF0000) {
        debugs(20, DBG_CRITICAL, "WARNING: preventing off_t overflow for " << url());
        abort();
        return;
    }

#endif
    if (swap_status == SWAPOUT_WRITING)
        assert(mem_obj->inmem_lo <=  mem_obj->objectBytesOnDisk() );

    // buffered bytes we have not swapped out yet
    const int64_t swapout_maxsize = mem_obj->availableForSwapOut();
    assert(swapout_maxsize >= 0);
    debugs(20, 7, "storeSwapOut: swapout_size = " << swapout_maxsize);

    if (swapout_maxsize == 0) { // swapped everything we got
        if (store_status == STORE_OK) { // got everything we wanted
            assert(mem_obj->object_sz >= 0);
            swapOutFileClose(StoreIOState::wroteAll);
        }
        // else need more data to swap out
        return;
    }

    if (store_status == STORE_PENDING) {
        /* wait for a full block to write */

        if (swapout_maxsize < SM_PAGE_SIZE)
            return;

        /*
         * Wait until we are below the disk FD limit, only if the
         * next server-side read won't be deferred.
         */
        if (storeTooManyDiskFilesOpen() && !checkDeferRead(-1))
            return;
    }

    /* Ok, we have stuff to swap out.  Is there a swapout.sio open? */
    if (swap_status == SWAPOUT_NONE) {
        assert(mem_obj->swapout.sio == NULL);
        assert(mem_obj->inmem_lo == 0);
        storeSwapOutStart(this); // sets SwapOut::swImpossible on failures
    }

    if (mem_obj->swapout.sio == NULL)
        return;

    if (!doPages(this))
        /* oops, we're not swapping out any more */
        return;

    if (store_status == STORE_OK) {
        /*
         * If the state is STORE_OK, then all data must have been given
         * to the filesystem at this point because storeSwapOut() is
         * not going to be called again for this entry.
         */
        assert(mem_obj->object_sz >= 0);
        assert(mem_obj->endOffset() == mem_obj->swapout.queue_offset);
        swapOutFileClose(StoreIOState::wroteAll);
    }
}

void
StoreEntry::swapOutFileClose(int how)
{
    assert(mem_obj != NULL);
    debugs(20, 3, "storeSwapOutFileClose: " << getMD5Text() << " how=" << how);
    debugs(20, 3, "storeSwapOutFileClose: sio = " << mem_obj->swapout.sio.getRaw());

    if (mem_obj->swapout.sio == NULL)
        return;

    storeClose(mem_obj->swapout.sio, how);
}

static void
storeSwapOutFileClosed(void *data, int errflag, StoreIOState::Pointer self)
{
    generic_cbdata *c = (generic_cbdata *)data;
    StoreEntry *e = (StoreEntry *)c->data;
    MemObject *mem = e->mem_obj;
    assert(mem->swapout.sio == self);
    assert(e->swap_status == SWAPOUT_WRITING);
    cbdataFree(c);

    // if object_size is still unknown, the entry was probably aborted
    if (errflag || e->objectLen() < 0) {
        debugs(20, 2, "storeSwapOutFileClosed: dirno " << e->swap_dirn << ", swapfile " <<
               std::hex << std::setw(8) << std::setfill('0') << std::uppercase <<
               e->swap_filen << ", errflag=" << errflag);

        if (errflag == DISK_NO_SPACE_LEFT) {
            /* FIXME: this should be handle by the link from store IO to
             * Store, rather than being a top level API call.
             */
            e->store()->diskFull();
            storeConfigure();
        }

        if (e->swap_filen >= 0)
            e->unlink();

        assert(e->swap_status == SWAPOUT_NONE);

        e->releaseRequest();
    } else {
        /* swapping complete */
        debugs(20, 3, "storeSwapOutFileClosed: SwapOut complete: '" << e->url() << "' to " <<
               e->swap_dirn  << ", " << std::hex << std::setw(8) << std::setfill('0') <<
               std::uppercase << e->swap_filen);
        debugs(20, 5, HERE << "swap_file_sz = " <<
               e->objectLen() << " + " << mem->swap_hdr_sz);

        e->swap_file_sz = e->objectLen() + mem->swap_hdr_sz;
        e->swap_status = SWAPOUT_DONE;
        e->store()->swappedOut(*e);

        // XXX: For some Stores, it is pointless to re-check cachability here
        // and it leads to double counts in store_check_cachable_hist. We need
        // another way to signal a completed but failed swapout. Or, better,
        // each Store should handle its own logging and LOG state setting.
        if (e->checkCachable()) {
            storeLog(STORE_LOG_SWAPOUT, e);
            storeDirSwapLog(e, SWAP_LOG_ADD);
        }

        ++statCounter.swap.outs;
    }

    debugs(20, 3, "storeSwapOutFileClosed: " << __FILE__ << ":" << __LINE__);
    mem->swapout.sio = NULL;
    e->unlock();
}

bool
StoreEntry::mayStartSwapOut()
{
    dlink_node *node;

    // must be checked in the caller
    assert(!EBIT_TEST(flags, ENTRY_ABORTED));
    assert(!swappingOut());

    if (!Config.cacheSwap.n_configured)
        return false;

    assert(mem_obj);
    MemObject::SwapOut::Decision &decision = mem_obj->swapout.decision;

    // if we decided that swapout is not possible, do not repeat same checks
    if (decision == MemObject::SwapOut::swImpossible) {
        debugs(20, 3, HERE << " already rejected");
        return false;
    }

    // if we decided that swapout is possible, do not repeat same checks
    if (decision == MemObject::SwapOut::swPossible) {
        debugs(20, 3,  HERE << "already allowed");
        return true;
    }

    // if we swapped out already, do not start over
    if (swap_status == SWAPOUT_DONE) {
        debugs(20, 3,  HERE << "already did");
        decision = MemObject::SwapOut::swImpossible;
        return false;
    }

    if (!checkCachable()) {
        debugs(20, 3,  HERE << "not cachable");
        decision = MemObject::SwapOut::swImpossible;
        return false;
    }

    if (EBIT_TEST(flags, ENTRY_SPECIAL)) {
        debugs(20, 3,  HERE  << url() << " SPECIAL");
        decision = MemObject::SwapOut::swImpossible;
        return false;
    }

    // check cache_dir max-size limit if all cache_dirs have it
    if (store_maxobjsize >= 0) {
        // TODO: add estimated store metadata size to be conservative

        // use guaranteed maximum if it is known
        const int64_t expectedEnd = mem_obj->expectedReplySize();
        debugs(20, 7,  HERE << "expectedEnd = " << expectedEnd);
        if (expectedEnd > store_maxobjsize) {
            debugs(20, 3,  HERE << "will not fit: " << expectedEnd <<
                   " > " << store_maxobjsize);
            decision = MemObject::SwapOut::swImpossible;
            return false; // known to outgrow the limit eventually
        }

        // use current minimum (always known)
        const int64_t currentEnd = mem_obj->endOffset();
        if (currentEnd > store_maxobjsize) {
            debugs(20, 3,  HERE << "does not fit: " << currentEnd <<
                   " > " << store_maxobjsize);
            decision = MemObject::SwapOut::swImpossible;
            return false; // already does not fit and may only get bigger
        }

        // prevent default swPossible answer for yet unknown length
        if (expectedEnd < 0) {
            debugs(20, 3,  HERE << "wait for more info: " <<
                   store_maxobjsize);
            return false; // may fit later, but will be rejected now
        }

        if (store_status != STORE_OK) {
            const int64_t maxKnownSize = expectedEnd < 0 ?
                                         mem_obj->availableForSwapOut() : expectedEnd;
            debugs(20, 7, HERE << "maxKnownSize= " << maxKnownSize);
            if (maxKnownSize < store_maxobjsize) {
                /*
                 * NOTE: the store_maxobjsize here is the max of optional
                 * max-size values from 'cache_dir' lines.  It is not the
                 * same as 'maximum_object_size'.  By default, store_maxobjsize
                 * will be set to -1.  However, I am worried that this
                 * deferance may consume a lot of memory in some cases.
                 * Should we add an option to limit this memory consumption?
                 */
                debugs(20, 5,  HERE << "Deferring swapout start for " <<
                       (store_maxobjsize - maxKnownSize) << " bytes");
                return false;
            }
        }
    }

    if (mem_obj->inmem_lo > 0) {
        debugs(20, 3, "storeSwapOut: (inmem_lo > 0)  imem_lo:" <<  mem_obj->inmem_lo);
        decision = MemObject::SwapOut::swImpossible;
        return false;
    }

    /*
     * If there are DISK clients, we must write to disk
     * even if its not cachable
     * RBC: Surely we should not create disk client on non cacheable objects?
     * therefore this should be an assert?
     * RBC 20030708: We can use disk to avoid mem races, so this shouldn't be
     * an assert.
     *
     * XXX: Not clear what "mem races" the above refers to, especially when
     * dealing with non-cachable objects that cannot have multiple clients.
     *
     * XXX: If STORE_DISK_CLIENT needs SwapOut::swPossible, we have to check
     * for that flag earlier, but forcing swapping may contradict max-size or
     * other swapability restrictions. Change storeClientType() and/or its
     * callers to take swap-in availability into account.
     */
    for (node = mem_obj->clients.head; node; node = node->next) {
        if (((store_client *) node->data)->getType() == STORE_DISK_CLIENT) {
            debugs(20, 3, HERE << "DISK client found");
            decision = MemObject::SwapOut::swPossible;
            return true;
        }
    }

    if (!mem_obj->isContiguous()) {
        debugs(20, 3, "storeSwapOut: not Contiguous");
        decision = MemObject::SwapOut::swImpossible;
        return false;
    }

    decision = MemObject::SwapOut::swPossible;
    return true;
}
