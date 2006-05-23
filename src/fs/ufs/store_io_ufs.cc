
/*
 * $Id: store_io_ufs.cc,v 1.31 2006/05/23 00:21:48 wessels Exp $
 *
 * DEBUG: section 79    Storage Manager UFS Interface
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
#include "Store.h"
#include "ufscommon.h"
#include "Generic.h"
#include "DiskIO/DiskFile.h"
#include "DiskIO/DiskIOStrategy.h"
#include "DiskIO/ReadRequest.h"
#include "DiskIO/WriteRequest.h"

#include "SwapDir.h"

bool
UFSStrategy::shedLoad()
{
    return io->shedLoad();
}

int
UFSStrategy::load()
{
    return io->load();
}

UFSStrategy::UFSStrategy (DiskIOStrategy *anIO) : io(anIO)
{}

UFSStrategy::~UFSStrategy ()
{
    delete io;
}

StoreIOState::Pointer
UFSStrategy::createState(SwapDir *SD, StoreEntry *e, StoreIOState::STIOCB * callback, void *callback_data) const
{
    return new UFSStoreState (SD, e, callback, callback_data);
}

DiskFile::Pointer
UFSStrategy::newFile (char const *path)
{
    return io->newFile(path);
}


void
UFSStrategy::unlinkFile(char const *path)
{
    io->unlinkFile(path);
}

CBDATA_CLASS_INIT(UFSStoreState);

void *
UFSStoreState::operator new (size_t)
{
    CBDATA_INIT_TYPE(UFSStoreState);
    return cbdataAlloc(UFSStoreState);
}

void
UFSStoreState::operator delete (void *address)
{
    cbdataFree(address);
}

void
UFSStoreState::ioCompletedNotification()
{
    if (opening) {
        opening = false;
        debug(79, 3) ("UFSStoreState::ioCompletedNotification: dirno %d, fileno %08x status %d\n",
                      swap_dirn, swap_filen, theFile->error());
        assert (FILE_MODE(mode) == O_RDONLY);
        openDone();

        return;
    }

    if (creating) {
        creating = false;
        debug(79, 3) ("UFSStoreState::ioCompletedNotification: dirno %d, fileno %08x status %d\n",
                      swap_dirn, swap_filen, theFile->error());
        openDone();

        return;
    }

    assert (!(closing ||opening));
    debug(79, 3) ("diskd::ioCompleted: dirno %d, fileno %08x status %d\n",                      swap_dirn, swap_filen, theFile->error());
    /* Ok, notification past open means an error has occured */
    assert (theFile->error());
    doCallback(DISK_ERROR);
}

void
UFSStoreState::openDone()
{
    if (theFile->error()) {
        doCallback(DISK_ERROR);
        return;
    }

    if (FILE_MODE(mode) == O_WRONLY) {
        if (kickWriteQueue())
            return;
    } else if ((FILE_MODE(mode) == O_RDONLY) && !closing) {
        if (kickReadQueue())
            return;
    }

    if (closing && !theFile->ioInProgress())
        doCallback(theFile->error() ? -1 : 0);

    debug(79, 3) ("squidaiostate_t::openDone: exiting\n");
}

void
UFSStoreState::closeCompleted()
{
    assert (closing);
    debug(79, 3) ("UFSStoreState::closeCompleted: dirno %d, fileno %08x status %d\n",
                  swap_dirn, swap_filen, theFile->error());

    if (theFile->error())
        doCallback(DISK_ERROR);
    else
        doCallback(DISK_OK);

    closing = false;
}

/* Close */
void
UFSStoreState::close()
{
    debug(79, 3) ("UFSStoreState::close: dirno %d, fileno %08X\n", swap_dirn,
                  swap_filen);
    /* mark the object to be closed on the next io that completes */
    closing = true;
    theFile->close();
}

void
UFSStoreState::read_(char *buf, size_t size, off_t offset, STRCB * callback, void *callback_data)
{
    assert(read.callback == NULL);
    assert(read.callback_data == NULL);
    assert(!reading);
    assert(!closing);
    assert (callback);

    if (!theFile->canRead()) {
        debug(79, 3) ("UFSStoreState::read_: queueing read because theFile can't read\n");
        queueRead (buf, size, offset, callback, callback_data);
        return;
    }

    read.callback = callback;
    read.callback_data = cbdataReference(callback_data);
    debug(79, 3) ("UFSStoreState::read_: dirno %d, fileno %08X\n",
                  swap_dirn, swap_filen);
    offset_ = offset;
    read_buf = buf;
    reading = true;
    theFile->read(new ReadRequest(buf,offset,size));
}


