//
// FILE            corRestClientMulti.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Concurrent multi-request engine for the HTTP client.
//

#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, malloc, free, atoi
#include <string.h>                              // memset, memcmp, memcpy, strlen, strcmp, strncmp
#include <stdbool.h>                             // bool, true, false
#include <unistd.h>                              // close, read, write
#include <errno.h>                               // errno
#include <fcntl.h>                               // fcntl
#include <poll.h>                                // poll
#include <sys/epoll.h>                           // epoll_create1, epoll_ctl, epoll_wait
#include <sys/socket.h>                          // socket, connect, recv
#include <netinet/in.h>                          // sockaddr_in
#include <netinet/tcp.h>                         // TCP_NODELAY
#include <netdb.h>                               // getaddrinfo, freeaddrinfo
#include <time.h>                                // clock_gettime

#include "corRest/corRestClient.h"                 // CorRestClientConn, CorRestClientMulti, CorRestClientRequest, CorRestClientResponse
#include "kalloc/KAlloc.h"                       // KAlloc



// Forward declarations from corRestClientParse.c
extern int corRestClientResponseComplete(CorRestClientConn* conn);
extern int corRestClientParseResponse(CorRestClientConn* conn, CorRestClientResponse* resp, KAlloc* allocP);



// -----------------------------------------------------------------------------
//
// Entry states
//
typedef enum CorrMultiState
{
  CorrStateInit = 0,
  CorrStateConnecting,
  CorrStateWriting,
  CorrStateReading,
  CorrStateDone,
} CorrMultiState;



// -----------------------------------------------------------------------------
//
// CorrMultiEntry - per-request state within multi engine
//
typedef struct CorrMultiEntry
{
  CorrMultiState          state;
  CorRestClientConn*      conn;
  CorRestClientRequest    req;
  CorRestClientResponse   resp;
  KAlloc*                allocP;
  void*                  userData;

  char*                  sendBuf;
  int                    sendLen;
  int                    sendPos;

  int                    error;

  bool                   fromPool;
  bool                   retried;
} CorrMultiEntry;



// -----------------------------------------------------------------------------
//
// CorRestClientMulti - the multi engine
//
struct CorRestClientMulti
{
  CorrMultiEntry*  entries;
  int             count;
  int             capacity;
  int             done;
};



// -----------------------------------------------------------------------------
//
// aiDone - release the address list a connection may still be walking (below)
//
static void aiDone(CorRestClientConn* conn);



// -----------------------------------------------------------------------------
//
// setNonBlocking -
//
static void setNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}



