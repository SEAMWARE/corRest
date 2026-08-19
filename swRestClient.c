//
// FILE            swRestClient.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// HTTP client with connection pooling, TLS, redirects.
//

#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, malloc, free, atoi
#include <string.h>                              // memset, memcmp, memcpy, strlen, strcmp, strcasecmp, strncasecmp, strncmp, strerror
#include <stdbool.h>                             // bool, true, false
#include <unistd.h>                              // close, read, write
#include <errno.h>                               // errno
#include <fcntl.h>                               // fcntl
#include <poll.h>                                // poll
#include <sys/socket.h>                          // socket, connect, getsockopt
#include <netinet/in.h>                          // sockaddr_in
#include <netinet/tcp.h>                         // TCP_NODELAY
#include <netdb.h>                               // getaddrinfo, freeaddrinfo

#include "swRest/swRestClient.h"                 // SwRestClientConn, SwRestClientRequest, SwRestClientResponse
#include "kalloc/kaAlloc.h"                      // kaAlloc
#include "kjson/KjNode.h"                        // KjNode
#include "kjson/kjRender.h"                      // kjFastRender
#include "kjson/kjRenderSize.h"                  // kjFastRenderSize



// -----------------------------------------------------------------------------
//
// Global pool instance
//
static bool        tlsInited = false;



// -----------------------------------------------------------------------------
//
// swRestClientDefaultRequestTimeoutMs - boot-time default for every client
// request that doesn't call swRestClientRequestTimeout explicitly. swBroker
// exposes this via the --distOpTimeout/-dtmo CLI flag; ETSI runs can dial
// it down to skip the 10 s wait per unmatched HttpCtrl stub forward.
//
int swRestClientDefaultRequestTimeoutMs = 5000;



// -----------------------------------------------------------------------------
//
// swRestClientUserAgent - the User-Agent every client path puts on the wire
//
// Set once at boot by swRestClientInit; the pool path and the multi path both
// read it, so a process speaks with a single identity no matter which path a
// request takes.
//
const char* swRestClientUserAgent = "Cor/1.0";



// -----------------------------------------------------------------------------
//
// swRestClientInit - Initialize the client subsystem
//
int swRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent)
{
  if (userAgent != NULL)
    swRestClientUserAgent = userAgent;

  int s;

  s = swRestClientPoolInit(maxIdleConns > 0 ? maxIdleConns : 4,
                           idleTimeoutSec > 0 ? idleTimeoutSec : 30);
  if (s != 0)
    return s;

  return 0;
}



// -----------------------------------------------------------------------------
//
// swRestClientCleanup - Tear down the client subsystem
//
void swRestClientCleanup(void)
{
  swRestClientPoolDestroy();

  if (tlsInited)
  {
    swRestClientTlsCleanup();
    tlsInited = false;
  }
}



// -----------------------------------------------------------------------------
//
// urlParse - Parse URL into scheme, host, port, path
//
// Parses "http://host:port/path" and "https://host:port/path" by scanning
// the string directly.  Default port 80 for http, 443 for https.
//
static int urlParse(SwRestClientRequest* req)
{
  const char* url = req->url;

  if (url == NULL)
    return SWC_ERR_URL;

  // Parse scheme
  if (strncmp(url, "https://", 8) == 0)
  {
    memcpy(req->scheme, "https", 6);
    url += 8;
    req->port = 443;
  }
  else if (strncmp(url, "http://", 7) == 0)
  {
    memcpy(req->scheme, "http", 5);
    url += 7;
    req->port = 80;
  }
  else
  {
    return SWC_ERR_URL;
  }

  // Parse host[:port]
  const char* hostStart = url;
  const char* p         = url;

  while (*p != '\0' && *p != '/' && *p != '?')
    p++;

  int hostPortLen = (int)(p - hostStart);

  const char* colon = (const char*)memchr(hostStart, ':', hostPortLen);
  if (colon != NULL)
  {
    int hostLen = (int)(colon - hostStart);
    if (hostLen <= 0 || hostLen >= (int)sizeof(req->host))
      return SWC_ERR_URL;

    memcpy(req->host, hostStart, hostLen);
    req->host[hostLen] = '\0';
    req->port = (unsigned short)atoi(colon + 1);
  }
  else
  {
    if (hostPortLen <= 0 || hostPortLen >= (int)sizeof(req->host))
      return SWC_ERR_URL;

    memcpy(req->host, hostStart, hostPortLen);
    req->host[hostPortLen] = '\0';
  }

  // Parse path
  req->path = (*p == '/' || *p == '?') ? (char*)p : (char*)"/";

  return 0;
}



