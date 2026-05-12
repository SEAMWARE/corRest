//
// FILE            swRestClientMulti.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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

#include "swRest/swRestClient.h"                 // SwRestClientConn, SwRestClientMulti, SwRestClientRequest, SwRestClientResponse
#include "kalloc/KAlloc.h"                       // KAlloc



// Forward declarations from swRestClientParse.c
extern int swRestClientResponseComplete(SwRestClientConn* conn);
extern int swRestClientParseResponse(SwRestClientConn* conn, SwRestClientResponse* resp, KAlloc* allocP);



// -----------------------------------------------------------------------------
//
// Entry states
//
typedef enum SwcMultiState
{
  SwcStateInit = 0,
  SwcStateConnecting,
  SwcStateWriting,
  SwcStateReading,
  SwcStateDone,
} SwcMultiState;



// -----------------------------------------------------------------------------
//
// SwcMultiEntry - per-request state within multi engine
//
typedef struct SwcMultiEntry
{
  SwcMultiState          state;
  SwRestClientConn*      conn;
  SwRestClientRequest    req;
  SwRestClientResponse   resp;
  KAlloc*                allocP;
  void*                  userData;

  char*                  sendBuf;
  int                    sendLen;
  int                    sendPos;

  int                    error;

  bool                   fromPool;
  bool                   retried;
} SwcMultiEntry;



// -----------------------------------------------------------------------------
//
// SwRestClientMulti - the multi engine
//
struct SwRestClientMulti
{
  SwcMultiEntry*  entries;
  int             count;
  int             capacity;
  int             done;
};



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
// swRestClientMultiCreate - Create a multi-request engine
//
SwRestClientMulti* swRestClientMultiCreate(int capacity)
{
  SwRestClientMulti* multi = (SwRestClientMulti*)calloc(1, sizeof(SwRestClientMulti));
  if (multi == NULL)
    return NULL;

  multi->entries = (SwcMultiEntry*)calloc(capacity, sizeof(SwcMultiEntry));
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
// swRestClientMultiAdd - Add a request to the multi engine
//
int swRestClientMultiAdd(SwRestClientMulti* multi, SwRestVerb verb, const char* url,
                         SwRestKeyValue* headers, int headerCount,
                         const char* body, int bodyLen,
                         KAlloc* allocP, void* userData)
{
  if (multi->count >= multi->capacity)
    return -1;

  int idx = multi->count++;
  SwcMultiEntry* entry = &multi->entries[idx];

  memset(entry, 0, sizeof(SwcMultiEntry));

  swRestClientRequestInit(&entry->req, verb, url, allocP);

  for (int i = 0; i < headerCount; i++)
    swRestClientRequestHeader(&entry->req, headers[i].key, headers[i].value);

  if (body != NULL && bodyLen > 0)
    swRestClientRequestBody(&entry->req, body, bodyLen);

  entry->allocP  = allocP;
  entry->userData = userData;
  entry->state    = SwcStateInit;

  memset(&entry->resp, 0, sizeof(SwRestClientResponse));
  entry->resp.headerV    = entry->resp.headers;
  entry->resp.headerSize = SW_REST_INITIAL_KV_SLOTS;

  return idx;
}



// -----------------------------------------------------------------------------
//
// buildSendBuf - Build complete request into a malloc'd buffer
//
static int buildSendBuf(SwcMultiEntry* entry)
{
  SwRestClientRequest* req = &entry->req;
  const char* verbStr  = swRestVerbToString(req->verb);
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

  {
    static const char uaLine[] = "User-Agent: swRest/1.0\r\n";
    int uaLen = (int) (sizeof(uaLine) - 1);
    if (p + uaLen > end) { free(buf); return -1; }
    memcpy(p, uaLine, uaLen);
    p += uaLen;
  }

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
static int startConnect(SwcMultiEntry* entry)
{
  SwRestClientRequest* req = &entry->req;

  bool isTls = (strcmp(req->scheme, "https") == 0) ? true : false;
  SwRestClientConn* conn = swRestClientPoolGet(req->host, req->port, isTls);

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
          swRestClientTlsClose(conn);
        close(conn->fd);
        free(conn->buf);
        free(conn);
        conn = NULL;
      }
    }

    if (conn != NULL)
    {
      entry->conn     = conn;
      entry->state    = SwcStateWriting;
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

  int fd = socket(res->ai_family, SOCK_STREAM, 0);
  if (fd < 0)
  {
    freeaddrinfo(res);
    return -1;
  }

  setNonBlocking(fd);

  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  int r = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);

  if (r < 0 && errno != EINPROGRESS)
  {
    close(fd);
    return -1;
  }

  conn = (SwRestClientConn*)calloc(1, sizeof(SwRestClientConn));
  if (conn == NULL)
  {
    close(fd);
    return -1;
  }

  conn->fd   = fd;
  conn->port = req->port;
  conn->tls  = isTls;
  snprintf(conn->host, sizeof(conn->host), "%s", req->host);

  conn->bufSize = 8192;
  conn->buf     = (char*)malloc(conn->bufSize);
  if (conn->buf == NULL)
  {
    close(fd);
    free(conn);
    return -1;
  }

  conn->keepAlive = true;
  entry->conn     = conn;

  if (r == 0)
    entry->state = SwcStateWriting;
  else
    entry->state = SwcStateConnecting;

  return 0;
}



