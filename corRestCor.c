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

  while (n > 0)
  {
    if (ioWait(fd, POLLOUT, timeoutMs) == false)
      return false;

    ssize_t w = send(fd, cP, n, MSG_NOSIGNAL);
    if (w < 0)
    {
      if (errno == EINTR)
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
// readAll - exactly n bytes; timeoutMs < 0 waits for ever (a server waiting for the next request)
//
static bool readAll(int fd, void* p, int n, int timeoutMs)
{
  char* cP = p;

  while (n > 0)
  {
    if (ioWait(fd, POLLIN, timeoutMs) == false)
    {
      errno = ETIMEDOUT;
      return false;
    }

    ssize_t r = read(fd, cP, n);
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

    cP += r;
    n  -= r;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// frameSend - a tree, encoded with the connection's outgoing tables (or none, for HELLO)
//
static bool frameSend(CorConn* cP, uint8_t type, uint32_t correlation, CorNode* treeP, int timeoutMs)
{
  CorBinBuffer body   = { NULL, 0, 0 };
  bool         plain  = (type == FRAME_HELLO) || (type == FRAME_HELLO_ACK);

  if (corTreeBinEncode(treeP, plain ? NULL : codecP, plain ? NULL : &cP->out, &body) == false)
  {
    free(body.buf);
    return false;
  }

  uint8_t header[FRAME_HEADER_LEN] = { 0 };

  memcpy(header, frameMagic, 4);
  header[4] = type;
  memcpy(&header[8],  &correlation, 4);                // little-endian, as the hosts are
  memcpy(&header[12], &body.len,    4);

  bool ok = writeAll(cP->fd, header, FRAME_HEADER_LEN, timeoutMs) && writeAll(cP->fd, body.buf, body.len, timeoutMs);

  free(body.buf);
  return ok;
}



// -----------------------------------------------------------------------------
//
// frameRecv - a frame's header and its body; the body from allocP, or malloc'd when allocP is NULL
//
static bool frameRecv(CorConn* cP, uint8_t* typeP, uint32_t* correlationP, char** bufP, int* lenP, CorAlloc* allocP, int timeoutMs, const char** errorP)
{
  uint8_t header[FRAME_HEADER_LEN];

  if (readAll(cP->fd, header, FRAME_HEADER_LEN, timeoutMs) == false)
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

  if (readAll(cP->fd, buf, len, timeoutMs) == false)
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
  cP->fd = -1;

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
// =============================================================================



// -----------------------------------------------------------------------------
//
// serverRequest - one request: run it like any other, and answer with the tree it built
//
// The same sequence as corRestProcessInProcess - a fresh state, the userData hook, URI params, headers -
// except that the body is a tree already, and the response is not rendered.
//
static bool serverRequest(CorConn* cP, uint32_t correlation, CorNode* requestP)
{
  CorNode* verbP    = corTreeLookup(requestP, "verb");
  CorNode* pathP    = corTreeLookup(requestP, "path");
  CorNode* headersP = corTreeLookup(requestP, "headers");
  CorNode* bodyP    = corTreeLookup(requestP, "body");

  if ((verbP == NULL) || (verbP->type != CorString) || (pathP == NULL) || (pathP->type != CorString))
  {
    COR_W("cor:// %s: a request without verb or path - closing", cP->peer);
    return false;
  }

  CorRestState* stateP = (CorRestState*) malloc(sizeof(CorRestState));
  if (stateP == NULL)
    return false;

  corRestP = stateP;
  corRestStateInit(NULL, pathP->value.s, verbP->value.s);

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

  //
  // The body: the tree itself. It lives in the connection's arena, not the request's - which is
  // fine, the request is over before that arena is reset.
  //
  if (bodyP != NULL)
  {
    bodyP->name = NULL;
    bodyP->next = NULL;
    corRest.in.requestTree = bodyP;
  }

  corRest.out.noRender = true;
  corRestProcessRequest();

  //
  // The response: status, the headers HTTP would send, and the tree
  //
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

  bool ok = frameSend(cP, FRAME_RESPONSE, correlation, responseP, 30000);

  corRestPostResponseHook();
  corRestStateRelease();
  if ((corRestUserDataFreeHookF != NULL) && (corRest.userData != NULL))
    corRestUserDataFreeHookF(corRest.userData);
  free(stateP);
  corRestP = NULL;

  return ok;
}



// -----------------------------------------------------------------------------
//
// serverConnection - a thread per connection: HELLO, then request after request
//
static void* serverConnection(void* arg)
{
  CorConn*    cP = (CorConn*) arg;
  const char* error;
  uint8_t     type;
  uint32_t    correlation;
  char*       buf;
  int         len;

  CorAlloc    ka;
  static __thread char kaBuf[16 * 1024];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "cor:// connection");

  //
  // HELLO
  //
  if (frameRecv(cP, &type, &correlation, &buf, &len, NULL, 10000, &error) == false)
  {
    COR_W("cor:// %s: no HELLO: %s", cP->peer, error);
    goto done;
  }

  int      peerNamespaces = 0;
  CorNode* helloP         = (type == FRAME_HELLO) ? corTreeBinDecode(buf, len, NULL, NULL, &ka, &error) : NULL;
  const char* why         = (helloP == NULL) ? "not a HELLO" : helloCheck(helloP, &peerNamespaces);

  free(buf);

  if (why != NULL)
  {
    COR_W("cor:// %s: refused: %s", cP->peer, why);
    goto done;
  }

  if ((frameSend(cP, FRAME_HELLO_ACK, correlation, helloTree(&ka), 10000) == false) || (tablesOpen(cP, peerNamespaces) == false))
    goto done;

  corAllocBufferReset(&ka, true);
  COR_V("cor:// %s: connected", cP->peer);

  //
  // Requests, one at a time, for as long as the peer keeps the connection
  //
  while (frameRecv(cP, &type, &correlation, &buf, &len, NULL, -1, &error) == true)
  {
    if (type != FRAME_REQUEST)
    {
      free(buf);
      COR_W("cor:// %s: unexpected frame type %d - closing", cP->peer, type);
      break;
    }

    CorNode* requestP = corTreeBinDecode(buf, len, codecP, &cP->in, &ka, &error);
    bool     ok       = (requestP != NULL) && serverRequest(cP, correlation, requestP);

    if (requestP == NULL)
      COR_W("cor:// %s: undecodable request (%s) - closing", cP->peer, error);

    free(buf);                                       // the request's tree pointed into it - the request is over
    corAllocBufferReset(&ka, true);

    if (ok == false)
      break;
  }

 done:
  connClose(cP);
  corAllocBufferReset(&ka, false);
  free(cP);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// listener - accept, and a thread for each connection
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
      if (errno == EINTR)
        continue;
      COR_E("cor:// accept failed: %s", strerror(errno));
      continue;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    CorConn* cP = calloc(1, sizeof(CorConn));
    if (cP == NULL)
    {
      close(fd);
      continue;
    }

    char host[64], port[16];

    cP->fd = fd;
    if (getnameinfo((struct sockaddr*) &peer, peerLen, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
      snprintf(cP->peer, sizeof(cP->peer), "%s:%s", host, port);

    pthread_t      tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, serverConnection, cP) != 0)
    {
      close(fd);
      free(cP);
    }

    pthread_attr_destroy(&attr);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// corRestCorListen -
//
bool corRestCorListen(unsigned short port)
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

  pthread_t tid;
  if (pthread_create(&tid, NULL, listener, (void*) (intptr_t) fd) != 0)
  {
    close(fd);
    return false;
  }
  pthread_detach(tid);

  COR_V("cor:// listening on port %d", port);
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

static __thread CorConn clientConnV[CLIENT_CONNS_MAX];
static __thread bool    clientConnsInit = false;



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

  int one = 1;
  setsockopt(cP->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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

  if (clientConnsInit == false)
  {
    for (int i = 0; i < CLIENT_CONNS_MAX; i++)
      clientConnV[i].fd = -1;
    clientConnsInit = true;
  }

  CorConn* freeP = NULL;

  for (int i = 0; i < CLIENT_CONNS_MAX; i++)
  {
    if ((clientConnV[i].fd >= 0) && (strcmp(clientConnV[i].peer, key) == 0))
    {
      *reusedP = true;
      return &clientConnV[i];
    }

    if ((clientConnV[i].fd < 0) && (freeP == NULL))
      freeP = &clientConnV[i];
  }

  if (freeP == NULL)                                 // all in use: the first one makes room
  {
    freeP = &clientConnV[0];
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
