//
// FILE            corRestCor.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// cor:// - see corRestCor.h, and coraine's doc/cor-protocol.md § 5.
//
// A frame: a 16-byte header, then one tree in the cor format.
//
//   magic        4   C0 4F 52 01 - 0xC0 'O' 'R' and the format version
//   type         1   HELLO, HELLO_ACK, REQUEST, RESPONSE, ...
//   flags        1   0
//   reserved     2   0
//   correlation  4   little-endian - a REQUEST's, copied into its RESPONSE
//   length       4   little-endian - the tree's bytes
//
// A request:   { "verb": "GET", "path": "/ngsi-ld/v1/entities/urn:E1?options=keyValues",
//                "headers": { "Link": "...", ... }, "body": <tree> }
// A response:  { "status": 200, "headers": { ... }, "body": <tree> }      ("text": "..." for a body that is no tree)
//
// HELLO / HELLO_ACK: { "version": 1, "terms": <core-term table size>, "namespaces": <fixed namespaces> }, sent
// with no codec and no tables - before them there is nothing to agree on. After it, each direction of the
// connection has its own tables, preloaded with the fixed namespaces both sides have, and kept for the life
// of the connection.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint8_t, uint32_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // memcpy, memcmp, strcmp, strchr, strncmp
#include <errno.h>                                    // errno, EINTR
#include <unistd.h>                                   // close, read, write
#include <time.h>                                     // clock_gettime
#include <poll.h>                                     // poll
#include <pthread.h>                                  // pthread_create
#include <netdb.h>                                    // getaddrinfo
#include <sys/socket.h>                               // socket, setsockopt, accept, bind, listen
#include <netinet/in.h>                               // sockaddr_in6
#include <netinet/tcp.h>                              // TCP_NODELAY
#include <sys/epoll.h>                                // epoll_create1, epoll_ctl, epoll_wait
#include <fcntl.h>                                    // fcntl, O_NONBLOCK
#include <stdatomic.h>                                // _Atomic, atomic_fetch_add

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc, corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corAlloc/corAllocAdopt.h"                   // corAllocAdopt
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBin.h"                       // corTreeBinEncode, corTreeBinDecode
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse

#include "corRest/corRest.h"                          // corRest, corRestP
#include "corRest/CorRestState.h"                     // CorRestState
#include "corRest/corRestHooks.h"                     // CorRestHook, CorRestUserData*Hook
#include "corRest/corRestStateInit.h"                 // corRestStateInit, corRestStateRelease, corRestUrlPathNormalize
#include "corRest/corRestUrlValueEncode.h"            // corRestUrlValueDecode
#include "corRest/corRestBackend.h"                   // corRestHttpHeaderAdd, corRestUriParamsParse, corRestProcessRequest, corRestResponseHeaderVBuild
#include "corRest/corRestCor.h"                       // Own interface

extern CorRestUserDataAllocHook  corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook   corRestUserDataFreeHookF;
extern CorRestHook               corRestPostResponseHook;
extern CorRestFinishInlineHook   corRestFinishInlineHookF;



// -----------------------------------------------------------------------------
//
// The frame
//
enum
{
  FRAME_HELLO       = 1,
  FRAME_HELLO_ACK   = 2,
  FRAME_REQUEST     = 3,
  FRAME_RESPONSE    = 4,
  FRAME_ERROR       = 7
};

static const uint8_t frameMagic[4] = { 0xC0, 'O', 'R', 1 };

enum
{
  FRAME_HEADER_LEN  = 16,
  FRAME_MAX         = 64 * 1024 * 1024,         // a tree larger than this is refused
  NAMESPACES_MAX    = 1024,                     // the connection tables' caps - both ends use the same
  NAMES_MAX         = 8192
};



// -----------------------------------------------------------------------------
//
// The codec, set once by the application
//
static const CorBinCodec*  codecP       = NULL;
static const char**        namespaceV   = NULL;
static int                 namespaces   = 0;
static int                 termCount    = 0;



// -----------------------------------------------------------------------------
//
// CorConn - one cor:// connection, either end
//
typedef struct CorConn
{
  int           fd;
  CorBinTables  out;            // what this end has defined
  CorBinTables  in;             // the mirror of what the peer has defined
  bool          tables;         // both initialised
  uint32_t      correlation;
  char          rbuf[16 * 1024];  // what the socket gave beyond what has been consumed - one read() usually brings a whole frame
  int           rpos;
  int           rlen;
  char          peer[160];      // host:port, for the client cache and the log - host (128) + port (16)
} CorConn;



// -----------------------------------------------------------------------------
//
// corRestCorInit -
//
void corRestCorInit(const CorBinCodec* _codecP, const char** _namespaceV, int _namespaces, int _termCount)
{
  codecP     = _codecP;
  namespaceV = _namespaceV;
  namespaces = _namespaces;
  termCount  = _termCount;
}