void
UFSStoreState::write(char const *buf, size_t size, off_t offset, FREE * free_func)
{
    debug(79, 3) ("UFSStoreState::write: dirn %d, fileno %08X\n", swap_dirn, swap_filen);

    if (!theFile->canWrite()) {
        assert(creating || writing);
        queueWrite(buf, size, offset, free_func);
        return;
    }

    writing = true;
    theFile->write(new WriteRequest(buf, offset, size, free_func));
}

void
UFSStoreState::readCompleted(const char *buf, int len, int errflag, RefCount<ReadRequest> result)
{
    assert (result.getRaw());
    reading = false;
    debug(79, 3) ("UFSStoreState::readCompleted: dirno %d, fileno %08x len %d\n",
                  swap_dirn, swap_filen, len);

    if (len > 0)
        offset_ += len;

    STRCB *callback = read.callback;

    assert(callback);

    read.callback = NULL;

    void *cbdata;

    /* A note:
     * diskd IO queues closes via the diskd queue. So close callbacks
     * occur strictly after reads and writes.
     * ufs doesn't queue, it simply completes, so close callbacks occur
     * strictly after reads and writes.
     * aufs performs closes syncronously, so close events must be managed
     * to force strict ordering.
     * The below does this:
     * closing is set when close() is called, and close only triggers
     * when no io's are pending.
     * writeCompleted likewise.
     */
    if (!closing && cbdataReferenceValidDone(read.callback_data, &cbdata)) {
        if (len > 0 && read_buf != buf)
            memcpy(read_buf, buf, len);

        callback(cbdata, read_buf, len);
    } else if (closing && theFile.getRaw()!= NULL && !theFile->ioInProgress())
        doCallback(errflag);
}

void
UFSStoreState::writeCompleted(int errflag, size_t len, RefCount<WriteRequest> writeRequest)
{
    debug(79, 3) ("storeUfsWriteDone: dirno %d, fileno %08X, len %ld\n",
                  swap_dirn, swap_filen, (long int) len);
    writing = false;

    offset_ += len;

    if (theFile->error()) {
        doCallback(DISK_ERROR);
        return;
    }

    if (closing && !theFile->ioInProgress()) {
        theFile->close();
        return;
    }

    if (!flags.write_kicking) {
        flags.write_kicking = true;
        /* While we start and complete syncronously io's. */

        while (kickWriteQueue() && !theFile->ioInProgress())

            ;
        flags.write_kicking = false;

        if (!theFile->ioInProgress() && closing)
            doCallback(errflag);
    }
}

void
UFSStoreState::doCallback(int errflag)
{
    debug(79, 3) ("storeUfsIOCallback: errflag=%d\n", errflag);
    STIOCB *theCallback = callback;
    callback = NULL;

    void *cbdata;

    if (cbdataReferenceValidDone(callback_data, &cbdata) && theCallback)
        theCallback(cbdata, errflag);

    /* We are finished with the file as this is on close or error only.*/
    /* This must be the last line, as theFile may be the only object holding
     * us in memory 
     */
    theFile = NULL;
}

/* ============= THE REAL UFS CODE ================ */

UFSStoreState::UFSStoreState(SwapDir * SD, StoreEntry * anEntry, STIOCB * callback_, void *callback_data_) : opening (false), creating (false), closing (false), reading(false), writing(false), pending_reads(NULL), pending_writes (NULL)
{
    swap_filen = anEntry->swap_filen;
    swap_dirn = SD->index;
    mode = O_BINARY;
    callback = callback_;
    callback_data = cbdataReference(callback_data_);
    e = anEntry;
    flags.write_kicking = false;
}

UFSStoreState::~UFSStoreState()
{
    _queued_read *qr;

    while ((qr = (_queued_read *)linklistShift(&pending_reads))) {
        cbdataReferenceDone(qr->callback_data);
        delete qr;
    }

    _queued_write *qw;

    while ((qw = (_queued_write *)linklistShift(&pending_writes))) {
        if (qw->free_func)
            qw->free_func(const_cast<char *>(qw->buf));
        delete qw;
    }
}

