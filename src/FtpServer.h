/*
 * DEBUG: section 09    File Transfer Protocol (FTP)
 *
 */

#ifndef SQUID_FTP_SERVER_H
#define SQUID_FTP_SERVER_H

#include "Server.h"

namespace Ftp {

extern const char *const crlf;

/// common code for FTP server control and data channels
/// does not own the channel descriptor, which is managed by FtpStateData
class ServerChannel
{
public:
    /// called after the socket is opened, sets up close handler
    void opened(const Comm::ConnectionPointer &conn, const AsyncCall::Pointer &aCloser);

    /** Handles all operations needed to properly close the active channel FD.
     * clearing the close handler, clearing the listen socket properly, and calling comm_close
     */
    void close();

    void forget(); /// remove the close handler, leave connection open

    void clear(); ///< just drops conn and close handler. does not close active connections.

    Comm::ConnectionPointer conn; ///< channel descriptor

    /** A temporary handle to the connection being listened on.
     * Closing this will also close the waiting Data channel acceptor.
     * If a data connection has already been accepted but is still waiting in the event queue
     * the callback will still happen and needs to be handled (usually dropped).
     */
    Comm::ConnectionPointer listenConn;

    AsyncCall::Pointer opener; ///< Comm opener handler callback.
private:
    AsyncCall::Pointer closer; ///< Comm close handler callback
};

/// Base class for FTP over HTTP and FTP Gateway server state.
class ServerStateData: public ::ServerStateData
{
public:
    ServerStateData(FwdState *fwdState);
    virtual ~ServerStateData();

    virtual void failed(err_type error = ERR_NONE, int xerrno = 0);
    virtual void timeout(const CommTimeoutCbParams &io);
    virtual const Comm::ConnectionPointer & dataConnection() const;
    virtual void abortTransaction(const char *reason);
    void writeCommand(const char *buf);
    bool handlePasvReply();
    void connectDataChannel();
    virtual void maybeReadVirginBody();
    void switchTimeoutToDataChannel();

    // \todo: optimize ctrl and data structs member order, to minimize size
    /// FTP control channel info; the channel is opened once per transaction
    struct CtrlChannel: public ServerChannel {
        char *buf;
        size_t size;
        size_t offset;
        wordlist *message;
        char *last_command;
        char *last_reply;
        int replycode;
    } ctrl;

    /// FTP data channel info; the channel may be opened/closed a few times
    struct DataChannel: public ServerChannel {
        MemBuf *readBuf;
        Ip::Address addr;
        bool read_pending;
    } data;

    int state;
    char *old_request;
    char *old_reply;

protected:
    virtual void start();

    void initReadBuf();
    virtual void closeServer();
    virtual bool doneWithServer() const;
    virtual void failedErrorMessage(err_type error, int xerrno);
    virtual Http::StatusCode failedHttpStatus(err_type &error);
    void ctrlClosed(const CommCloseCbParams &io);
    void scheduleReadControlReply(int buffered_ok);
    void readControlReply(const CommIoCbParams &io);
    virtual void handleControlReply();
    void writeCommandCallback(const CommIoCbParams &io);
    static CNCB dataChannelConnected;
    virtual void dataChannelConnected(const Comm::ConnectionPointer &conn, comm_err_t status, int xerrno) = 0;
    void dataRead(const CommIoCbParams &io);
    void dataComplete();
    AsyncCall::Pointer dataCloser();
    virtual void dataClosed(const CommCloseCbParams &io);

    // sending of the request body to the server
    virtual void sentRequestBody(const CommIoCbParams &io);
    virtual void doneSendingRequestBody();

private:
    static wordlist *parseControlReply(char *buf, size_t len, int *codep, size_t *used);

    CBDATA_CLASS2(ServerStateData);
};

}; // namespace Ftp

#endif /* SQUID_FTP_SERVER_H */