// -----------------------------------------------------------------------------
//
// ioWait - until fd is readable/writable or timeoutMs passes
//
static bool ioWait(int fd, short events, int timeoutMs)
{
  struct pollfd p = { fd, events, 0 };

  while (true)
  {
    int r = poll(&p, 1, timeoutMs);

    if (r > 0)
      return true;
    if ((r < 0) && (errno == EINTR))
      continue;
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// writeAll -
//
static bool writeAll(int fd, const void* p, int n, int timeoutMs)
{
  const char* cP = p;

  (void) timeoutMs;                                  // blocking socket: send() waits - SO_SNDTIMEO bounds it (clientConnect)

  while (n > 0)
  {
    ssize_t w = send(fd, cP, n, MSG_NOSIGNAL);
    if (w < 0)
    {
      if (errno == EINTR)
        continue;

      //
      // A server socket is non-blocking: a full send buffer waits here, briefly - a v1 simplification
      //
      if (((errno == EAGAIN) || (errno == EWOULDBLOCK)) && (ioWait(fd, POLLOUT, 30000) == true))
        continue;

      return false;
    }

    cP += w;
    n  -= w;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// connRead - exactly n bytes, from the connection's buffer first and the socket after
//
// One read() asks for as much as the buffer holds, so a frame - header and tree - normally arrives in
// a single system call. poll() only when there is nothing buffered AND a timeout applies; timeoutMs < 0
// (a server waiting for its next request) is a plain blocking read.
//
static bool connRead(CorConn* cP, void* p, int n, int timeoutMs)
{
  char* outP = p;

  while (n > 0)
  {
    int avail = cP->rlen - cP->rpos;

    if (avail > 0)
    {
      int take = (avail < n) ? avail : n;

      memcpy(outP, &cP->rbuf[cP->rpos], take);
      cP->rpos += take;
      outP     += take;
      n        -= take;
      continue;
    }

    if ((timeoutMs >= 0) && (ioWait(cP->fd, POLLIN, timeoutMs) == false))
    {
      errno = ETIMEDOUT;
      return false;
    }

    //
    // A large remainder goes straight to its destination; anything else through the buffer
    //
    ssize_t r = (n >= (int) sizeof(cP->rbuf)) ? read(cP->fd, outP, n) : read(cP->fd, cP->rbuf, sizeof(cP->rbuf));

    if (r < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }

    if (r == 0)                                      // the peer closed
    {
      errno = ECONNRESET;
      return false;
    }

    if (n >= (int) sizeof(cP->rbuf))
    {
      outP += r;
      n    -= r;
    }
    else
    {
      cP->rpos = 0;
      cP->rlen = (int) r;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// frameSend - a tree, encoded with the connection's outgoing tables (or none, for HELLO)
//
static bool frameSend(CorConn* cP, uint8_t type, uint32_t correlation, CorNode* treeP, int timeoutMs)
{
  CorBinBuffer frame = { NULL, 0, 0 };
  bool         plain = (type == FRAME_HELLO) || (type == FRAME_HELLO_ACK);
  uint8_t      header[FRAME_HEADER_LEN] = { 0 };

  //
  // Header and tree in ONE buffer, so one send(): the header's length is patched in after the encoding
  //
  frame.buf  = malloc(4096);
  frame.size = (frame.buf != NULL) ? 4096 : 0;

  if (frame.buf == NULL)
    return false;

  memcpy(frame.buf, header, FRAME_HEADER_LEN);
  frame.len = FRAME_HEADER_LEN;

  if (corTreeBinEncode(treeP, plain ? NULL : codecP, plain ? NULL : &cP->out, &frame) == false)
  {
    free(frame.buf);
    return false;
  }

  uint32_t bodyLen = frame.len - FRAME_HEADER_LEN;

  memcpy(frame.buf, frameMagic, 4);
  frame.buf[4] = type;
  memcpy(&frame.buf[8],  &correlation, 4);             // little-endian, as the hosts are
  memcpy(&frame.buf[12], &bodyLen,     4);

  bool ok = writeAll(cP->fd, frame.buf, frame.len, timeoutMs);

  free(frame.buf);
  return ok;
}



// -----------------------------------------------------------------------------
//
// frameRecv - a frame's header and its body; the body from allocP, or malloc'd when allocP is NULL
//
static bool frameRecv(CorConn* cP, uint8_t* typeP, uint32_t* correlationP, char** bufP, int* lenP, CorAlloc* allocP, int timeoutMs, const char** errorP)
{
  uint8_t header[FRAME_HEADER_LEN];

  if (connRead(cP, header, FRAME_HEADER_LEN, timeoutMs) == false)
  {
    *errorP = (errno == ETIMEDOUT) ? "timed out" : "connection closed";
    return false;
  }

  if (memcmp(header, frameMagic, 4) != 0)
  {
    *errorP = "not a cor:// frame (bad magic)";
    return false;
  }

  uint32_t len;

  memcpy(correlationP, &header[8],  4);
  memcpy(&len,         &header[12], 4);

  if (len > FRAME_MAX)
  {
    *errorP = "frame too large";
    return false;
  }

  char* buf = (allocP != NULL) ? corAlloc(allocP, len + 1) : malloc(len + 1);
  if (buf == NULL)
  {
    *errorP = "out of memory";
    return false;
  }

  if (connRead(cP, buf, len, timeoutMs) == false)
  {
    if (allocP == NULL)
      free(buf);
    *errorP = "connection closed in the middle of a frame";
    return false;
  }

  *typeP = header[4];
  *bufP  = buf;
  *lenP  = (int) len;

  return true;
}



// -----------------------------------------------------------------------------
//
// tablesOpen - after HELLO: both directions' tables, preloaded with the namespaces both ends have
//
static bool tablesOpen(CorConn* cP, int peerNamespaces)
{
  int preload = (peerNamespaces < namespaces) ? peerNamespaces : namespaces;

  if ((corTreeBinTablesInit(&cP->out, NAMESPACES_MAX, NAMES_MAX) == false) || (corTreeBinTablesInit(&cP->in, NAMESPACES_MAX, NAMES_MAX) == false))
    return false;

  cP->tables = true;

  return corTreeBinTablesPreload(&cP->out, namespaceV, preload) && corTreeBinTablesPreload(&cP->in, namespaceV, preload);
}



// -----------------------------------------------------------------------------
//
// connClose -
//
static void connClose(CorConn* cP)
{
  if (cP->fd >= 0)
    close(cP->fd);
  cP->fd   = -1;
  cP->rpos = 0;
  cP->rlen = 0;

  if (cP->tables == true)
  {
    corTreeBinTablesRelease(&cP->out);
    corTreeBinTablesRelease(&cP->in);
    cP->tables = false;
  }
}



// -----------------------------------------------------------------------------
//
// helloTree - { "version": 1, "terms": N, "namespaces": M }
//
static CorNode* helloTree(CorAlloc* kaP)
{
  CorNode* helloP = corTreeObject(kaP, NULL);

  corTreeChildAdd(helloP, corTreeInteger(kaP, "version",    1));
  corTreeChildAdd(helloP, corTreeInteger(kaP, "terms",      termCount));
  corTreeChildAdd(helloP, corTreeInteger(kaP, "namespaces", namespaces));

  return helloP;
}



// -----------------------------------------------------------------------------
//
// helloCheck - a peer's HELLO: same version, same term table; its namespace count into *namespacesP
//
static const char* helloCheck(CorNode* helloP, int* namespacesP)
{
  CorNode* versionP    = (helloP != NULL) ? corTreeLookup(helloP, "version")    : NULL;
  CorNode* termsP      = (helloP != NULL) ? corTreeLookup(helloP, "terms")      : NULL;
  CorNode* namespacesP_ = (helloP != NULL) ? corTreeLookup(helloP, "namespaces") : NULL;

  if ((versionP == NULL) || (versionP->type != CorInt) || (versionP->value.i != 1))
    return "unsupported cor:// version";

  //
  // v1: the term tables must be the same. Writing a term the peer does not have as a string is the
  // way to relax this - an encoder option, no format change.
  //
  if ((termsP == NULL) || (termsP->type != CorInt) || (termsP->value.i != termCount))
    return "the peer's core-term table differs from ours";

  *namespacesP = ((namespacesP_ != NULL) && (namespacesP_->type == CorInt)) ? (int) namespacesP_->value.i : 0;

  return NULL;
}



// =============================================================================
//
// SERVER
//
// Event loops, as HTTP has: a listener hands each connection to a loop in turn, and a loop reads
// requests off its connections with epoll. Requests are MULTIPLEXED (coraine doc/cor-protocol.md
// § 5.3): the loop goes on reading a connection while its earlier requests are still running, and each
// response goes back with its request's correlation id as soon as it is ready - in whatever order
// that is.
//
// A request that cannot block - the same check HTTP makes (corRestAsyncDispatch) - runs right there on
// the loop. One that can (a forward, waiting on its peer) is run by the thread that read it, which
// first hands the loop to another of the loop's threads (leader/follower - CorLoop): no thread hop on
// the request's way. A post-response phase that may block goes to the worker pool
// (CorRestState.finishF), as HTTP's does.
//
// What is the connection's, and what each request's:
//   - reading, and the incoming tables (decoding): the loop leader's alone. The connection is armed
//     EPOLLONESHOT, so one thread at a time reads it, and a frame is decoded as it arrives, in order.
//   - writing, and the outgoing tables (encoding): whoever responds, under the connection's write
//     lock - a frame is encoded and sent in one piece, so the peer decodes in the order it was encoded.
//   - the request frame's bytes, its decoded tree and its state: the request's (ServerReq).
//
// The connection lives as long as its reader or any request still needs it - a reference each.
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// ServerConn - a CorConn, plus what a server needs to assemble frames off a non-blocking socket
//
typedef struct ServerConn
{
  CorConn          conn;
  int              loopFd;           // the epoll instance the connection belongs to
  int              shard;            // the loop's index - the worker queue its requests go to
  bool             helloDone;
  uint8_t          hdr[FRAME_HEADER_LEN];
  int              hdrHave;
  char*            body;             // the frame being assembled - a request takes it (frameTake)
  uint32_t         bodyLen;
  uint32_t         bodyHave;
  uint32_t         correlation;
  pthread_mutex_t  writeMutex;       // a response is encoded and sent in one piece
  _Atomic int      refs;             // the reader's, and one per request in flight
  _Atomic bool     dead;             // the peer is gone, or the stream broke: responses still running are dropped
  _Atomic int      reader;           // who reads it now: READER_NONE (armed - the loop, next), _LOOP, _THREAD
} ServerConn;



// -----------------------------------------------------------------------------
//
// ServerReq - one request in flight: its state, and the frame its decoded tree points into
//
typedef struct ServerReq
{
  CorRestState  state;              // the request's corRest state - its 'connection' is this ServerReq
  ServerConn*   scP;
  uint32_t      correlation;
  char*         body;               // the request frame
  CorAlloc      ka;                 // the decoded request tree
  char          kaBuf[8 * 1024];
} ServerReq;



// -----------------------------------------------------------------------------
//
// connArm - read the next frame when it comes
//
static void connArm(ServerConn* scP)
{
  struct epoll_event ev;

  ev.events   = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
  ev.data.ptr = scP;

  epoll_ctl(scP->loopFd, EPOLL_CTL_MOD, scP->conn.fd, &ev);
}



// -----------------------------------------------------------------------------
//
// connRelease - one reference less; the last one frees the connection
//
static void connRelease(ServerConn* scP)
{
  if (atomic_fetch_sub(&scP->refs, 1) != 1)
    return;

  connClose(&scP->conn);
  free(scP->body);
  pthread_mutex_destroy(&scP->writeMutex);
  free(scP);
}



// -----------------------------------------------------------------------------
//
// connDead - the loop stops reading: the peer is gone, or spoke nonsense
//
// The socket stays open until the last request still running is over (connRelease) - closed now, its
// number could be reused while a worker still writes to it.
//
static void connDead(ServerConn* scP)
{
  epoll_ctl(scP->loopFd, EPOLL_CTL_DEL, scP->conn.fd, NULL);
  scP->dead = true;
  shutdown(scP->conn.fd, SHUT_RDWR);
  connRelease(scP);
}



// -----------------------------------------------------------------------------
//
// frameAssemble - consume what the socket has; true when a whole frame is in, false on EAGAIN (wait)
//
// *deadP set when the peer closed, or the bytes are not a cor:// frame.
//
static bool frameAssemble(ServerConn* scP, bool* deadP)
{
  CorConn* cP = &scP->conn;

  *deadP = false;

  while (true)
  {
    //
    // From the read buffer first
    //
    while (cP->rpos < cP->rlen)
    {
      if (scP->hdrHave < FRAME_HEADER_LEN)
      {
        int take = FRAME_HEADER_LEN - scP->hdrHave;
        if (take > cP->rlen - cP->rpos)
          take = cP->rlen - cP->rpos;

        memcpy(&scP->hdr[scP->hdrHave], &cP->rbuf[cP->rpos], take);
        scP->hdrHave += take;
        cP->rpos     += take;

        if (scP->hdrHave < FRAME_HEADER_LEN)
          continue;

        if (memcmp(scP->hdr, frameMagic, 4) != 0)
        {
          COR_W("cor:// %s: not a cor:// frame (bad magic) - closing", cP->peer);
          *deadP = true;
          return false;
        }

        memcpy(&scP->correlation, &scP->hdr[8],  4);
        memcpy(&scP->bodyLen,     &scP->hdr[12], 4);

        if ((scP->bodyLen > FRAME_MAX) || ((scP->body = malloc(scP->bodyLen + 1)) == NULL))
        {
          COR_W("cor:// %s: frame too large - closing", cP->peer);
          *deadP = true;
          return false;
        }
        scP->bodyHave = 0;
      }

      int take = scP->bodyLen - scP->bodyHave;
      if (take > cP->rlen - cP->rpos)
        take = cP->rlen - cP->rpos;

      memcpy(&scP->body[scP->bodyHave], &cP->rbuf[cP->rpos], take);
      scP->bodyHave += take;
      cP->rpos      += take;

      if (scP->bodyHave == scP->bodyLen)
        return true;
    }

    if ((scP->hdrHave == FRAME_HEADER_LEN) && (scP->bodyHave == scP->bodyLen))
      return true;                                   // an empty tree - never sent, but not an error

    //
    // Then the socket - a large remainder straight into the frame
    //
    ssize_t r;

    if ((scP->hdrHave == FRAME_HEADER_LEN) && (scP->bodyLen - scP->bodyHave >= sizeof(cP->rbuf)))
    {
      r = read(cP->fd, &scP->body[scP->bodyHave], scP->bodyLen - scP->bodyHave);
      if (r > 0)
      {
        scP->bodyHave += r;
        if (scP->bodyHave == scP->bodyLen)
          return true;
        continue;
      }
    }
    else
    {
      r = read(cP->fd, cP->rbuf, sizeof(cP->rbuf));
      if (r > 0)
      {
        cP->rpos = 0;
        cP->rlen = (int) r;
        continue;
      }
    }

    if (r == 0)
    {
      *deadP = true;
      return false;
    }

    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return false;

    if (errno != EINTR)
    {
      *deadP = true;
      return false;
    }
  }
}



// -----------------------------------------------------------------------------
//
// frameTake - the assembled frame's bytes, now the caller's: the connection is ready for the next header
//
static char* frameTake(ServerConn* scP)
{
  char* body = scP->body;

  scP->body     = NULL;
  scP->hdrHave  = 0;
  scP->bodyLen  = 0;
  scP->bodyHave = 0;

  return body;
}



// -----------------------------------------------------------------------------
//
// requestFinish - the post-response phase, and the request's release
//
// Runs where the response was sent, or - when the post-response phase may block and that was the
// loop - on a worker (CorRestState.finishF), bound to the request's state.
//
static void requestFinish(CorRestState* stateP)
{
  ServerReq*  reqP = (ServerReq*) stateP->connection;
  ServerConn* scP  = reqP->scP;

  corRestP = stateP;
  corRestPostResponseHook();
  corRestStateRelease();
  if ((corRestUserDataFreeHookF != NULL) && (corRest.userData != NULL))
    corRestUserDataFreeHookF(corRest.userData);
  corRestP = NULL;

  free(reqP->body);                                  // the request's tree pointed into it
  corAllocBufferReset(&reqP->ka, false);
  free(reqP);

  connRelease(scP);
}



// -----------------------------------------------------------------------------
//
// requestRespond - the response, as the tree the service routine built; then the finish
//
// onLoop: called on the event loop (an inline request). Then the post-response phase runs here only
// when the application's finish-inline hook allows it, like HTTP's; otherwise it goes to a worker.
//
static void requestRespond(CorRestState* stateP, bool onLoop)
{
  ServerReq*  reqP = (ServerReq*) stateP->connection;
  ServerConn* scP  = reqP->scP;

  corRestP = stateP;

  CorNode*         responseP = corTreeObject(corRest.kallocP, NULL);
  CorNode*         outHdrP   = corTreeObject(corRest.kallocP, "headers");
  CorRestKeyValue  hv[64];
  int              hc        = corRestResponseHeaderVBuild(hv, 64);

  corTreeChildAdd(responseP, corTreeInteger(corRest.kallocP, "status", corRest.out.httpStatusCode));

  for (int i = 0; i < hc; i++)
    corTreeChildAdd(outHdrP, corTreeString(corRest.kallocP, hv[i].key, hv[i].value));
  corTreeChildAdd(responseP, outHdrP);

  if (corRest.out.responseTree != NULL)
  {
    CorNode* treeP = corRest.out.responseTree;

    treeP->name = (char*) "body";
    treeP->next = NULL;
    corTreeChildAdd(responseP, treeP);
  }
  else if ((corRest.out.payload != NULL) && (corRest.out.payloadSize > 0))
    corTreeChildAdd(responseP, corTreeString(corRest.kallocP, "text", corRest.out.payload));

  //
  // A frame that went out in part leaves the stream broken: the connection is closed, and the loop
  // sees it
  //
  pthread_mutex_lock(&scP->writeMutex);
  if ((scP->dead == false) && (frameSend(&scP->conn, FRAME_RESPONSE, reqP->correlation, responseP, 30000) == false))
  {
    COR_W("cor:// %s: the response could not be sent - closing", scP->conn.peer);
    scP->dead = true;
    shutdown(scP->conn.fd, SHUT_RDWR);
  }
  pthread_mutex_unlock(&scP->writeMutex);

  if ((onLoop == false) || ((corRestFinishInlineHookF != NULL) && (corRestFinishInlineHookF() == true)))
  {
    requestFinish(stateP);
    return;
  }

  stateP->asyncFinishing = true;
  stateP->finishF        = requestFinish;
  corRestP               = NULL;
  corRestAsyncEnqueue(stateP);
}



// -----------------------------------------------------------------------------
//
// requestResume - a worker has processed the request: the response goes out from the worker
//
static void requestResume(CorRestState* stateP)
{
  requestRespond(stateP, false);
}



// -----------------------------------------------------------------------------
//
// requestStart - a request frame is in: decoded, its state, and run - on the loop or on a worker
//
// The same sequence as corRestProcessInProcess - a fresh state, the userData hook, URI params,
// headers - except that the body is a tree already, and the response is not rendered.
//
static bool requestStart(ServerConn* scP, CorRestState** blockingPP)
{
  ServerReq* reqP = (ServerReq*) malloc(sizeof(ServerReq));
  uint32_t   len  = scP->bodyLen;

  if (reqP == NULL)
    return false;

  reqP->scP         = scP;
  reqP->correlation = scP->correlation;
  reqP->body        = frameTake(scP);
  corAllocBufferInit(&reqP->ka, reqP->kaBuf, sizeof(reqP->kaBuf), 64 * 1024, NULL, "cor:// request");

  const char* error;
  CorNode*    requestP = corTreeBinDecode(reqP->body, len, codecP, &scP->conn.in, &reqP->ka, &error);
  CorNode*    verbP    = (requestP != NULL) ? corTreeLookup(requestP, "verb")    : NULL;
  CorNode*    pathP    = (requestP != NULL) ? corTreeLookup(requestP, "path")    : NULL;
  CorNode*    headersP = (requestP != NULL) ? corTreeLookup(requestP, "headers") : NULL;
  CorNode*    bodyP    = (requestP != NULL) ? corTreeLookup(requestP, "body")    : NULL;

  if ((verbP == NULL) || (verbP->type != CorString) || (pathP == NULL) || (pathP->type != CorString))
  {
    if (requestP == NULL)
      COR_W("cor:// %s: undecodable request (%s) - closing", scP->conn.peer, error);
    else
      COR_W("cor:// %s: a request without verb or path - closing", scP->conn.peer);

    free(reqP->body);
    corAllocBufferReset(&reqP->ka, false);
    free(reqP);
    return false;
  }

  atomic_fetch_add(&scP->refs, 1);                  // released in requestFinish

  CorRestState* stateP = &reqP->state;

  corRestP = stateP;
  corRestStateInit(reqP, pathP->value.s, verbP->value.s);

  //
  // The request's time, as each HTTP backend sets it: createdAt/modifiedAt, TRoE and notification times
  // are all this one instant. Missing, every entity written over cor:// was created in 1970.
  //
  struct timespec ts;
  struct timespec tsM;

  clock_gettime(CLOCK_REALTIME,  &ts);
  clock_gettime(CLOCK_MONOTONIC, &tsM);
  corRest.requestStartTime     = (uint64_t) ts.tv_sec  * 1000000000ULL + (uint64_t) ts.tv_nsec;
  corRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

  //
  // The path travels as the client wrote it, percent-encoded - decoded here as an HTTP backend
  // decodes it (corRestStateInit has split off the query already, so a '%3F' stays in the path)
  //
  corRestUrlValueDecode(corRest.in.urlPath);
  corRestUrlPathNormalize();

  if (corRestUserDataAllocHookF != NULL)
    corRest.userData = corRestUserDataAllocHookF();

  corRestUriParamsParse();

  if ((headersP != NULL) && (headersP->type == CorObject))
  {
    for (CorNode* hP = headersP->value.head; hP != NULL; hP = hP->next)
    {
      if (hP->type == CorString)
        corRestHttpHeaderAdd(hP->name, hP->value.s);
    }
  }

  if (bodyP != NULL)
  {
    bodyP->name = NULL;
    bodyP->next = NULL;
    corRest.in.requestTree = bodyP;
  }

  corRest.out.noRender = true;
  stateP->shard        = scP->shard;

  //
  // On the loop when it cannot block. One that can: back to the leader that read it (*blockingPP),
  // which hands the loop to another thread and runs it itself - or, with nobody to take the loop
  // (blockingPP NULL), to a worker, and the loop reads on.
  //
  if (corRestAsyncDispatch() == true)
  {
    corRestP = NULL;

    if (blockingPP != NULL)
    {
      *blockingPP = stateP;
      return true;
    }

    stateP->resumeF = requestResume;
    corRestAsyncEnqueue(stateP);
    return true;
  }

  corRestProcessRequest();
  requestRespond(stateP, true);
  return true;
}



// -----------------------------------------------------------------------------
//
// helloAnswer - the connection's first frame
//
static bool helloAnswer(ServerConn* scP)
{
  CorAlloc    ka;
  char        kaBuf[2048];
  const char* error;
  int         peerNamespaces = 0;
  uint32_t    len            = scP->bodyLen;
  char*       body           = frameTake(scP);

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 4096, NULL, "cor:// hello");

  CorNode*    helloP = corTreeBinDecode(body, len, NULL, NULL, &ka, &error);
  const char* why    = (helloP == NULL) ? "not a HELLO" : helloCheck(helloP, &peerNamespaces);
  bool        ok     = false;

  if ((scP->hdr[4] != FRAME_HELLO) || (why != NULL))
    COR_W("cor:// %s: refused: %s", scP->conn.peer, (why != NULL) ? why : "the first frame is not HELLO");
  else
  {
    pthread_mutex_lock(&scP->writeMutex);
    ok = (frameSend(&scP->conn, FRAME_HELLO_ACK, scP->correlation, helloTree(&ka), 10000) == true) &&
         (tablesOpen(&scP->conn, peerNamespaces) == true);
    pthread_mutex_unlock(&scP->writeMutex);
  }

  free(body);
  corAllocBufferReset(&ka, false);
  scP->helloDone = true;

  if (ok == true)
    COR_V("cor:// %s: connected", scP->conn.peer);

  return ok;
}



// -----------------------------------------------------------------------------
//
// Who reads a connection - its loop, or a thread of its own
//
// A connection starts on its loop. When it brings a request that can block (a forward, waiting on its
// peer), a thread takes it over: the thread runs that request, and from then on reads the connection
// itself - the request that a frame carries runs on the thread that read it, with no hand-off, as in a
// thread-per-connection server. That is what the connections between brokers are: few, long-lived,
// one request in flight on each most of the time.
//
// The loop is still there for what multiplexing adds. Before it runs a request that can block, the
// thread gives the connection back to the loop - armed - so that a frame arriving meanwhile is read and
// started at once instead of waiting behind it. Done, the thread takes the connection back - unless the
// loop has it by then: then the thread is free again, and the connection stays with the loop until its
// next blocking request finds a thread.
//
// 'reader' says who has it; whoever takes it does so with a compare-and-swap from READER_NONE, so an
// epoll event that was already on its way when the thread took the connection back finds it taken
// and is dropped. With one request in flight, the cost over a thread per connection is two epoll_ctl
// calls per request, and no thread wake-up.
//
// Free threads wait in a pool; a thread is made when none is free.
//
enum { READER_NONE = 0, READER_LOOP = 1, READER_THREAD = 2 };

typedef struct OwnerTask
{
  ServerConn*        scP;
  CorRestState*      stateP;          // the blocking request it starts with
  struct OwnerTask*  next;
} OwnerTask;

static pthread_mutex_t  ownerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   ownerCond  = PTHREAD_COND_INITIALIZER;
static OwnerTask*       ownerQueue = NULL;
static int              ownerIdle  = 0;

static void* ownerThread(void* arg);



// -----------------------------------------------------------------------------
//
// connDisarm - no more events for this connection until it is armed again
//
static void connDisarm(ServerConn* scP)
{
  struct epoll_event ev;

  ev.events   = 0;
  ev.data.ptr = scP;

  epoll_ctl(scP->loopFd, EPOLL_CTL_MOD, scP->conn.fd, &ev);
}



// -----------------------------------------------------------------------------
//
// connLetGo - whoever reads the connection lets go of it, and arms it: the loop reads it next
//
static void connLetGo(ServerConn* scP)
{
  atomic_store(&scP->reader, READER_NONE);
  connArm(scP);
}



// -----------------------------------------------------------------------------
//
// connServe - read and serve a connection's frames until it has to wait, dies, or yields a blocking request
//
// Called by the connection's reader. Returns the blocking request's state - the reader still has the
// connection then - or NULL: the connection is let go (armed) or gone.
//
static CorRestState* connServe(ServerConn* scP)
{
  while (true)
  {
    bool dead;

    if (frameAssemble(scP, &dead) == false)
    {
      if (dead == true)
        connDead(scP);
      else
        connLetGo(scP);                              // not all of the next one yet
      return NULL;
    }

    CorRestState* blockingP = NULL;
    bool          ok        = (scP->helloDone == false) ? helloAnswer(scP) : requestStart(scP, &blockingP);

    if (ok == false)
    {
      connDead(scP);
      return NULL;
    }

    if (blockingP != NULL)
      return blockingP;
  }
}



// -----------------------------------------------------------------------------
//
// ownerRun - a thread with a connection: its requests, until it loses the connection to the loop
//
static void ownerRun(ServerConn* scP, CorRestState* stateP)
{
  while (true)
  {
    //
    // The blocking request: the connection goes back to the loop while it runs. The thread keeps a
    // reference of its own meanwhile - the loop may find the peer gone and drop the reader's, and the
    // request its own when it finishes.
    //
    atomic_fetch_add(&scP->refs, 1);
    connLetGo(scP);

    corRestP = stateP;
    corRestProcessRequest();
    requestRespond(stateP, false);

    //
    // Take the connection back - if the loop has not (a connection the loop found dead stays the
    // loop's: it is never READER_NONE again)
    //
    connDisarm(scP);

    int  none = READER_NONE;
    bool back = atomic_compare_exchange_strong(&scP->reader, &none, READER_THREAD);

    connRelease(scP);                              // the thread's own - with 'back', the reader's is still there

    if (back == false)
      break;

    //
    // Its next frames: read here, the inline ones run here, until the next one that can block. Waiting
    // for a frame, the thread holds the connection, so the loop never sees it.
    //
    stateP = NULL;

    while (stateP == NULL)
    {
      struct pollfd p = { scP->conn.fd, POLLIN, 0 };

      if ((scP->conn.rpos >= scP->conn.rlen) && (poll(&p, 1, -1) < 0) && (errno != EINTR))
      {
        connDead(scP);
        return;
      }

      bool dead;

      if (frameAssemble(scP, &dead) == false)
      {
        if (dead == true)
        {
          connDead(scP);
          return;
        }
        continue;                                    // not all of it yet
      }

      bool ok = (scP->helloDone == false) ? helloAnswer(scP) : requestStart(scP, &stateP);

      if (ok == false)
      {
        connDead(scP);
        return;
      }
    }
  }
}



// -----------------------------------------------------------------------------
//
// ownerThread - waits for a connection with a blocking request, serves it, waits again
//
static void* ownerThread(void* arg)
{
  OwnerTask* taskP = (OwnerTask*) arg;

  while (true)
  {
    ServerConn*   scP    = taskP->scP;
    CorRestState* stateP = taskP->stateP;

    free(taskP);
    ownerRun(scP, stateP);

    //
    // Free: counted idle until a giver claims it (ownerGive) - so idle = waiting - queued, always
    //
    pthread_mutex_lock(&ownerMutex);
    ownerIdle += 1;
    while (ownerQueue == NULL)
      pthread_cond_wait(&ownerCond, &ownerMutex);
    taskP      = ownerQueue;
    ownerQueue = taskP->next;
    pthread_mutex_unlock(&ownerMutex);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// ownerGive - a thread for this connection and its blocking request: a free one, or a new one
//
static void ownerGive(ServerConn* scP, CorRestState* stateP)
{
  OwnerTask* taskP = (OwnerTask*) malloc(sizeof(OwnerTask));

  atomic_store(&scP->reader, READER_THREAD);

  if (taskP != NULL)
  {
    taskP->scP    = scP;
    taskP->stateP = stateP;
    taskP->next   = NULL;

    pthread_mutex_lock(&ownerMutex);

    if (ownerIdle > 0)
    {
      OwnerTask** tailPP = &ownerQueue;

      ownerIdle -= 1;                                // claimed

      while (*tailPP != NULL)
        tailPP = &(*tailPP)->next;
      *tailPP = taskP;

      pthread_cond_signal(&ownerCond);
      pthread_mutex_unlock(&ownerMutex);
      return;
    }

    pthread_mutex_unlock(&ownerMutex);

    pthread_t tid;

    if (pthread_create(&tid, NULL, ownerThread, taskP) == 0)
    {
      pthread_detach(tid);
      return;
    }

    free(taskP);
  }

  //
  // No thread to be had: a worker runs the request, and the loop keeps the connection
  //
  COR_W("cor:// %s: no thread for a blocking request - it goes to a worker", scP->conn.peer);
  stateP->resumeF = requestResume;
  corRestAsyncEnqueue(stateP);
  connLetGo(scP);
}



// -----------------------------------------------------------------------------
//
// serverLoop - one event loop
//
static void* serverLoop(void* arg)
{
  int                loopFd = (int) (intptr_t) arg;
  struct epoll_event evV[64];

  while (true)
  {
    int n = epoll_wait(loopFd, evV, 64, -1);

    for (int i = 0; i < n; i++)
    {
      ServerConn* scP  = (ServerConn*) evV[i].data.ptr;
      int         none = READER_NONE;

      if (atomic_compare_exchange_strong(&scP->reader, &none, READER_LOOP) == false)
        continue;                                    // a thread took it back - this event is stale

      CorRestState* blockingP = connServe(scP);

      if (blockingP != NULL)
        ownerGive(scP, blockingP);
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// The loops, and the listener that hands them connections
//
static int  loopFdV[16];
static int  loops    = 0;
static int  nextLoop = 0;



// -----------------------------------------------------------------------------
//
// listener - hands each connection to a loop in turn
//
static void* listener(void* arg)
{
  int listenFd = (int) (intptr_t) arg;

  while (true)
  {
    struct sockaddr_storage peer;
    socklen_t               peerLen = sizeof(peer);
    int                     fd      = accept(listenFd, (struct sockaddr*) &peer, &peerLen);

    if (fd < 0)
    {
      if (errno != EINTR)
        COR_E("cor:// accept failed: %s", strerror(errno));
      continue;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    ServerConn* scP = calloc(1, sizeof(ServerConn));
    if (scP == NULL)
    {
      close(fd);
      continue;
    }

    char host[64], port[16];

    scP->conn.fd = fd;
    scP->loopFd  = loopFdV[nextLoop];
    scP->shard   = nextLoop;
    scP->refs    = 1;                                // the loop's - dropped by connDead
    nextLoop     = (nextLoop + 1) % loops;
    pthread_mutex_init(&scP->writeMutex, NULL);

    if (getnameinfo((struct sockaddr*) &peer, peerLen, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
      snprintf(scP->conn.peer, sizeof(scP->conn.peer), "%s:%s", host, port);

    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.ptr = scP;

    if (epoll_ctl(scP->loopFd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
      pthread_mutex_destroy(&scP->writeMutex);
      close(fd);
      free(scP);
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// corRestCorListen -
//
bool corRestCorListen(unsigned short port, int loopCount)
{
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  int one = 1;
  int off = 0;
  setsockopt(fd, SOL_SOCKET,   SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,  &off, sizeof(off));      // IPv4 too

  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port   = htons(port);
  addr.sin6_addr   = in6addr_any;

  if ((bind(fd, (struct sockaddr*) &addr, sizeof(addr)) != 0) || (listen(fd, 128) != 0))
  {
    COR_E("cor:// cannot listen on port %d: %s", port, strerror(errno));
    close(fd);
    return false;
  }

  loops = (loopCount < 1) ? 1 : (loopCount > 16) ? 16 : loopCount;

  for (int i = 0; i < loops; i++)
  {
    pthread_t tid;

    if (((loopFdV[i] = epoll_create1(0)) < 0) || (pthread_create(&tid, NULL, serverLoop, (void*) (intptr_t) loopFdV[i]) != 0))
    {
      COR_E("cor:// cannot start event loop %d", i);
      return false;
    }
    pthread_detach(tid);
  }

  pthread_t tid;
  if (pthread_create(&tid, NULL, listener, (void*) (intptr_t) fd) != 0)
  {
    close(fd);
    return false;
  }
  pthread_detach(tid);

  COR_V("cor:// listening on port %d, %d event loop%s", port, loops, (loops == 1) ? "" : "s");
  return true;
}



// =============================================================================
//
// CLIENT
//
// The connections to a peer are the whole process's, shared by every thread, with any number of
// requests in flight on each: a request carries its own correlation id, and its response may come back
// in any order (coraine doc/cor-protocol.md § 5.3).
//
// Sending: a request is encoded and written in one piece under the connection's write lock - the
// outgoing tables change as a tree is encoded, and the peer decodes in the order the frames arrive.
//
// Receiving, with no reader thread: a caller waiting for its response that finds nobody reading the
// connection becomes its READER (leader/follower). It reads the frames one after the other and decodes
// each one as it arrives - the incoming tables change as a tree is decoded, so frames are decoded in
// stream order - into the allocator of the call it answers, marks that call done and wakes its caller.
// Once its own response is in, it stops reading and wakes a caller still waiting, which takes over.
// With one request in flight, the caller reads its own response: no thread hop at all.
//
// A peer has up to corRestCorClientConns connections (default 1). A call takes an idle one, else opens
// a new one while under that cap, else shares the least busy: a caller alone on a connection reads its
// own response, so with enough connections nothing is handed between threads - requests are
// multiplexed when more are in flight than there are connections.
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// CorCall - one request in flight, as its caller waits for it
//
typedef struct CorCall
{
  uint32_t         correlation;
  CorAlloc*        allocP;          // where the response is decoded - the caller's
  pthread_cond_t   cond;
  bool             done;
  bool             claimed;         // the reader is reading its response: the caller may no longer time out
  const char*      error;           // done, and failed
  CorNode*         treeP;           // done: the response
  struct CorCall*  next;
} CorCall;



// -----------------------------------------------------------------------------
//
// MuxConn - a client connection, shared
//
typedef struct MuxConn
{
  CorConn          conn;            // the socket, the tables, the read buffer - the reader's
  pthread_mutex_t  mutex;           // the calls, 'reading'
  pthread_mutex_t  writeMutex;      // a request is encoded and sent in one piece
  CorCall*         callList;
  bool             reading;         // a caller is the reader
  uint32_t         correlation;
  _Atomic bool     dead;            // broken: every call on it failed, and the next one gets a new connection
  _Atomic int      refs;            // the peer table's, and one per caller using it
  _Atomic int      inFlight;        // callers using it now - a caller picks an idle one first
} MuxConn;



// -----------------------------------------------------------------------------
//
// CorPeer - the connections to one host:port
//
enum { PEERS_MAX = 64, PEER_CONNS_MAX = 16 };

typedef struct CorPeer
{
  char             key[160];        // host:port
  pthread_mutex_t  mutex;           // connV - a (re)connection is made under it
  MuxConn*         connV[PEER_CONNS_MAX];
} CorPeer;

static CorPeer          peerV[PEERS_MAX];
static int              peerCount       = 0;
static pthread_mutex_t  peerTableMutex  = PTHREAD_MUTEX_INITIALIZER;
static int              connsPerPeer    = 1;

static pthread_condattr_t  condAttr;
static pthread_once_t      condAttrOnce = PTHREAD_ONCE_INIT;

static void condAttrInit(void)
{
  pthread_condattr_init(&condAttr);
  pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
}



// -----------------------------------------------------------------------------
//
// corRestCorClientConns -
//
void corRestCorClientConns(int n)
{
  connsPerPeer = (n < 1) ? 1 : (n > PEER_CONNS_MAX) ? PEER_CONNS_MAX : n;
}



// -----------------------------------------------------------------------------
//
// authority - host and port out of cor://host:port[/...]
//
static bool authority(const char* url, char* host, int hostSize, char* port, int portSize)
{
  if (strncmp(url, "cor://", 6) != 0)
    return false;

  const char* hP    = &url[6];
  const char* endP  = strchr(hP, '/');
  int         len   = (endP != NULL) ? (int) (endP - hP) : (int) strlen(hP);
  const char* colon = memchr(hP, ':', len);

  if ((colon == NULL) || (colon == hP) || (colon - hP >= hostSize) || (len - (colon - hP) - 1 >= portSize) || (len - (colon - hP) - 1 <= 0))
    return false;

  memcpy(host, hP, colon - hP);
  host[colon - hP] = 0;
  memcpy(port, colon + 1, len - (colon - hP) - 1);
  port[len - (colon - hP) - 1] = 0;

  return true;
}



// -----------------------------------------------------------------------------
//
// clientConnect - a connection to host:port, HELLO exchanged
//
static bool clientConnect(CorConn* cP, const char* host, const char* port, int timeoutMs, const char** errorP)
{
  struct addrinfo  hints;
  struct addrinfo* resP = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host, port, &hints, &resP) != 0)
  {
    *errorP = "cannot resolve the cor:// host";
    return false;
  }

  cP->fd = -1;
  for (struct addrinfo* aP = resP; (aP != NULL) && (cP->fd < 0); aP = aP->ai_next)
  {
    int fd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol);

    if (fd < 0)
      continue;

    if (connect(fd, aP->ai_addr, aP->ai_addrlen) == 0)
      cP->fd = fd;
    else
      close(fd);
  }
  freeaddrinfo(resP);

  if (cP->fd < 0)
  {
    *errorP = "cannot connect to the cor:// endpoint";
    return false;
  }

  int            one = 1;
  struct timeval tv  = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };

  setsockopt(cP->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(cP->fd, SOL_SOCKET,  SO_SNDTIMEO, &tv,  sizeof(tv));
  snprintf(cP->peer, sizeof(cP->peer), "%s:%s", host, port);

  //
  // HELLO
  //
  CorAlloc ka;
  char     kaBuf[2048];
  uint8_t  type;
  uint32_t correlation;
  char*    buf;
  int      len;
  int      peerNamespaces = 0;
  bool     ok             = false;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 4096, NULL, "cor:// hello");

  if ((frameSend(cP, FRAME_HELLO, 0, helloTree(&ka), timeoutMs) == true) &&
      (frameRecv(cP, &type, &correlation, &buf, &len, &ka, timeoutMs, errorP) == true))
  {
    CorNode*    ackP = (type == FRAME_HELLO_ACK) ? corTreeBinDecode(buf, len, NULL, NULL, &ka, errorP) : NULL;
    const char* why  = (ackP == NULL) ? "no HELLO_ACK" : helloCheck(ackP, &peerNamespaces);

    if (why != NULL)
      *errorP = why;
    else
      ok = tablesOpen(cP, peerNamespaces);
  }

  corAllocBufferReset(&ka, false);

  if (ok == false)
    connClose(cP);

  return ok;
}



// -----------------------------------------------------------------------------
//
// muxUnref - one reference less; the last one closes and frees the connection
//
static void muxUnref(MuxConn* mcP)
{
  if (atomic_fetch_sub(&mcP->refs, 1) != 1)
    return;

  connClose(&mcP->conn);
  pthread_mutex_destroy(&mcP->mutex);
  pthread_mutex_destroy(&mcP->writeMutex);
  free(mcP);
}



// -----------------------------------------------------------------------------
//
// muxFail - the connection is broken: every call still on it fails, and nobody reads it any more
//
// Called with mcP->mutex held. The socket is shut down, not closed: a reader blocked on it returns, and
// the descriptor stays this connection's until its last user lets go (muxUnref).
//
static void muxFail(MuxConn* mcP, const char* error)
{
  if (mcP->dead == false)
  {
    mcP->dead = true;
    shutdown(mcP->conn.fd, SHUT_RDWR);
    COR_W("cor:// %s: %s - the connection is closed", mcP->conn.peer, error);
  }

  //
  // Not a CLAIMED call: the reader is reading its response into the caller's allocator - the caller,
  // woken now, would use it at the same time. The reader finishes that one itself.
  //
  for (CorCall* callP = mcP->callList; callP != NULL; callP = callP->next)
  {
    if ((callP->done == false) && (callP->claimed == false))
    {
      callP->done  = true;
      callP->error = error;
      pthread_cond_signal(&callP->cond);
    }
  }
}



// -----------------------------------------------------------------------------
//
// peerConnGet - a connection to host:port, its reference the caller's - opened if it has to be
//
// *reusedP: it was open already - a connection the peer may have closed since (a restart).
//
static MuxConn* peerConnGet(const char* host, const char* port, int timeoutMs, bool* reusedP, const char** errorP)
{
  char     key[160];
  CorPeer* peerP = NULL;

  snprintf(key, sizeof(key), "%s:%s", host, port);

  pthread_mutex_lock(&peerTableMutex);
  for (int i = 0; (i < peerCount) && (peerP == NULL); i++)
  {
    if (strcmp(peerV[i].key, key) == 0)
      peerP = &peerV[i];
  }

  if ((peerP == NULL) && (peerCount < PEERS_MAX))
  {
    peerP = &peerV[peerCount];
    snprintf(peerP->key, sizeof(peerP->key), "%s", key);
    pthread_mutex_init(&peerP->mutex, NULL);
    peerCount += 1;
  }
  pthread_mutex_unlock(&peerTableMutex);

  if (peerP == NULL)
  {
    *errorP = "too many cor:// peers";
    return NULL;
  }

  //
  // Which connection: an idle one if there is one; else a new one, while under the cap; else the
  // least busy. A caller alone on its connection reads its own response - nothing handed between
  // threads - so requests share a connection only when more of them are in flight than connections
  // are allowed.
  //
  pthread_mutex_lock(&peerP->mutex);

  int ix    = -1;
  int empty = -1;
  int least = -1;

  for (int i = 0; i < connsPerPeer; i++)
  {
    MuxConn* cP = peerP->connV[i];

    if ((cP != NULL) && (cP->dead == true))
    {
      peerP->connV[i] = NULL;
      muxUnref(cP);                                  // the table's reference
      cP = NULL;
    }

    if (cP == NULL)
    {
      if (empty < 0)
        empty = i;
      continue;
    }

    if (cP->inFlight == 0)
    {
      ix = i;
      break;
    }

    if ((least < 0) || (cP->inFlight < peerP->connV[least]->inFlight))
      least = i;
  }

  if (ix < 0)
    ix = (empty >= 0) ? empty : least;

  MuxConn* mcP = peerP->connV[ix];

  if (mcP != NULL)
  {
    atomic_fetch_add(&mcP->refs, 1);
    atomic_fetch_add(&mcP->inFlight, 1);
    *reusedP = true;
  }
  else if ((mcP = calloc(1, sizeof(MuxConn))) != NULL)
  {
    mcP->conn.fd = -1;
    pthread_mutex_init(&mcP->mutex, NULL);
    pthread_mutex_init(&mcP->writeMutex, NULL);

    if (clientConnect(&mcP->conn, host, port, timeoutMs, errorP) == false)
    {
      pthread_mutex_destroy(&mcP->mutex);
      pthread_mutex_destroy(&mcP->writeMutex);
      free(mcP);
      mcP = NULL;
    }
    else
    {
      mcP->refs        = 2;                          // the table's and the caller's
      mcP->inFlight    = 1;
      peerP->connV[ix] = mcP;
      *reusedP         = false;
    }
  }
  else
    *errorP = "out of memory";

  pthread_mutex_unlock(&peerP->mutex);

  return mcP;
}



// -----------------------------------------------------------------------------
//
// callSend - the call joins the connection, and its request goes out
//
// On failure the call is done, with its error.
//
static void callSend(MuxConn* mcP, CorCall* callP, CorNode* reqP, int timeoutMs)
{
  pthread_mutex_lock(&mcP->mutex);

  if (mcP->dead == true)
  {
    callP->done  = true;
    callP->error = "connection closed";
    pthread_mutex_unlock(&mcP->mutex);
    return;
  }

  callP->correlation = ++mcP->correlation;
  callP->next        = mcP->callList;
  mcP->callList      = callP;

  pthread_mutex_unlock(&mcP->mutex);

  pthread_mutex_lock(&mcP->writeMutex);
  bool sent = (mcP->dead == false) && (frameSend(&mcP->conn, FRAME_REQUEST, callP->correlation, reqP, timeoutMs) == true);
  pthread_mutex_unlock(&mcP->writeMutex);

  if (sent == false)
  {
    pthread_mutex_lock(&mcP->mutex);
    muxFail(mcP, "connection closed");             // a frame sent in part leaves the stream broken
    pthread_mutex_unlock(&mcP->mutex);
  }
}



// -----------------------------------------------------------------------------
//
// nowMs - CLOCK_MONOTONIC, in milliseconds
//
static int64_t nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// callFind - the call a response belongs to, NULL if none (it gave up waiting)
//
static CorCall* callFind(MuxConn* mcP, uint32_t correlation)
{
  for (CorCall* callP = mcP->callList; callP != NULL; callP = callP->next)
  {
    if ((callP->correlation == correlation) && (callP->done == false))
      return callP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// readerRun - read the connection until myP's own response is in, or myP's time is up
//
// Entered and left with mcP->mutex held, and 'reading' set by the caller; it is cleared here, and a
// caller still waiting is woken to read on.
//
// A frame is waited for only as long as myP may wait. Once a frame has begun, all of it is read
// (with the send timeout as the limit): a frame given up half read would leave the stream unreadable.
//
static void readerRun(MuxConn* mcP, CorCall* myP, int64_t deadline)
{
  while ((myP->done == false) && (mcP->dead == false))
  {
    pthread_mutex_unlock(&mcP->mutex);

    CorConn* cP      = &mcP->conn;
    int64_t  wait    = deadline - nowMs();
    bool     started = (cP->rpos < cP->rlen) || ((wait > 0) && (ioWait(cP->fd, POLLIN, (int) wait) == true));

    if (started == false)
    {
      pthread_mutex_lock(&mcP->mutex);
      if (myP->done == false)
      {
        myP->done  = true;
        myP->error = "timed out";
      }
      break;
    }

    uint8_t  header[FRAME_HEADER_LEN];
    uint32_t correlation;
    uint32_t len;

    if (connRead(cP, header, FRAME_HEADER_LEN, 30000) == false)
    {
      pthread_mutex_lock(&mcP->mutex);
      muxFail(mcP, (errno == ETIMEDOUT) ? "timed out in the middle of a frame" : "connection closed");
      break;
    }

    memcpy(&correlation, &header[8],  4);
    memcpy(&len,         &header[12], 4);

    if ((memcmp(header, frameMagic, 4) != 0) || (header[4] != FRAME_RESPONSE) || (len > FRAME_MAX))
    {
      pthread_mutex_lock(&mcP->mutex);
      muxFail(mcP, "not a cor:// response frame");
      break;
    }

    //
    // Whose? Claimed, its caller waits for it however long its own time was. A response nobody waits
    // for any more is decoded all the same - the tables must follow the stream - and dropped.
    //
    pthread_mutex_lock(&mcP->mutex);
    CorCall* callP = callFind(mcP, correlation);
    if (callP != NULL)
      callP->claimed = true;
    pthread_mutex_unlock(&mcP->mutex);

    CorAlloc   scratch;
    CorAlloc*  allocP = (callP != NULL) ? callP->allocP : &scratch;

    if (callP == NULL)
      corAllocBufferInit(&scratch, NULL, 0, 64 * 1024, NULL, "cor:// late response");

    const char* error = "connection closed in the middle of a frame";
    char*       buf   = corAlloc(allocP, len + 1);
    CorNode*    treeP = NULL;

    if ((buf != NULL) && (connRead(cP, buf, len, 30000) == true))
      treeP = corTreeBinDecode(buf, len, codecP, &cP->in, allocP, &error);

    if (callP == NULL)
      corAllocBufferReset(&scratch, false);

    pthread_mutex_lock(&mcP->mutex);

    if (treeP == NULL)
    {
      if (callP != NULL)
        callP->claimed = false;                    // released: muxFail fails it with the rest
      muxFail(mcP, error);                         // the tables can no longer be trusted
      break;
    }

    if (callP != NULL)
    {
      callP->treeP = treeP;
      callP->done  = true;
      pthread_cond_signal(&callP->cond);
    }
  }

  //
  // Done reading: a caller still waiting reads on
  //
  mcP->reading = false;

  for (CorCall* callP = mcP->callList; callP != NULL; callP = callP->next)
  {
    if (callP->done == false)
    {
      pthread_cond_signal(&callP->cond);
      break;
    }
  }
}



// -----------------------------------------------------------------------------
//
// callWait - until the call's response is in, it fails, or its time is up; then it leaves the connection
//
static void callWait(MuxConn* mcP, CorCall* callP, int timeoutMs)
{
  int64_t deadline = nowMs() + timeoutMs;

  pthread_mutex_lock(&mcP->mutex);

  while (callP->done == false)
  {
    if ((mcP->reading == false) && (mcP->dead == false))
    {
      mcP->reading = true;
      readerRun(mcP, callP, deadline);
      continue;
    }

    if (callP->claimed == true)
    {
      pthread_cond_wait(&callP->cond, &mcP->mutex);
      continue;
    }

    struct timespec ts = { deadline / 1000, (deadline % 1000) * 1000000 };

    if ((pthread_cond_timedwait(&callP->cond, &mcP->mutex, &ts) == ETIMEDOUT) && (callP->done == false) && (callP->claimed == false))
    {
      callP->done  = true;
      callP->error = "timed out";
    }
  }

  for (CorCall** pP = &mcP->callList; *pP != NULL; pP = &(*pP)->next)
  {
    if (*pP == callP)
    {
      *pP = callP->next;
      break;
    }
  }

  pthread_mutex_unlock(&mcP->mutex);
}



// -----------------------------------------------------------------------------
//
// requestTree - { verb, path, headers, body }
//
static CorNode* requestTree(CorAlloc* kaP, CorRestVerb verb, const char* pathAndQuery, CorRestKeyValue* headerV, int headerCount, CorNode* bodyP)
{
  CorNode* reqP = corTreeObject(kaP, NULL);
  CorNode* hdrP = corTreeObject(kaP, "headers");

  corTreeChildAdd(reqP, corTreeString(kaP, "verb", corRestVerbToString(verb)));
  corTreeChildAdd(reqP, corTreeString(kaP, "path", pathAndQuery));

  for (int i = 0; i < headerCount; i++)
    corTreeChildAdd(hdrP, corTreeString(kaP, headerV[i].key, headerV[i].value));
  corTreeChildAdd(reqP, hdrP);

  if (bodyP != NULL)
  {
    //
    // Borrowed: the caller's tree, named and unlinked for the encoding only - the caller gets it
    // back as it was
    //
    reqP->value.tail->next = bodyP;
    reqP->value.tail       = bodyP;
  }

  return reqP;
}



// -----------------------------------------------------------------------------
//
// responseUnpack - { status, headers, body | text } into a CorRestCorResponse
//
static void responseUnpack(CorNode* respTreeP, CorAlloc* respAllocP, CorRestCorResponse* respP)
{
  CorNode* statusP  = corTreeLookup(respTreeP, "status");
  CorNode* headersP = corTreeLookup(respTreeP, "headers");
  CorNode* bodyP    = corTreeLookup(respTreeP, "body");
  CorNode* textP    = corTreeLookup(respTreeP, "text");

  respP->status = ((statusP != NULL) && (statusP->type == CorInt)) ? (int) statusP->value.i : 500;

  if ((headersP != NULL) && (headersP->type == CorObject))
  {
    int n = 0;

    for (CorNode* hP = headersP->value.head; hP != NULL; hP = hP->next)
      ++n;

    respP->headerV = (n > 0) ? (CorRestKeyValue*) corAlloc(respAllocP, n * sizeof(CorRestKeyValue)) : NULL;
    for (CorNode* hP = headersP->value.head; (hP != NULL) && (respP->headerV != NULL); hP = hP->next)
    {
      if (hP->type != CorString)
        continue;
      respP->headerV[respP->headerCount].key   = hP->name;
      respP->headerV[respP->headerCount].value = hP->value.s;
      respP->headerCount += 1;
    }
  }

  if (bodyP != NULL)
  {
    bodyP->name = NULL;
    bodyP->next = NULL;
    respP->bodyTree = bodyP;
  }
  else if ((textP != NULL) && (textP->type == CorString))
    respP->bodyText = textP->value.s;
}



// -----------------------------------------------------------------------------
//
// corRestCorSend -
//
bool corRestCorSend(const char*         url,
                    CorRestVerb         verb,
                    const char*         pathAndQuery,
                    CorRestKeyValue*    headerV,
                    int                 headerCount,
                    CorNode*            bodyTree,
                    const char*         bodyText,
                    int                 timeoutMs,
                    CorAlloc*           respAllocP,
                    CorRestCorResponse* respP,
                    const char**        errorP)
{
  char host[128];
  char port[16];

  memset(respP, 0, sizeof(CorRestCorResponse));
  *errorP = NULL;

  if (authority(url, host, sizeof(host), port, sizeof(port)) == false)
  {
    *errorP = "not a cor://host:port URL";
    return false;
  }

  if (timeoutMs <= 0)
    timeoutMs = 30000;

  pthread_once(&condAttrOnce, condAttrInit);

  //
  // A body given as text: parsed here, once - the receiver then never parses it
  //
  if ((bodyTree == NULL) && (bodyText != NULL) && (bodyText[0] != 0))
  {
    CorJson cj;
    char*   copy = corAllocStrdup(respAllocP, bodyText);

    corJsonCreate(&cj, respAllocP);
    if ((copy == NULL) || ((bodyTree = corJsonParse(&cj, copy)) == NULL))
    {
      *errorP = "the request body is not JSON";
      return false;
    }
  }

  char*    savedName = (bodyTree != NULL) ? bodyTree->name : NULL;
  CorNode* savedNext = (bodyTree != NULL) ? bodyTree->next : NULL;

  if (bodyTree != NULL)
  {
    bodyTree->name = (char*) "body";
    bodyTree->next = NULL;
  }

  CorNode* reqP = requestTree(respAllocP, verb, pathAndQuery, headerV, headerCount, bodyTree);

  //
  // Send, and wait for the response. A connection the peer dropped since its last use (a restart) fails
  // on its first write or read: that one is retried once, on a fresh connection.
  //
  bool ok = false;

  for (int attempt = 0; (attempt < 2) && (ok == false); attempt++)
  {
    bool     reused = false;
    MuxConn* mcP    = peerConnGet(host, port, timeoutMs, &reused, errorP);

    if (mcP == NULL)
      break;

    CorCall call;

    memset(&call, 0, sizeof(call));
    call.allocP = respAllocP;
    pthread_cond_init(&call.cond, &condAttr);

    callSend(mcP, &call, reqP, timeoutMs);
    callWait(mcP, &call, timeoutMs);

    pthread_cond_destroy(&call.cond);
    atomic_fetch_sub(&mcP->inFlight, 1);
    muxUnref(mcP);

    if (call.error == NULL)
    {
      responseUnpack(call.treeP, respAllocP, respP);
      ok = true;
      break;
    }

    *errorP = call.error;

    //
    // Retried ONLY on a connection that was reused and that the peer had closed - a restart since
    // its last use. Never after a timeout: the peer may be executing it, and a second POST would
    // create twice.
    //
    if ((reused == false) || (strcmp(call.error, "connection closed") != 0))
      break;
  }

  if (bodyTree != NULL)
  {
    bodyTree->name = savedName;
    bodyTree->next = savedNext;
  }

  return ok;
}



// -----------------------------------------------------------------------------
//
// CorRestCorCall - a request sent with corRestCorStart, waited for with corRestCorWait
//
// Its response is decoded - by whichever thread reads it - into the call's own arena, which no other
// thread touches; corRestCorWait hands the arena to the caller's allocator (corAllocAdopt).
//
struct CorRestCorCall
{
  CorCall          call;
  MuxConn*         mcP;               // NULL: it failed before it was sent
  bool             reused;
  CorAlloc         arena;
  int              timeoutMs;
  const char*      error;             // it failed before it was sent

  // For the one retry - a connection the peer had closed since its last use
  char             url[300];
  CorRestVerb      verb;
  const char*      pathAndQuery;
  CorRestKeyValue* headerV;
  int              headerCount;
  CorNode*         bodyTree;
};



// -----------------------------------------------------------------------------
//
// corRestCorStart -
//
CorRestCorCall* corRestCorStart(const char*       url,
                                CorRestVerb       verb,
                                const char*       pathAndQuery,
                                CorRestKeyValue*  headerV,
                                int               headerCount,
                                CorNode*          bodyTree,
                                const char*       bodyText,
                                int               timeoutMs,
                                CorAlloc*         kaP,
                                const char**      errorP)
{
  char host[128];
  char port[16];

  *errorP = NULL;

  if (authority(url, host, sizeof(host), port, sizeof(port)) == false)
  {
    *errorP = "not a cor://host:port URL";
    return NULL;
  }

  if (timeoutMs <= 0)
    timeoutMs = 30000;

  pthread_once(&condAttrOnce, condAttrInit);

  if ((bodyTree == NULL) && (bodyText != NULL) && (bodyText[0] != 0))
  {
    CorJson cj;
    char*   copy = corAllocStrdup(kaP, bodyText);

    corJsonCreate(&cj, kaP);
    if ((copy == NULL) || ((bodyTree = corJsonParse(&cj, copy)) == NULL))
    {
      *errorP = "the request body is not JSON";
      return NULL;
    }
  }

  CorRestCorCall* cP = (CorRestCorCall*) calloc(1, sizeof(CorRestCorCall));

  if (cP == NULL)
  {
    *errorP = "out of memory";
    return NULL;
  }

  snprintf(cP->url, sizeof(cP->url), "%s", url);
  cP->verb         = verb;
  cP->pathAndQuery = pathAndQuery;
  cP->headerV      = headerV;
  cP->headerCount  = headerCount;
  cP->bodyTree     = bodyTree;
  cP->timeoutMs    = timeoutMs;

  corAllocBufferInit(&cP->arena, NULL, 0, 16 * 1024, NULL, "cor:// response");

  cP->mcP = peerConnGet(host, port, timeoutMs, &cP->reused, &cP->error);
  if (cP->mcP == NULL)
    return cP;                                       // corRestCorWait reports it

  char*    savedName = (bodyTree != NULL) ? bodyTree->name : NULL;
  CorNode* savedNext = (bodyTree != NULL) ? bodyTree->next : NULL;

  if (bodyTree != NULL)
  {
    bodyTree->name = (char*) "body";
    bodyTree->next = NULL;
  }

  CorNode* reqP = requestTree(kaP, verb, pathAndQuery, headerV, headerCount, bodyTree);

  cP->call.allocP = &cP->arena;
  pthread_cond_init(&cP->call.cond, &condAttr);
  callSend(cP->mcP, &cP->call, reqP, timeoutMs);     // encoded and sent here - the body is the caller's again after it

  if (bodyTree != NULL)
  {
    bodyTree->name = savedName;
    bodyTree->next = savedNext;
  }

  return cP;
}



// -----------------------------------------------------------------------------
//
// corRestCorWait -
//
bool corRestCorWait(CorRestCorCall* cP, CorAlloc* respAllocP, CorRestCorResponse* respP, const char** errorP)
{
  bool ok = false;

  memset(respP, 0, sizeof(CorRestCorResponse));
  *errorP = NULL;

  if (cP == NULL)
  {
    *errorP = "no such call";
    return false;
  }

  if (cP->mcP == NULL)
    *errorP = (cP->error != NULL) ? cP->error : "cor:// connection failed";
  else
  {
    callWait(cP->mcP, &cP->call, cP->timeoutMs);

    pthread_cond_destroy(&cP->call.cond);
    atomic_fetch_sub(&cP->mcP->inFlight, 1);
    muxUnref(cP->mcP);

    if (cP->call.error == NULL)
    {
      corAllocAdopt(respAllocP, &cP->arena);       // the response tree lives in respAllocP now
      responseUnpack(cP->call.treeP, respAllocP, respP);
      ok = true;
    }
    else if ((cP->reused == true) && (strcmp(cP->call.error, "connection closed") == 0))
    {
      //
      // The same rule as corRestCorSend: a reused connection the peer had closed - once more, on a
      // fresh one, and waiting for it here
      //
      ok = corRestCorSend(cP->url, cP->verb, cP->pathAndQuery, cP->headerV, cP->headerCount, cP->bodyTree, NULL,
                          cP->timeoutMs, respAllocP, respP, errorP);
    }
    else
      *errorP = cP->call.error;
  }

  corAllocBufferReset(&cP->arena, false);           // empty if adopted
  free(cP);

  return ok;
}