bool
UFSStoreState::kickReadQueue()
{
    _queued_read *q = (_queued_read *)linklistShift(&pending_reads);

    if (NULL == q)
        return false;

    debug(79, 3) ("UFSStoreState::kickReadQueue: reading queued request of %ld bytes\n",
                  (long int) q->size);

    void *cbdata;

    if (cbdataReferenceValidDone(q->callback_data, &cbdata))
        read_(q->buf, q->size, q->offset, q->callback, cbdata);

    delete q;

    return true;
}

void
UFSStoreState::queueRead(char *buf, size_t size, off_t offset, STRCB *callback, void *callback_data)
{
    debug(79, 3) ("UFSStoreState::queueRead: queueing read\n");
    assert(opening);
    assert (pending_reads == NULL);
    _queued_read *q = new _queued_read;
    q->buf = buf;
    q->size = size;
    q->offset = offset;
    q->callback = callback;
    q->callback_data = cbdataReference(callback_data);
    linklistPush(&pending_reads, q);
}

bool
UFSStoreState::kickWriteQueue()
{
    _queued_write *q = (_queued_write *)linklistShift(&pending_writes);

    if (NULL == q)
        return false;

    debug(79, 3) ("storeAufsKickWriteQueue: writing queued chunk of %ld bytes\n",
                  (long int) q->size);

    write(const_cast<char *>(q->buf), q->size, q->offset, q->free_func);
    delete q;
    return true;
}

void
UFSStoreState::queueWrite(char const *buf, size_t size, off_t offset, FREE * free_func)
{
    debug(79, 3) ("UFSStoreState::queueWrite: queuing write\n");

    _queued_write *q;
    q = new _queued_write;
    q->buf = buf;
    q->size = size;
    q->offset = offset;
    q->free_func = free_func;
    linklistPush(&pending_writes, q);
}

StoreIOState::Pointer
UFSStrategy::open(SwapDir * SD, StoreEntry * e, StoreIOState::STFNCB * file_callback,
                  StoreIOState::STIOCB * callback, void *callback_data)
{
    assert (((UFSSwapDir *)SD)->IO == this);
    debug(79, 3) ("UFSStrategy::open: fileno %08X\n", e->swap_filen);

    /* to consider: make createstate a private UFSStrategy call */
    StoreIOState::Pointer sio = createState (SD, e, callback, callback_data);

    sio->mode |= O_RDONLY;

    UFSStoreState *state = dynamic_cast <UFSStoreState *>(sio.getRaw());

    assert (state);

    char *path = ((UFSSwapDir *)SD)->fullPath(e->swap_filen, NULL);

    DiskFile::Pointer myFile = newFile (path);

    if (myFile.getRaw() == NULL)
        return NULL;

    state->theFile = myFile;

    state->opening = true;

    myFile->open (sio->mode, 0644, state);

    if (myFile->error())
        return NULL;

    return sio;
}

StoreIOState::Pointer
UFSStrategy::create(SwapDir * SD, StoreEntry * e, StoreIOState::STFNCB * file_callback,
                    StoreIOState::STIOCB * callback, void *callback_data)
{
    assert (((UFSSwapDir *)SD)->IO == this);
    /* Allocate a number */
    sfileno filn = ((UFSSwapDir *)SD)->mapBitAllocate();
    debug(79, 3) ("UFSStrategy::create: fileno %08X\n", filn);

    /* Shouldn't we handle a 'bitmap full' error here? */

    StoreIOState::Pointer sio = createState (SD, e, callback, callback_data);

    sio->mode |= O_WRONLY | O_CREAT | O_TRUNC;

    sio->swap_filen = filn;

    UFSStoreState *state = dynamic_cast <UFSStoreState *>(sio.getRaw());

    assert (state);

    char *path = ((UFSSwapDir *)SD)->fullPath(filn, NULL);

    DiskFile::Pointer myFile = newFile (path);

    if (myFile.getRaw() == NULL) {
        ((UFSSwapDir *)SD)->mapBitReset (filn);
        return NULL;
    }

    state->theFile = myFile;

    state->creating = true;

    myFile->create (state->mode, 0644, state);

    if (myFile->error()) {
        ((UFSSwapDir *)SD)->mapBitReset (filn);
        return NULL;
    }

    /* now insert into the replacement policy */
    ((UFSSwapDir *)SD)->replacementAdd(e);

    return sio;
}

int
UFSStrategy::callback()
{
    return io->callback();
}

void
UFSStrategy::init()
{
    io->init();
}

void
UFSStrategy::sync()
{
    io->sync();
}

void
UFSStrategy::statfs(StoreEntry & sentry)const
{
    io->statfs(sentry);
}