// -----------------------------------------------------------------------------
//
// swRestClientRequestInit - Initialize a request
//
void swRestClientRequestInit(SwRestClientRequest* req, SwRestVerb verb, const char* url, KAlloc* allocP)
{
  memset(req, 0, sizeof(SwRestClientRequest));

  req->verb             = verb;
  req->url              = (char*)url;
  req->headerV          = req->headers;
  req->headerSize       = SW_REST_INITIAL_KV_SLOTS;
  req->connectTimeoutMs = 5000;
  req->requestTimeoutMs = swRestClientDefaultRequestTimeoutMs;
  req->maxRedirects     = 5;
  req->allocP           = allocP;
}



// -----------------------------------------------------------------------------
//
// swRestClientRequestHeader - Add a header to the request
//
void swRestClientRequestHeader(SwRestClientRequest* req, const char* name, const char* value)
{
  if (req->headerCount >= req->headerSize)
  {
    int newSize = req->headerSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV;

    if (req->allocP != NULL)
      newV = (SwRestKeyValue*)kaAlloc(req->allocP, newSize * sizeof(SwRestKeyValue));
    else
      newV = (SwRestKeyValue*)malloc(newSize * sizeof(SwRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, req->headerV, req->headerCount * sizeof(SwRestKeyValue));
    if (req->headerV != req->headers)
      free(req->headerV);

    req->headerV   = newV;
    req->headerSize = newSize;
  }

  req->headerV[req->headerCount].key  = (char*)name;
  req->headerV[req->headerCount].value = (char*)value;
  req->headerCount++;
}



// -----------------------------------------------------------------------------
//
// swRestClientRequestBody - Set request body
//
void swRestClientRequestBody(SwRestClientRequest* req, const char* body, int bodyLen)
{
  req->body    = (char*)body;
  req->bodyLen = bodyLen;
}



// -----------------------------------------------------------------------------
//
// swRestClientRequestJsonBody - Set JSON body (auto-rendered on send)
//
void swRestClientRequestJsonBody(SwRestClientRequest* req, KjNode* json)
{
  req->bodyJson = json;
}



// -----------------------------------------------------------------------------
//
// swRestClientRequestTimeout - Set timeouts
//
void swRestClientRequestTimeout(SwRestClientRequest* req, int connectMs, int requestMs)
{
  if (connectMs > 0)
    req->connectTimeoutMs = connectMs;
  if (requestMs > 0)
    req->requestTimeoutMs = requestMs;
}



// -----------------------------------------------------------------------------
//
// responseInit - Initialize a response struct
//
static void responseInit(SwRestClientResponse* resp)
{
  memset(resp, 0, sizeof(SwRestClientResponse));
  resp->headerV    = resp->headers;
  resp->headerSize = SW_REST_INITIAL_KV_SLOTS;
}



// -----------------------------------------------------------------------------
//
// setError - Set error in response
//
static void setError(SwRestClientResponse* resp, int code, const char* detail)
{
  resp->error = code;
  if (detail != NULL)
    snprintf(resp->errorDetail, sizeof(resp->errorDetail), "%s", detail);
}



// -----------------------------------------------------------------------------
//
// tcpConnect - Non-blocking TCP connect with timeout
//
static int tcpConnect(const char* host, unsigned short port, int timeoutMs)
{
  struct addrinfo  hints;
  struct addrinfo* res    = NULL;
  struct addrinfo* rp     = NULL;
  char             portStr[8];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  snprintf(portStr, sizeof(portStr), "%d", port);

  int r = getaddrinfo(host, portStr, &hints, &res);
  if (r != 0 || res == NULL)
    return -1;

  int fd = -1;

  for (rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    r = connect(fd, rp->ai_addr, rp->ai_addrlen);

    if (r == 0)
    {
      fcntl(fd, F_SETFL, flags);
      break;
    }

    if (errno == EINPROGRESS)
    {
      struct pollfd pfd;
      pfd.fd     = fd;
      pfd.events = POLLOUT;

      r = poll(&pfd, 1, timeoutMs);
      if (r > 0)
      {
        int err = 0;
        socklen_t errLen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
        if (err == 0)
        {
          fcntl(fd, F_SETFL, flags);
          break;
        }
      }
    }

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);

  if (fd < 0)
    return -1;

  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  return fd;
}



// -----------------------------------------------------------------------------
//
// connRead - Read from connection (dispatches plain/TLS)
//
static int connRead(SwRestClientConn* conn, char* buf, int len)
{
  if (conn->ssl != NULL)
    return swRestClientTlsRead(conn, buf, len);

  return (int)read(conn->fd, buf, len);
}



// -----------------------------------------------------------------------------
//
// connWrite - Write to connection (dispatches plain/TLS)
//
static int connWrite(SwRestClientConn* conn, const char* buf, int len)
{
  if (conn->ssl != NULL)
    return swRestClientTlsWrite(conn, buf, len);

  return (int)write(conn->fd, buf, len);
}



// -----------------------------------------------------------------------------
//
// connWriteAll - Write all bytes to connection
//
static int connWriteAll(SwRestClientConn* conn, const char* buf, int len)
{
  int totalSent = 0;

  while (totalSent < len)
  {
    int n = connWrite(conn, buf + totalSent, len - totalSent);
    if (n < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;

    totalSent += n;
  }

  return totalSent;
}



// -----------------------------------------------------------------------------
//
// connEnsureBuf - Ensure read buffer has at least 'needed' extra bytes
//
static int connEnsureBuf(SwRestClientConn* conn, int needed)
{
  if (conn->bufSize - conn->bufLen >= needed)
    return 0;

  int newSize = conn->bufSize * 2;
  if (newSize < conn->bufLen + needed)
    newSize = conn->bufLen + needed;

  char* newBuf = (char*)realloc(conn->buf, newSize);
  if (newBuf == NULL)
    return -1;

  conn->buf     = newBuf;
  conn->bufSize = newSize;

  return 0;
}



// -----------------------------------------------------------------------------
//
// connReadMore - Read more data into conn->buf
//
static int connReadMore(SwRestClientConn* conn, int timeoutMs)
{
  if (connEnsureBuf(conn, 4096) != 0)
    return -1;

  struct pollfd pfd;
  pfd.fd     = conn->fd;
  pfd.events = POLLIN;

  int r = poll(&pfd, 1, timeoutMs);
  if (r <= 0)
    return r == 0 ? -2 : -1;

  int n = connRead(conn, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);
  if (n <= 0)
    return n == 0 ? -3 : -1;

  conn->bufLen += n;
  return n;
}



// Forward declarations
int swRestClientResponseComplete(SwRestClientConn* conn);
int swRestClientParseResponse(SwRestClientConn* conn, SwRestClientResponse* resp, KAlloc* allocP);



// -----------------------------------------------------------------------------
//
// buildRequestBuf - Build HTTP request into a buffer
//
static int buildRequestBuf(SwRestClientRequest* req, char* buf, int bufSize,
                           const char* renderedBody, int renderedBodyLen)
{
  char* p   = buf;
  char* end = buf + bufSize;
  int   n;

  const char* verbStr = swRestVerbToString(req->verb);
  n = snprintf(p, end - p, "%s %s HTTP/1.1\r\n", verbStr, req->path);
  if (n < 0 || n >= end - p)
    return -1;
  p += n;

  if (req->port == 80 || req->port == 443)
    n = snprintf(p, end - p, "Host: %s\r\n", req->host);
  else
    n = snprintf(p, end - p, "Host: %s:%d\r\n", req->host, req->port);
  if (n < 0 || n >= end - p)
    return -1;
  p += n;

  n = snprintf(p, end - p, "User-Agent: %s\r\n", swRestClientUserAgent);
  if (n < 0 || n >= end - p)
    return -1;
  p += n;

  for (int i = 0; i < req->headerCount; i++)
  {
    n = snprintf(p, end - p, "%s: %s\r\n", req->headerV[i].key, req->headerV[i].value);
    if (n < 0 || n >= end - p)
      return -1;
    p += n;
  }

  int bodyLen = renderedBodyLen;
  if (bodyLen > 0)
  {
    n = snprintf(p, end - p, "Content-Length: %d\r\n", bodyLen);
    if (n < 0 || n >= end - p)
      return -1;
    p += n;
  }

  if (end - p < 2)
    return -1;
  *p++ = '\r';
  *p++ = '\n';

  return (int)(p - buf);
}



// -----------------------------------------------------------------------------
//
// swRestClientSendOnce - Send request on a connection, get response (no redirect)
//
static int swRestClientSendOnce(SwRestClientRequest* req, SwRestClientResponse* resp, SwRestClientConn* conn)
{
  char*  renderedBody    = NULL;
  int    renderedBodyLen = 0;

  if (req->bodyJson != NULL)
  {
    int size = kjFastRenderSize(req->bodyJson);
    renderedBody = (char*)malloc(size + 1);
    if (renderedBody == NULL)
    {
      setError(resp, SWC_ERR_ALLOC, "Failed to allocate JSON render buffer");
      return SWC_ERR_ALLOC;
    }
    kjFastRender(req->bodyJson, renderedBody);
    renderedBodyLen = (int)strlen(renderedBody);

    bool hasContentType = false;
    for (int i = 0; i < req->headerCount; i++)
    {
      if (strcasecmp(req->headerV[i].key, "Content-Type") == 0)
      {
        hasContentType = true;
        break;
      }
    }
    if (!hasContentType)
      swRestClientRequestHeader(req, "Content-Type", "application/json");
  }
  else if (req->body != NULL && req->bodyLen > 0)
  {
    renderedBody    = req->body;
    renderedBodyLen = req->bodyLen;
  }

  char headerBuf[8192];
  int  headerLen = buildRequestBuf(req, headerBuf, sizeof(headerBuf),
                                   renderedBody, renderedBodyLen);
  if (headerLen < 0)
  {
    if (req->bodyJson != NULL && renderedBody != NULL)
      free(renderedBody);
    setError(resp, SWC_ERR_ALLOC, "Request too large for header buffer");
    return SWC_ERR_ALLOC;
  }

  if (renderedBody != NULL && renderedBodyLen > 0)
  {
    int   totalLen = headerLen + renderedBodyLen;
    char* sendBuf  = (char*)malloc(totalLen);

    if (sendBuf == NULL)
    {
      if (req->bodyJson != NULL && renderedBody != NULL)
        free(renderedBody);
      setError(resp, SWC_ERR_ALLOC, "Failed to allocate send buffer");
      return SWC_ERR_ALLOC;
    }

    memcpy(sendBuf, headerBuf, headerLen);
    memcpy(sendBuf + headerLen, renderedBody, renderedBodyLen);

    if (req->bodyJson != NULL && renderedBody != NULL)
      free(renderedBody);

    int sendResult = connWriteAll(conn, sendBuf, totalLen);
    free(sendBuf);

    if (sendResult < 0)
    {
      setError(resp, SWC_ERR_SEND, strerror(errno));
      return SWC_ERR_SEND;
    }
  }
  else
  {
    if (connWriteAll(conn, headerBuf, headerLen) < 0)
    {
      setError(resp, SWC_ERR_SEND, strerror(errno));
      return SWC_ERR_SEND;
    }
  }

  // Read response
  conn->bufLen = 0;

  while (1)
  {
    int r = connReadMore(conn, req->requestTimeoutMs);
    if (r == -2)
    {
      setError(resp, SWC_ERR_TIMEOUT, "Request timeout");
      return SWC_ERR_TIMEOUT;
    }
    if (r < 0)
    {
      setError(resp, SWC_ERR_RECV, r == -3 ? "Connection closed" : strerror(errno));
      return r == -3 ? SWC_ERR_CLOSED : SWC_ERR_RECV;
    }

    int completeResult = swRestClientResponseComplete(conn);
    if (completeResult == 0)
      break;
    if (completeResult < 0)
    {
      setError(resp, SWC_ERR_PARSE, "Malformed HTTP response");
      return SWC_ERR_PARSE;
    }
  }

  int parseResult = swRestClientParseResponse(conn, resp, req->allocP);
  if (parseResult != 0)
  {
    setError(resp, SWC_ERR_PARSE, "Malformed HTTP response");
    return SWC_ERR_PARSE;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// connCreate - Create a new connection to host:port
//
static SwRestClientConn* connCreate(const char* host, unsigned short port, bool tls, int timeoutMs)
{
  int fd = tcpConnect(host, port, timeoutMs);
  if (fd < 0)
    return NULL;

  SwRestClientConn* conn = (SwRestClientConn*)calloc(1, sizeof(SwRestClientConn));
  if (conn == NULL)
  {
    close(fd);
    return NULL;
  }

  conn->fd   = fd;
  conn->tls  = tls;
  conn->port = port;
  snprintf(conn->host, sizeof(conn->host), "%s", host);

  conn->bufSize = 8192;
  conn->buf     = (char*)malloc(conn->bufSize);
  if (conn->buf == NULL)
  {
    close(fd);
    free(conn);
    return NULL;
  }

  conn->keepAlive = true;

  if (tls)
  {
    if (!tlsInited)
    {
      if (swRestClientTlsInit() != 0)
      {
        close(fd);
        free(conn->buf);
        free(conn);
        return NULL;
      }
      tlsInited = true;
    }

    if (swRestClientTlsConnect(conn) != 0)
    {
      close(fd);
      free(conn->buf);
      free(conn);
      return NULL;
    }
  }

  return conn;
}



// -----------------------------------------------------------------------------
//
// connDestroy - Close and free a connection
//
static void connDestroy(SwRestClientConn* conn)
{
  if (conn == NULL)
    return;

  if (conn->ssl != NULL)
    swRestClientTlsClose(conn);

  if (conn->fd >= 0)
    close(conn->fd);

  free(conn->buf);
  free(conn);
}



// -----------------------------------------------------------------------------
//
// swRestClientSend - Send request, receive response (with redirects and pooling)
//
int swRestClientSend(SwRestClientRequest* req, SwRestClientResponse* resp)
{
  responseInit(resp);

  int s = urlParse(req);
  if (s != 0)
  {
    setError(resp, SWC_ERR_URL, "Malformed URL");
    return SWC_ERR_URL;
  }

  bool isTls = (strcmp(req->scheme, "https") == 0) ? true : false;

  int redirects = 0;

  while (1)
  {
    SwRestClientConn* conn = swRestClientPoolGet(req->host, req->port, isTls);
    bool fromPool = (conn != NULL) ? true : false;

    if (conn == NULL)
    {
      conn = connCreate(req->host, req->port, isTls, req->connectTimeoutMs);
      if (conn == NULL)
      {
        setError(resp, SWC_ERR_CONNECT, "Connection failed");
        return SWC_ERR_CONNECT;
      }
    }

    s = swRestClientSendOnce(req, resp, conn);

    if (s != 0)
    {
      connDestroy(conn);

      if (fromPool && (s == SWC_ERR_SEND || s == SWC_ERR_RECV || s == SWC_ERR_CLOSED))
      {
        conn = connCreate(req->host, req->port, isTls, req->connectTimeoutMs);
        if (conn == NULL)
        {
          setError(resp, SWC_ERR_CONNECT, "Retry connection failed");
          return SWC_ERR_CONNECT;
        }

        responseInit(resp);
        s = swRestClientSendOnce(req, resp, conn);
        if (s != 0)
        {
          connDestroy(conn);
          return s;
        }
      }
      else
      {
        return s;
      }
    }

    // Check for redirect
    if (resp->statusCode == 301 || resp->statusCode == 302 ||
        resp->statusCode == 307 || resp->statusCode == 308)
    {
      if (++redirects > req->maxRedirects)
      {
        connDestroy(conn);
        setError(resp, SWC_ERR_TOO_MANY_REDIR, "Too many redirects");
        return SWC_ERR_TOO_MANY_REDIR;
      }

      const char* location = swRestClientResponseHeader(resp, "Location");
      if (location == NULL)
      {
        connDestroy(conn);
        setError(resp, SWC_ERR_PARSE, "Redirect without Location header");
        return SWC_ERR_PARSE;
      }

      if (conn->keepAlive)
        swRestClientPoolPut(conn);
      else
        connDestroy(conn);

      if (resp->statusCode == 301 || resp->statusCode == 302)
      {
        req->verb    = SwVerbGet;
        req->body    = NULL;
        req->bodyLen = 0;
        req->bodyJson = NULL;
      }

      req->url = (char*)location;
      s = urlParse(req);
      if (s != 0)
      {
        setError(resp, SWC_ERR_URL, "Malformed redirect URL");
        return SWC_ERR_URL;
      }

      isTls = (strcmp(req->scheme, "https") == 0) ? true : false;

      responseInit(resp);
      continue;
    }

    if (conn->keepAlive)
      swRestClientPoolPut(conn);
    else
      connDestroy(conn);

    break;
  }

  // No auto-parse of JSON responses - caller can parse manually if needed

  return 0;
}



// -----------------------------------------------------------------------------
//
// swRestClientGet - Convenience GET
//
int swRestClientGet(const char* url, KAlloc* allocP, KjNode** responseP)
{
  SwRestClientRequest   req;
  SwRestClientResponse  resp;

  *responseP = NULL;

  swRestClientRequestInit(&req, SwVerbGet, url, allocP);

  int s = swRestClientSend(&req, &resp);
  if (s != 0)
    return s;

  if (resp.bodyJson != NULL)
    *responseP = resp.bodyJson;

  return resp.statusCode;
}



// -----------------------------------------------------------------------------
//
// swRestClientPost - Convenience POST with JSON body
//
int swRestClientPost(const char* url, KjNode* body, KAlloc* allocP, KjNode** responseP)
{
  SwRestClientRequest   req;
  SwRestClientResponse  resp;

  *responseP = NULL;

  swRestClientRequestInit(&req, SwVerbPost, url, allocP);
  if (body != NULL)
    swRestClientRequestJsonBody(&req, body);

  int s = swRestClientSend(&req, &resp);
  if (s != 0)
    return s;

  if (resp.bodyJson != NULL)
    *responseP = resp.bodyJson;

  return resp.statusCode;
}



// -----------------------------------------------------------------------------
//
// swRestClientResponseHeader - Lookup a response header by name (case-insensitive)
//
const char* swRestClientResponseHeader(SwRestClientResponse* resp, const char* name)
{
  int nameLen = strlen(name);

  for (int i = 0; i < resp->headerCount; i++)
  {
    if (resp->headerV[i].key != NULL &&
        strncasecmp(resp->headerV[i].key, name, nameLen) == 0 &&
        resp->headerV[i].key[nameLen] == '\0')
    {
      return resp->headerV[i].value;
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// swRestClientResponseCleanup - release the response's heap-owned parts
//
// The response body and statusText point into the connection's own receive
// buffer (owned by the connection, not the response), so only the header vector
// needs releasing — and only when it outgrew the inline 'headers' array and was
// malloc'd. Idempotent. Call once per completed swRestClientSend, regardless of
// whether the request used an arena (the parser mallocs the grown header vector
// unconditionally).
//
void swRestClientResponseCleanup(SwRestClientResponse* resp)
{
  if (resp == NULL)
    return;

  if ((resp->headerV != NULL) && (resp->headerV != resp->headers))
  {
    free(resp->headerV);
    resp->headerV    = resp->headers;
    resp->headerSize = SW_REST_INITIAL_KV_SLOTS;
    resp->headerCount = 0;
  }
}