// -----------------------------------------------------------------------------
//
// nowMs -
//
static uint64_t nowMs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiCreate - Create a multi-request engine
//
CorRestClientMulti* corRestClientMultiCreate(int capacity)
{
  CorRestClientMulti* multi = (CorRestClientMulti*)calloc(1, sizeof(CorRestClientMulti));
  if (multi == NULL)
    return NULL;

  multi->entries = (CorrMultiEntry*)calloc(capacity, sizeof(CorrMultiEntry));
  if (multi->entries == NULL)
  {
    free(multi);
    return NULL;
  }

  multi->capacity = capacity;
  return multi;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiAdd - Add a request to the multi engine
//
int corRestClientMultiAdd(CorRestClientMulti* multi, CorRestVerb verb, const char* url,
                         CorRestKeyValue* headers, int headerCount,
                         const char* body, int bodyLen,
                         KAlloc* allocP, void* userData)
{
  if (multi->count >= multi->capacity)
    return -1;

  int idx = multi->count++;
  CorrMultiEntry* entry = &multi->entries[idx];

  memset(entry, 0, sizeof(CorrMultiEntry));

  corRestClientRequestInit(&entry->req, verb, url, allocP);

  for (int i = 0; i < headerCount; i++)
    corRestClientRequestHeader(&entry->req, headers[i].key, headers[i].value);

  if (body != NULL && bodyLen > 0)
    corRestClientRequestBody(&entry->req, body, bodyLen);

  entry->allocP  = allocP;
  entry->userData = userData;
  entry->state    = CorrStateInit;

  memset(&entry->resp, 0, sizeof(CorRestClientResponse));
  entry->resp.headerV    = entry->resp.headers;
  entry->resp.headerSize = COR_REST_INITIAL_KV_SLOTS;

  return idx;
}



// -----------------------------------------------------------------------------
//
// buildSendBuf - Build complete request into a malloc'd buffer
//
static int buildSendBuf(CorrMultiEntry* entry)
{
  CorRestClientRequest* req = &entry->req;
  const char* verbStr  = corRestVerbToString(req->verb);
  int bodyLen          = req->bodyLen;

  int estimate = 512;
  for (int i = 0; i < req->headerCount; i++)
    estimate += (int)strlen(req->headerV[i].key) + (int)strlen(req->headerV[i].value) + 4;
  estimate += bodyLen;

  char* buf = (char*)malloc(estimate);
  if (buf == NULL)
    return -1;

  char* p   = buf;
  char* end = buf + estimate;
  int   n;

  n = snprintf(p, end - p, "%s %s HTTP/1.1\r\n", verbStr, req->path);
  if (n < 0 || n >= end - p) { free(buf); return -1; }
  p += n;

  if (req->port == 80 || req->port == 443)
    n = snprintf(p, end - p, "Host: %s\r\n", req->host);
  else
    n = snprintf(p, end - p, "Host: %s:%d\r\n", req->host, req->port);
  if (n < 0 || n >= end - p) { free(buf); return -1; }
  p += n;

  n = snprintf(p, end - p, "User-Agent: %s\r\n", corRestClientUserAgent);
  if (n < 0 || n >= end - p) { free(buf); return -1; }
  p += n;

  for (int i = 0; i < req->headerCount; i++)
  {
    n = snprintf(p, end - p, "%s: %s\r\n", req->headerV[i].key, req->headerV[i].value);
    if (n < 0 || n >= end - p) { free(buf); return -1; }
    p += n;
  }

  if (bodyLen > 0)
  {
    n = snprintf(p, end - p, "Content-Length: %d\r\n", bodyLen);
    if (n < 0 || n >= end - p) { free(buf); return -1; }
    p += n;
  }

  if (end - p < 2) { free(buf); return -1; }
  *p++ = '\r';
  *p++ = '\n';

  if (bodyLen > 0 && req->body != NULL)
  {
    if (end - p < bodyLen) { free(buf); return -1; }
    memcpy(p, req->body, bodyLen);
    p += bodyLen;
  }

  entry->sendBuf = buf;
  entry->sendLen = (int)(p - buf);
  entry->sendPos = 0;

  return 0;
}



// -----------------------------------------------------------------------------
//
// startConnect - Initiate non-blocking TCP connect
//
static int startConnect(CorrMultiEntry* entry)
{
  CorRestClientRequest* req = &entry->req;

  bool isTls = (strcmp(req->scheme, "https") == 0) ? true : false;
  CorRestClientConn* conn = corRestClientPoolGet(req->host, req->port, isTls);

  if (conn != NULL)
  {
    struct pollfd pfd;
    pfd.fd     = conn->fd;
    pfd.events = POLLIN;

    if (poll(&pfd, 1, 0) > 0)
    {
      char c;
      int  n = (int)recv(conn->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);

      if (n <= 0)
      {
        if (conn->ssl != NULL)
          corRestClientTlsClose(conn);
        close(conn->fd);
        free(conn->buf);
        aiDone(conn);
        free(conn);
        conn = NULL;
      }
    }

    if (conn != NULL)
    {
      entry->conn     = conn;
      entry->state    = CorrStateWriting;
      entry->fromPool = true;
      setNonBlocking(conn->fd);
      return 0;
    }
  }

  struct addrinfo  hints;
  struct addrinfo* res = NULL;
  char             portStr[8];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portStr, sizeof(portStr), "%d", req->port);

  if (getaddrinfo(req->host, portStr, &hints, &res) != 0 || res == NULL)
    return -1;

  //
  // EVERY address, not just the first. AF_UNSPEC means getaddrinfo may answer
  // with IPv6 ahead of IPv4 - "localhost" commonly resolves ::1 first - and a
  // listener bound to IPv4 only then refuses that address instantly. Taking the
  // first result and giving up turns a perfectly reachable endpoint into
  // "Connection failed", which is exactly what happened the first time these
  // tests ran somewhere other than a workstation: every distributed operation to
  // localhost failed while the same endpoint answered curl.
  //
  // The blocking client (corRestClient.c) has always walked the list; this one
  // did not. Note the asymmetry with connect() here being non-blocking: a
  // candidate that returns EINPROGRESS is ACCEPTED, and a genuine failure only
  // shows later - so this loop can only skip addresses that fail immediately,
  // which is precisely the ::1-refused case it exists for.
  //
  int              fd = -1;
  int              r  = -1;    // 0 = connected outright, EINPROGRESS = still connecting
  struct addrinfo* rp;

  for (rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    setNonBlocking(fd);

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    r = connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (r == 0 || errno == EINPROGRESS)
      break;

    close(fd);
    fd = -1;
  }

  if (fd < 0)
  {
    freeaddrinfo(res);
    return -1;
  }

  conn = (CorRestClientConn*)calloc(1, sizeof(CorRestClientConn));
  if (conn == NULL)
  {
    close(fd);
    return -1;
  }

  conn->fd     = fd;
  conn->aiList = res;                       // owned by the conn from here
  conn->aiNext = (rp != NULL) ? rp->ai_next : NULL;
  conn->port   = req->port;
  conn->tls  = isTls;
  snprintf(conn->host, sizeof(conn->host), "%s", req->host);

  conn->bufSize = 8192;
  conn->buf     = (char*)malloc(conn->bufSize);
  if (conn->buf == NULL)
  {
    close(fd);
    aiDone(conn);
    free(conn);
    return -1;
  }

  conn->keepAlive = true;
  entry->conn     = conn;

  if (r == 0)
    entry->state = CorrStateWriting;
  else
    entry->state = CorrStateConnecting;

  return 0;
}



