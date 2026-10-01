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
#include <poll.h>                                     // poll
#include <pthread.h>                                  // pthread_create
#include <netdb.h>                                    // getaddrinfo
#include <sys/socket.h>                               // socket, setsockopt, accept, bind, listen
#include <netinet/in.h>                               // sockaddr_in6
#include <netinet/tcp.h>                              // TCP_NODELAY
#include <sys/epoll.h>                                // epoll_create1, epoll_ctl, epoll_wait
#include <fcntl.h>                                    // fcntl, O_NONBLOCK

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc, corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
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
#include "corRest/corRestStateInit.h"                 // corRestStateInit, corRestStateRelease
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
// requests off its connections with epoll. A request that cannot block - the same check HTTP makes
// (corRestAsyncDispatch) - runs right there on the loop. One that can (a forward, waiting on its
// peer) moves its connection off the loops for good, to a thread of its own (connDedicate): those are
// other brokers' forwarding connections, and a hop to the worker pool on every request would cost
// two thread wakeups each. A post-response phase that may block still goes to the worker pool
// (CorRestState.finishF), as HTTP's does.
//
// A connection is armed EPOLLONESHOT: once a request is in, it is not read again until its response
// is out and its post-response phase done - v1's one request in flight per connection.
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// ServerConn - a CorConn, plus what a server needs to assemble frames off a non-blocking socket
//
typedef struct ServerConn
{
  CorConn        conn;
  int            loopFd;           // the epoll instance the connection belongs to
  bool           helloDone;
  uint8_t        hdr[FRAME_HEADER_LEN];
  int            hdrHave;
  char*          body;             // the current frame's tree - the request's tree points into it
  uint32_t       bodyLen;
  uint32_t       bodyHave;
  uint32_t       correlation;
  CorAlloc       ka;               // the decoded request lives here - reset when the request is over
  char           kaBuf[16 * 1024];
  CorRestState*  stateP;           // the request in flight, NULL between requests
  bool           dedicated;        // moved off the loops to a thread of its own (connDedicate)
} ServerConn;



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
// serverConnFree - the peer is gone, or spoke nonsense
//
static void serverConnFree(ServerConn* scP)
{
  epoll_ctl(scP->loopFd, EPOLL_CTL_DEL, scP->conn.fd, NULL);
  connClose(&scP->conn);
  free(scP->body);
  corAllocBufferReset(&scP->ka, false);
  free(scP);
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
// frameDone - the frame's bytes are no longer needed: ready for the next header
//
static void frameDone(ServerConn* scP)
{
  free(scP->body);
  scP->body     = NULL;
  scP->hdrHave  = 0;
  scP->bodyLen  = 0;
  scP->bodyHave = 0;
}



// -----------------------------------------------------------------------------
//
// requestFinish - the post-response phase, the request's release, and the connection armed again
//
// Runs where the response was sent, or - when the post-response phase may block and that was the
// loop - on a worker (CorRestState.finishF), bound to the request's state.
//
static void requestFinish(CorRestState* stateP)
{
  ServerConn* scP = (ServerConn*) stateP->connection;

  corRestP = stateP;
  corRestPostResponseHook();
  corRestStateRelease();
  if ((corRestUserDataFreeHookF != NULL) && (corRest.userData != NULL))
    corRestUserDataFreeHookF(corRest.userData);
  free(stateP);
  corRestP = NULL;

  scP->stateP = NULL;
  frameDone(scP);                                    // the request's tree pointed into it
  corAllocBufferReset(&scP->ka, true);

  if (scP->dedicated == false)
    connArm(scP);
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
  ServerConn* scP = (ServerConn*) stateP->connection;

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

  if (frameSend(&scP->conn, FRAME_RESPONSE, scP->correlation, responseP, 30000) == false)
    COR_W("cor:// %s: the response could not be sent", scP->conn.peer);

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



static bool requestStart(ServerConn* scP);



// -----------------------------------------------------------------------------
//
// dedicatedThread - a connection of its own: the request that moved it here, then every later one
//
// A blocking socket and a blocking read for the next request, as cor:// v1 served every connection.
// The connection's buffered bytes are already in scP->conn.rbuf, so nothing that arrived is lost.
//
static void* dedicatedThread(void* arg)
{
  ServerConn* scP = (ServerConn*) arg;

  corRestP = scP->stateP;
  corRestProcessRequest();
  requestRespond(scP->stateP, false);

  while (true)
  {
    uint8_t     type;
    const char* error;
    char*       buf;
    int         len;

    if (frameRecv(&scP->conn, &type, &scP->correlation, &buf, &len, NULL, -1, &error) == false)
      break;

    if (type != FRAME_REQUEST)
    {
      free(buf);
      COR_W("cor:// %s: unexpected frame type %d - closing", scP->conn.peer, type);
      break;
    }

    scP->body     = buf;
    scP->bodyLen  = len;
    scP->bodyHave = len;
    scP->hdrHave  = FRAME_HEADER_LEN;

    if (requestStart(scP) == false)
      break;
  }

  connClose(&scP->conn);
  free(scP->body);
  corAllocBufferReset(&scP->ka, false);
  free(scP);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// connDedicate - off the loops, for good: a thread of its own for this connection
//
// A request that cannot run on a loop (a forward, waiting on its peer) would otherwise cost a hop to
// the worker pool and back - two thread wakeups - on EVERY request. The connections that send such
// requests are other brokers' forwarding connections: few, long-lived, and nothing but blocking
// requests. They are served best as cor:// v1 served everything, a thread each; the many short client
// connections stay on the loops.
//
static bool connDedicate(ServerConn* scP)
{
  epoll_ctl(scP->loopFd, EPOLL_CTL_DEL, scP->conn.fd, NULL);
  fcntl(scP->conn.fd, F_SETFL, fcntl(scP->conn.fd, F_GETFL, 0) & ~O_NONBLOCK);
  scP->dedicated = true;

  pthread_t      tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&tid, &attr, dedicatedThread, scP);
  pthread_attr_destroy(&attr);

  return (r == 0);
}



// -----------------------------------------------------------------------------
//
// requestStart - a request frame is in: its state, and then the loop or the connection's own thread
//
// The same sequence as corRestProcessInProcess - a fresh state, the userData hook, URI params,
// headers - except that the body is a tree already, and the response is not rendered.
//
static bool requestStart(ServerConn* scP)
{
  const char* error;
  CorNode*    requestP = corTreeBinDecode(scP->body, scP->bodyLen, codecP, &scP->conn.in, &scP->ka, &error);

  if (requestP == NULL)
  {
    COR_W("cor:// %s: undecodable request (%s) - closing", scP->conn.peer, error);
    return false;
  }

  CorNode* verbP    = corTreeLookup(requestP, "verb");
  CorNode* pathP    = corTreeLookup(requestP, "path");
  CorNode* headersP = corTreeLookup(requestP, "headers");
  CorNode* bodyP    = corTreeLookup(requestP, "body");

  if ((verbP == NULL) || (verbP->type != CorString) || (pathP == NULL) || (pathP->type != CorString))
  {
    COR_W("cor:// %s: a request without verb or path - closing", scP->conn.peer);
    return false;
  }

  CorRestState* stateP = (CorRestState*) malloc(sizeof(CorRestState));
  if (stateP == NULL)
    return false;

  corRestP = stateP;
  corRestStateInit(scP, pathP->value.s, verbP->value.s);

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
  stateP->shard        = 0;
  scP->stateP          = stateP;

  //
  // A dedicated connection's thread runs everything itself. On a loop: inline when it cannot block,
  // else the connection moves to a thread of its own (connDedicate), which runs this request first.
  //
  if (scP->dedicated == true)
  {
    corRestProcessRequest();
    requestRespond(stateP, false);
    return true;
  }

  if (corRestAsyncDispatch() == true)
  {
    corRestP = NULL;
    return connDedicate(scP);
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
  const char* error;
  int         peerNamespaces = 0;
  CorNode*    helloP         = corTreeBinDecode(scP->body, scP->bodyLen, NULL, NULL, &scP->ka, &error);
  const char* why            = (helloP == NULL) ? "not a HELLO" : helloCheck(helloP, &peerNamespaces);

  if ((scP->hdr[4] != FRAME_HELLO) || (why != NULL))
  {
    COR_W("cor:// %s: refused: %s", scP->conn.peer, (why != NULL) ? why : "the first frame is not HELLO");
    return false;
  }

  bool ok = (frameSend(&scP->conn, FRAME_HELLO_ACK, scP->correlation, helloTree(&scP->ka), 10000) == true) &&
            (tablesOpen(&scP->conn, peerNamespaces) == true);

  frameDone(scP);
  corAllocBufferReset(&scP->ka, true);
  scP->helloDone = true;

  if (ok == true)
    COR_V("cor:// %s: connected", scP->conn.peer);

  return ok;
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
      ServerConn* scP = (ServerConn*) evV[i].data.ptr;
      bool        dead;

      if (frameAssemble(scP, &dead) == false)
      {
        if (dead == true)
          serverConnFree(scP);
        else
          connArm(scP);                              // not all of it yet
        continue;
      }

      //
      // Who arms the connection again: after HELLO, this loop; after a request, requestFinish -
      // here when the request ran inline, on a worker when it did not
      //
      bool ok;

      if (scP->helloDone == false)
      {
        ok = helloAnswer(scP);
        if (ok == true)
          connArm(scP);
      }
      else
        ok = requestStart(scP);

      if (ok == false)
        serverConnFree(scP);
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
    nextLoop     = (nextLoop + 1) % loops;
    corAllocBufferInit(&scP->ka, scP->kaBuf, sizeof(scP->kaBuf), 64 * 1024, NULL, "cor:// connection");

    if (getnameinfo((struct sockaddr*) &peer, peerLen, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
      snprintf(scP->conn.peer, sizeof(scP->conn.peer), "%s:%s", host, port);

    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.ptr = scP;

    if (epoll_ctl(scP->loopFd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
      corAllocBufferReset(&scP->ka, false);
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
// =============================================================================



// -----------------------------------------------------------------------------
//
// The client's connections - per thread: a forward never shares a connection with another thread's,
// so v1 needs no multiplexing and no lock
//
enum { CLIENT_CONNS_MAX = 8 };

//
// Pointers, allocated on first use: a connection carries a 16 KB read buffer, and every broker
// thread would otherwise pay for eight of them in thread-local storage, cor:// or not
//
static __thread CorConn* clientConnV[CLIENT_CONNS_MAX];



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
// clientConnGet - this thread's connection to host:port, opened if there is none
//
static CorConn* clientConnGet(const char* host, const char* port, int timeoutMs, bool* reusedP, const char** errorP)
{
  char key[160];

  snprintf(key, sizeof(key), "%s:%s", host, port);

  CorConn* freeP = NULL;

  for (int i = 0; i < CLIENT_CONNS_MAX; i++)
  {
    CorConn* cP = clientConnV[i];

    if ((cP != NULL) && (cP->fd >= 0) && (strcmp(cP->peer, key) == 0))
    {
      *reusedP = true;
      return cP;
    }

    if ((freeP == NULL) && ((cP == NULL) || (cP->fd < 0)))
    {
      if (cP == NULL)
      {
        if ((cP = calloc(1, sizeof(CorConn))) == NULL)
          continue;
        cP->fd         = -1;
        clientConnV[i] = cP;
      }
      freeP = cP;
    }
  }

  if (freeP == NULL)                                 // all in use: the first one makes room
  {
    freeP = clientConnV[0];
    connClose(freeP);
  }

  return (clientConnect(freeP, host, port, timeoutMs, errorP) == true) ? freeP : NULL;
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
  // Send, and read the response. A connection the peer dropped since its last use (a restart) fails
  // on its first write or read: that one is retried once, on a fresh connection.
  //
  bool     ok      = false;
  char*    buf     = NULL;
  int      len     = 0;

  for (int attempt = 0; (attempt < 2) && (ok == false); attempt++)
  {
    bool     reused = false;
    CorConn* cP     = clientConnGet(host, port, timeoutMs, &reused, errorP);

    if (cP == NULL)
      break;

    uint32_t correlation = ++cP->correlation;
    uint8_t  type;
    uint32_t respCorrelation;

    if ((frameSend(cP, FRAME_REQUEST, correlation, reqP, timeoutMs) == true) &&
        (frameRecv(cP, &type, &respCorrelation, &buf, &len, respAllocP, timeoutMs, errorP) == true))
    {
      if ((type != FRAME_RESPONSE) || (respCorrelation != correlation))
      {
        *errorP = "unexpected frame in answer";
        connClose(cP);
        break;
      }

      CorNode* respTreeP = corTreeBinDecode(buf, len, codecP, &cP->in, respAllocP, errorP);

      if (respTreeP == NULL)
      {
        connClose(cP);                               // the tables can no longer be trusted
        break;
      }

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

      ok = true;
    }
    else
    {
      connClose(cP);

      //
      // Retried ONLY on a connection that was reused and that the peer had closed - a restart since
      // its last use. Never after a timeout: the peer may be executing it, and a second POST would
      // create twice.
      //
      if ((reused == false) || (*errorP == NULL) || (strcmp(*errorP, "connection closed") != 0))
        break;
    }
  }

  if (bodyTree != NULL)
  {
    bodyTree->name = savedName;
    bodyTree->next = savedNext;
  }

  return ok;
}