// -----------------------------------------------------------------------------
//
// finishConnect - Check connect result and do TLS if needed
//
static int finishConnect(SwcMultiEntry* entry)
{
  SwRestClientConn* conn = entry->conn;

  int err = 0;
  socklen_t errLen = sizeof(err);
  getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
  if (err != 0)
    return -1;

  if (conn->tls)
  {
    int flags = fcntl(conn->fd, F_GETFL, 0);
    fcntl(conn->fd, F_SETFL, flags & ~O_NONBLOCK);

    if (swRestClientTlsConnect(conn) != 0)
      return -1;

    fcntl(conn->fd, F_SETFL, flags);
  }

  entry->state = SwcStateWriting;
  return 0;
}



// -----------------------------------------------------------------------------
//
// swRestClientMultiPerform - Execute all pending requests concurrently
//
int swRestClientMultiPerform(SwRestClientMulti* multi, int timeoutMs)
{
  if (multi->count == 0)
    return 0;

  int epollFd = epoll_create1(0);
  if (epollFd < 0)
    return -1;

  uint64_t startTime = nowMs();

  for (int i = 0; i < multi->count; i++)
  {
    SwcMultiEntry* entry = &multi->entries[i];

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
      entry->resp.error = SWC_ERR_URL;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Bad URL scheme");
      entry->state = SwcStateDone;
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
      entry->resp.error = SWC_ERR_ALLOC;
      entry->state = SwcStateDone;
      multi->done++;
      continue;
    }

    if (startConnect(entry) != 0)
    {
      entry->resp.error = SWC_ERR_CONNECT;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
      entry->state = SwcStateDone;
      multi->done++;
      continue;
    }

    struct epoll_event ev;
    ev.data.u32 = (uint32_t)i;

    if (entry->state == SwcStateConnecting)
      ev.events = EPOLLOUT;
    else if (entry->state == SwcStateWriting)
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
        SwcMultiEntry* entry = &multi->entries[i];
        if (entry->state != SwcStateDone)
        {
          entry->resp.error = SWC_ERR_TIMEOUT;
          snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Request timeout");
          entry->state = SwcStateDone;
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
      SwcMultiEntry* entry = &multi->entries[idx];

      if (entry->state == SwcStateDone)
        continue;

      SwRestClientConn* conn = entry->conn;

      switch (entry->state)
      {
        case SwcStateConnecting:
        {
          if (finishConnect(entry) != 0)
          {
            entry->resp.error = SWC_ERR_CONNECT;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
            entry->state = SwcStateDone;
            multi->done++;
            break;
          }

          struct epoll_event ev;
          ev.data.u32 = (uint32_t)idx;
          ev.events   = EPOLLOUT;
          epoll_ctl(epollFd, EPOLL_CTL_MOD, conn->fd, &ev);
          break;
        }

        case SwcStateWriting:
        {
          int remaining = entry->sendLen - entry->sendPos;
          int n;

          if (conn->ssl != NULL)
            n = swRestClientTlsWrite(conn, entry->sendBuf + entry->sendPos, remaining);
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
                swRestClientTlsClose(conn);
              close(conn->fd);
              free(conn->buf);
              free(conn);

              entry->conn     = NULL;
              entry->fromPool = false;
              entry->retried  = true;
              entry->sendPos  = 0;

              if (startConnect(entry) != 0)
              {
                entry->resp.error = SWC_ERR_CONNECT;
                snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
                entry->state = SwcStateDone;
                multi->done++;
                break;
              }

              struct epoll_event ev;
              ev.data.u32 = (uint32_t)idx;
              ev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
              break;
            }

            entry->resp.error = SWC_ERR_SEND;
            entry->state = SwcStateDone;
            multi->done++;
            break;
          }

          entry->sendPos += n;

          if (entry->sendPos >= entry->sendLen)
          {
            entry->state = SwcStateReading;
            conn->bufLen = 0;

            struct epoll_event ev;
            ev.data.u32 = (uint32_t)idx;
            ev.events   = EPOLLIN;
            epoll_ctl(epollFd, EPOLL_CTL_MOD, conn->fd, &ev);
          }
          break;
        }

        case SwcStateReading:
        {
          if (conn->bufSize - conn->bufLen < 4096)
          {
            int newSize = conn->bufSize * 2;
            char* newBuf = (char*)realloc(conn->buf, newSize);
            if (newBuf == NULL)
            {
              entry->resp.error = SWC_ERR_ALLOC;
              entry->state = SwcStateDone;
              multi->done++;
              break;
            }
            conn->buf     = newBuf;
            conn->bufSize = newSize;
          }

          int n;
          if (conn->ssl != NULL)
            n = swRestClientTlsRead(conn, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);
          else
            n = (int)read(conn->fd, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);

          if (n <= 0)
          {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
              break;

            if (conn->bufLen > 0)
            {
              int pr = swRestClientParseResponse(conn, &entry->resp, entry->allocP);
              if (pr == 0)
              {
                entry->state = SwcStateDone;
                multi->done++;
                break;
              }
            }

            if (entry->fromPool && !entry->retried && conn->bufLen == 0)
            {
              epoll_ctl(epollFd, EPOLL_CTL_DEL, conn->fd, NULL);

              if (conn->ssl != NULL)
                swRestClientTlsClose(conn);
              close(conn->fd);
              free(conn->buf);
              free(conn);

              entry->conn     = NULL;
              entry->fromPool = false;
              entry->retried  = true;
              entry->sendPos  = 0;

              if (startConnect(entry) != 0)
              {
                entry->resp.error = SWC_ERR_CONNECT;
                snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
                entry->state = SwcStateDone;
                multi->done++;
                break;
              }

              struct epoll_event ev;
              ev.data.u32 = (uint32_t)idx;
              ev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
              break;
            }

            entry->resp.error = (n == 0) ? SWC_ERR_CLOSED : SWC_ERR_RECV;
            entry->state = SwcStateDone;
            multi->done++;
            break;
          }

          conn->bufLen += n;

          int cr = swRestClientResponseComplete(conn);
          if (cr == 0)
          {
            int pr = swRestClientParseResponse(conn, &entry->resp, entry->allocP);
            if (pr == 0)
            {
              entry->state = SwcStateDone;
              multi->done++;
            }
            else
            {
              entry->resp.error = SWC_ERR_PARSE;
              entry->state = SwcStateDone;
              multi->done++;
            }
          }
          else if (cr < 0)
          {
            entry->resp.error = SWC_ERR_PARSE;
            entry->state = SwcStateDone;
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
    SwcMultiEntry* entry = &multi->entries[i];

    free(entry->sendBuf);
    entry->sendBuf = NULL;

    if (entry->conn != NULL)
    {
      if (entry->resp.error == 0 && entry->conn->keepAlive)
      {
        int flags = fcntl(entry->conn->fd, F_GETFL, 0);
        fcntl(entry->conn->fd, F_SETFL, flags & ~O_NONBLOCK);
        swRestClientPoolPut(entry->conn);
      }
      else
      {
        if (entry->conn->ssl != NULL)
          swRestClientTlsClose(entry->conn);
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
// swRestClientMultiResponse - Get the response for entry at index
//
SwRestClientResponse* swRestClientMultiResponse(SwRestClientMulti* multi, int index)
{
  if (index < 0 || index >= multi->count)
    return NULL;

  return &multi->entries[index].resp;
}



// -----------------------------------------------------------------------------
//
// swRestClientMultiUserData - Get user data for entry at index
//
void* swRestClientMultiUserData(SwRestClientMulti* multi, int index)
{
  if (index < 0 || index >= multi->count)
    return NULL;

  return multi->entries[index].userData;
}



// -----------------------------------------------------------------------------
//
// swRestClientMultiDestroy - Free the multi engine
//
void swRestClientMultiDestroy(SwRestClientMulti* multi)
{
  if (multi == NULL)
    return;

  for (int i = 0; i < multi->count; i++)
  {
    SwcMultiEntry* entry = &multi->entries[i];

    free(entry->sendBuf);

    if (entry->resp.headerV != entry->resp.headers)
      free(entry->resp.headerV);

    if (entry->req.headerV != entry->req.headers)
      free(entry->req.headerV);

    if (entry->conn != NULL)
    {
      if (entry->conn->ssl != NULL)
        swRestClientTlsClose(entry->conn);
      if (entry->conn->fd >= 0)
        close(entry->conn->fd);
      free(entry->conn->buf);
      free(entry->conn);
    }
  }

  free(multi->entries);
  free(multi);
}