// -----------------------------------------------------------------------------
//
// aiDone - release the address list a connection was still walking
//
static void aiDone(CorRestClientConn* conn)
{
  if (conn->aiList != NULL)
  {
    freeaddrinfo(conn->aiList);
    conn->aiList = NULL;
    conn->aiNext = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// connectNextAddress - the previous candidate failed; try the one after it
//
// Returns the new fd, or -1 when the list is exhausted. The caller re-arms epoll
// on the new descriptor: it is a different socket, and the old one is gone.
//
static int connectNextAddress(CorRestClientConn* conn)
{
  while (conn->aiNext != NULL)
  {
    struct addrinfo* rp = conn->aiNext;
    conn->aiNext = rp->ai_next;

    int fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    setNonBlocking(fd);

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    int r = connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (r == 0 || errno == EINPROGRESS)
    {
      close(conn->fd);
      conn->fd = fd;
      return fd;
    }

    close(fd);
  }

  return -1;
}


// -----------------------------------------------------------------------------
//
// finishConnect - Check connect result and do TLS if needed
//
static int finishConnect(CorrMultiEntry* entry)
{
  CorRestClientConn* conn = entry->conn;

  int err = 0;
  socklen_t errLen = sizeof(err);
  getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
  if (err != 0)
    return -1;

  if (conn->tls)
  {
    int flags = fcntl(conn->fd, F_GETFL, 0);
    fcntl(conn->fd, F_SETFL, flags & ~O_NONBLOCK);

    if (corRestClientTlsConnect(conn) != 0)
      return -1;

    fcntl(conn->fd, F_SETFL, flags);
  }

  entry->state = CorrStateWriting;
  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiPerform - Execute all pending requests concurrently
//
int corRestClientMultiPerform(CorRestClientMulti* multi, int timeoutMs)
{
  if (multi->count == 0)
    return 0;

  int epollFd = epoll_create1(0);
  if (epollFd < 0)
    return -1;

  uint64_t startTime = nowMs();

  for (int i = 0; i < multi->count; i++)
  {
    CorrMultiEntry* entry = &multi->entries[i];

    const char* url = entry->req.url;
    if (strncmp(url, "https://", 8) == 0)
    {
      memcpy(entry->req.scheme, "https", 6);
      url += 8;
      entry->req.port = 443;
    }
    else if (strncmp(url, "http://", 7) == 0)
    {
      memcpy(entry->req.scheme, "http", 5);
      url += 7;
      entry->req.port = 80;
    }
    else
    {
      entry->resp.error = CORR_ERR_URL;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Bad URL scheme");
      entry->state = CorrStateDone;
      multi->done++;
      continue;
    }

    const char* hostStart = url;
    const char* p = url;
    while (*p != '\0' && *p != '/' && *p != '?')
      p++;

    int hostPortLen = (int)(p - hostStart);
    const char* colon = (const char*)memchr(hostStart, ':', hostPortLen);
    if (colon != NULL)
    {
      int hostLen = (int)(colon - hostStart);
      if (hostLen > 0 && hostLen < (int)sizeof(entry->req.host))
      {
        memcpy(entry->req.host, hostStart, hostLen);
        entry->req.host[hostLen] = '\0';
        entry->req.port = (unsigned short)atoi(colon + 1);
      }
    }
    else
    {
      if (hostPortLen > 0 && hostPortLen < (int)sizeof(entry->req.host))
      {
        memcpy(entry->req.host, hostStart, hostPortLen);
        entry->req.host[hostPortLen] = '\0';
      }
    }

    entry->req.path = (*p == '/' || *p == '?') ? (char*)p : (char*)"/";

    if (buildSendBuf(entry) != 0)
    {
      entry->resp.error = CORR_ERR_ALLOC;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
               "Failed to build send buffer");
      entry->state = CorrStateDone;
      multi->done++;
      continue;
    }

    if (startConnect(entry) != 0)
    {
      entry->resp.error = CORR_ERR_CONNECT;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
      entry->state = CorrStateDone;
      multi->done++;
      continue;
    }

    struct epoll_event ev;
    ev.data.u32 = (uint32_t)i;

    if (entry->state == CorrStateConnecting)
      ev.events = EPOLLOUT;
    else if (entry->state == CorrStateWriting)
      ev.events = EPOLLOUT;
    else
      ev.events = EPOLLIN;

    epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
  }

  // Event loop
  struct epoll_event events[64];

  while (multi->done < multi->count)
  {
    uint64_t elapsed = nowMs() - startTime;
    if ((int)elapsed >= timeoutMs)
    {
      for (int i = 0; i < multi->count; i++)
      {
        CorrMultiEntry* entry = &multi->entries[i];
        if (entry->state != CorrStateDone)
        {
          entry->resp.error = CORR_ERR_TIMEOUT;
          snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Request timeout");
          entry->state = CorrStateDone;
          multi->done++;
        }
      }
      break;
    }

    int waitMs = timeoutMs - (int)elapsed;
    if (waitMs > 1000)
      waitMs = 1000;

    int nev = epoll_wait(epollFd, events, 64, waitMs);

    for (int e = 0; e < nev; e++)
    {
      int idx = (int)events[e].data.u32;
      CorrMultiEntry* entry = &multi->entries[idx];

      if (entry->state == CorrStateDone)
        continue;

      CorRestClientConn* conn = entry->conn;

      switch (entry->state)
      {
        case CorrStateConnecting:
        {
          if (finishConnect(entry) != 0)
          {
            //
            // This candidate refused. A non-blocking connect answers EINPROGRESS
            // even for a port nobody listens on, so the choice of address can
            // only be judged here - and "localhost" resolves ::1 before
            // 127.0.0.1 on most systems while a server may be listening on IPv4
            // alone. Try the next address before calling the endpoint dead.
            //
            int newFd = connectNextAddress(conn);

            if (newFd >= 0)
            {
              struct epoll_event nev;
              nev.data.u32 = (uint32_t)idx;
              nev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, newFd, &nev);
              break;                       // still CorrStateConnecting
            }

            aiDone(conn);
            entry->resp.error = CORR_ERR_CONNECT;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          aiDone(conn);                    // connected: the rest of the list is moot

          struct epoll_event ev;
          ev.data.u32 = (uint32_t)idx;
          ev.events   = EPOLLOUT;
          epoll_ctl(epollFd, EPOLL_CTL_MOD, conn->fd, &ev);
          break;
        }

        case CorrStateWriting:
        {
          int remaining = entry->sendLen - entry->sendPos;
          int n;

          if (conn->ssl != NULL)
            n = corRestClientTlsWrite(conn, entry->sendBuf + entry->sendPos, remaining);
          else
            n = (int)write(conn->fd, entry->sendBuf + entry->sendPos, remaining);

          if (n < 0)
          {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;

            if (entry->fromPool && !entry->retried)
            {
              epoll_ctl(epollFd, EPOLL_CTL_DEL, conn->fd, NULL);

              if (conn->ssl != NULL)
                corRestClientTlsClose(conn);
              close(conn->fd);
              free(conn->buf);
              aiDone(conn);
              free(conn);

              entry->conn     = NULL;
              entry->fromPool = false;
              entry->retried  = true;
              entry->sendPos  = 0;

              if (startConnect(entry) != 0)
              {
                entry->resp.error = CORR_ERR_CONNECT;
                snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }

              struct epoll_event ev;
              ev.data.u32 = (uint32_t)idx;
              ev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
              break;
            }

            entry->resp.error = CORR_ERR_SEND;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "send() failed (errno=%d %s)", errno, strerror(errno));
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          entry->sendPos += n;

          if (entry->sendPos >= entry->sendLen)
          {
            entry->state = CorrStateReading;
            conn->bufLen = 0;

            struct epoll_event ev;
            ev.data.u32 = (uint32_t)idx;
            ev.events   = EPOLLIN;
            epoll_ctl(epollFd, EPOLL_CTL_MOD, conn->fd, &ev);
          }
          break;
        }

        case CorrStateReading:
        {
          if (conn->bufSize - conn->bufLen < 4096)
          {
            int newSize = conn->bufSize * 2;
            char* newBuf = (char*)realloc(conn->buf, newSize);
            if (newBuf == NULL)
            {
              entry->resp.error = CORR_ERR_ALLOC;
              snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                       "Failed to grow recv buffer (size=%d)", newSize);
              entry->state = CorrStateDone;
              multi->done++;
              break;
            }
            conn->buf     = newBuf;
            conn->bufSize = newSize;
          }

          int n;
          if (conn->ssl != NULL)
            n = corRestClientTlsRead(conn, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);
          else
            n = (int)read(conn->fd, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);

          if (n <= 0)
          {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
              break;

            if (conn->bufLen > 0)
            {
              int pr = corRestClientParseResponse(conn, &entry->resp, entry->allocP);
              if (pr == 0)
              {
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }
            }

            if (entry->fromPool && !entry->retried && conn->bufLen == 0)
            {
              epoll_ctl(epollFd, EPOLL_CTL_DEL, conn->fd, NULL);

              if (conn->ssl != NULL)
                corRestClientTlsClose(conn);
              close(conn->fd);
              free(conn->buf);
              aiDone(conn);
              free(conn);

              entry->conn     = NULL;
              entry->fromPool = false;
              entry->retried  = true;
              entry->sendPos  = 0;

              if (startConnect(entry) != 0)
              {
                entry->resp.error = CORR_ERR_CONNECT;
                snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }

              struct epoll_event ev;
              ev.data.u32 = (uint32_t)idx;
              ev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
              break;
            }

            // Peer closed cleanly (n == 0): for HTTP/1.0-with-Connection-close
            // framing (no Content-Length, no chunked) EOF is the legitimate
            // body terminator — what's in the buffer IS the complete response.
            // Try to parse it before declaring an error; if the parser
            // succeeds (it knows about the !keepAlive case), succeed too.
            if (n == 0 && conn->bufLen > 0)
            {
              int pr = corRestClientParseResponse(conn, &entry->resp, entry->allocP);
              if (pr == 0)
              {
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }
              // pr != 0 → genuinely truncated/malformed; fall through to error
            }

            entry->resp.error = (n == 0) ? CORR_ERR_CLOSED : CORR_ERR_RECV;
            // include the head of the buffer (CR/LF replaced with | and ~)
            // so the trace stays single-line yet shows HTTP framing.
            char buf[256];
            int  dumpLen = conn->bufLen < 200 ? conn->bufLen : 200;
            int  j = 0;
            for (int k = 0; k < dumpLen && j < (int)sizeof(buf)-1; k++)
            {
              char c = conn->buf[k];
              buf[j++] = (c == '\r') ? '|' : (c == '\n') ? '~' : c;
            }
            buf[j] = 0;
            //
            // The precisions are what make this fit: errorDetail is 256 bytes and
            // the head alone can be 255, so without them the compiler is right to
            // warn that the tail of the message may be lost. Bounded, the worst
            // case is a truncated head - which is what a "head" is for anyway.
            //
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "%s (n=%d, errno=%d %.40s, bufLen=%d, head=%.120s)",
                     (n == 0) ? "Connection closed by peer" : "recv() failed",
                     n, errno, strerror(errno), conn->bufLen, buf);
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          conn->bufLen += n;

          int cr = corRestClientResponseComplete(conn);
          if (cr == 0)
          {
            int pr = corRestClientParseResponse(conn, &entry->resp, entry->allocP);
            if (pr == 0)
            {
              entry->state = CorrStateDone;
              multi->done++;
            }
            else
            {
              entry->resp.error = CORR_ERR_PARSE;
              snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                       "Failed to parse response");
              entry->state = CorrStateDone;
              multi->done++;
            }
          }
          else if (cr < 0)
          {
            entry->resp.error = CORR_ERR_PARSE;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "Malformed HTTP framing");
            entry->state = CorrStateDone;
            multi->done++;
          }
          break;
        }

        default:
          break;
      }
    }
  }

  // Cleanup
  for (int i = 0; i < multi->count; i++)
  {
    CorrMultiEntry* entry = &multi->entries[i];

    free(entry->sendBuf);
    entry->sendBuf = NULL;

    if (entry->conn != NULL)
    {
      if (entry->resp.error == 0 && entry->conn->keepAlive)
      {
        int flags = fcntl(entry->conn->fd, F_GETFL, 0);
        fcntl(entry->conn->fd, F_SETFL, flags & ~O_NONBLOCK);
        corRestClientPoolPut(entry->conn);
      }
      else
      {
        if (entry->conn->ssl != NULL)
          corRestClientTlsClose(entry->conn);
        if (entry->conn->fd >= 0)
          close(entry->conn->fd);
        free(entry->conn->buf);
        free(entry->conn);
      }
      entry->conn = NULL;
    }
  }

  close(epollFd);

  return multi->done;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiResponse - Get the response for entry at index
//
CorRestClientResponse* corRestClientMultiResponse(CorRestClientMulti* multi, int index)
{
  if (index < 0 || index >= multi->count)
    return NULL;

  return &multi->entries[index].resp;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiUserData - Get user data for entry at index
//
void* corRestClientMultiUserData(CorRestClientMulti* multi, int index)
{
  if (index < 0 || index >= multi->count)
    return NULL;

  return multi->entries[index].userData;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiDestroy - Free the multi engine
//
void corRestClientMultiDestroy(CorRestClientMulti* multi)
{
  if (multi == NULL)
    return;

  for (int i = 0; i < multi->count; i++)
  {
    CorrMultiEntry* entry = &multi->entries[i];

    free(entry->sendBuf);

    if (entry->resp.headerV != entry->resp.headers)
      free(entry->resp.headerV);

    if (entry->req.headerV != entry->req.headers)
      free(entry->req.headerV);

    if (entry->conn != NULL)
    {
      if (entry->conn->ssl != NULL)
        corRestClientTlsClose(entry->conn);
      if (entry->conn->fd >= 0)
        close(entry->conn->fd);
      free(entry->conn->buf);
      free(entry->conn);
    }
  }

  free(multi->entries);
  free(multi);
}
