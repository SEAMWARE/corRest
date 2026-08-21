//
// FILE            corRestClient.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP client with connection pooling, TLS, redirects, and concurrent requests.
//
#ifndef CORREST_CLIENT_H_
#define CORREST_CLIENT_H_

#include <stdbool.h>
#include <netdb.h>                              // struct addrinfo
#include <stdint.h>
#include <pthread.h>

#include "kalloc/KAlloc.h"
#include "kjson/KjNode.h"

#include "corRest/CorRestVerb.h"
#include "corRest/CorRestKeyValue.h"
#include "corRest/CorRestIn.h"              // COR_REST_INITIAL_KV_SLOTS, COR_REST_KV_GROW_SIZE



// -----------------------------------------------------------------------------
//
// Client error codes
//
#define CORR_OK                  0
#define CORR_ERR_URL            -1
#define CORR_ERR_CONNECT        -2
#define CORR_ERR_TIMEOUT        -3
#define CORR_ERR_SEND           -4
#define CORR_ERR_RECV           -5
#define CORR_ERR_PARSE          -6
#define CORR_ERR_ALLOC          -7
#define CORR_ERR_TLS            -8
#define CORR_ERR_TOO_MANY_REDIR -9
#define CORR_ERR_CLOSED         -10



// -----------------------------------------------------------------------------
//
// CorRestClientConn - outbound connection
//
typedef struct CorRestClientConn
{
  int                         fd;
  void*                       ssl;
  char                        host[256];
  unsigned short              port;
  bool                        tls;
  bool                        keepAlive;
  uint64_t                    lastUsed;
  char*                       buf;
  int                         bufSize;
  int                         bufLen;

  //
  // The addresses getaddrinfo returned, and the next one to try. A non-blocking
  // connect to a closed port answers EINPROGRESS, not a refusal, so which
  // address works can only be discovered asynchronously - and "localhost"
  // commonly resolves ::1 ahead of 127.0.0.1 while the listener is IPv4-only.
  // Both are freed the moment a connection is established.
  //
  struct addrinfo*            aiList;
  struct addrinfo*            aiNext;

  struct CorRestClientConn*    next;
} CorRestClientConn;



// -----------------------------------------------------------------------------
//
// CorRestClientPool - thread-safe connection pool
//
#define CORR_POOL_BUCKETS  64

typedef struct CorRestClientPool
{
  struct { CorRestClientConn* head; int count; } buckets[CORR_POOL_BUCKETS];
  pthread_mutex_t   mutex;
  int               maxIdlePerHost;
  int               idleTimeoutSec;
  int               totalIdle;
} CorRestClientPool;



// -----------------------------------------------------------------------------
//
// CorRestClientRequest - outbound request description
//
typedef struct CorRestClientRequest
{
  CorRestVerb        verb;
  char*             url;

  // Parsed from URL
  char              scheme[8];
  char              host[256];
  unsigned short    port;
  char*             path;

  // Headers (dynamic array, inline for small counts)
  CorRestKeyValue    headers[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue*   headerV;
  int               headerCount;
  int               headerSize;

  // Body
  char*             body;
  int               bodyLen;
  KjNode*           bodyJson;

  // Config
  int               connectTimeoutMs;
  int               requestTimeoutMs;
  int               maxRedirects;

  // Allocator for response allocation (NULL = use malloc)
  KAlloc*           allocP;
} CorRestClientRequest;



// -----------------------------------------------------------------------------
//
// CorRestClientResponse - response from server
//
typedef struct CorRestClientResponse
{
  int               statusCode;
  char*             statusText;

  CorRestKeyValue    headers[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue*   headerV;
  int               headerCount;
  int               headerSize;

  char*             body;
  int               bodyLen;
  KjNode*           bodyJson;

  int               error;
  char              errorDetail[256];
} CorRestClientResponse;



// -----------------------------------------------------------------------------
//
// CorRestClientMulti - concurrent request engine (opaque)
//
typedef struct CorRestClientMulti CorRestClientMulti;



// -----------------------------------------------------------------------------
//
// Init / Cleanup
//
extern int   corRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent);
extern void  corRestClientCleanup(void);

// Default per-request timeout used by corRestClientRequestInit when the
// caller doesn't override it via corRestClientRequestTimeout. Settable at
// boot (e.g. via coraine's --distOpTimeout/-dtmo CLI flag); applies to
// every HTTP-client call the broker makes (distop forwards, subscription
// notifications, JSON-LD context downloads, …).
extern int   corRestClientDefaultRequestTimeoutMs;

// The User-Agent every client path puts on the wire. Set once at boot via
// corRestClientInit's userAgent parameter; read by both the pool path and the
// multi path, so a process speaks with a single identity.
extern const char* corRestClientUserAgent;



// -----------------------------------------------------------------------------
//
// Synchronous request building
//
extern void  corRestClientRequestInit(CorRestClientRequest* req, CorRestVerb verb, const char* url, KAlloc* allocP);
extern void  corRestClientRequestHeader(CorRestClientRequest* req, const char* name, const char* value);
extern void  corRestClientRequestBody(CorRestClientRequest* req, const char* body, int bodyLen);
extern void  corRestClientRequestJsonBody(CorRestClientRequest* req, KjNode* json);
extern void  corRestClientRequestTimeout(CorRestClientRequest* req, int connectMs, int requestMs);



// -----------------------------------------------------------------------------
//
// Synchronous send
//
extern int   corRestClientSend(CorRestClientRequest* req, CorRestClientResponse* resp);



// -----------------------------------------------------------------------------
//
// Convenience functions
//
extern int   corRestClientGet(const char* url, KAlloc* allocP, KjNode** responseP);
extern int   corRestClientPost(const char* url, KjNode* body, KAlloc* allocP, KjNode** responseP);



// -----------------------------------------------------------------------------
//
// Response helpers
//
extern const char* corRestClientResponseHeader(CorRestClientResponse* resp, const char* name);

// -----------------------------------------------------------------------------
//
// corRestClientResponseCleanup - release a completed response's heap-owned parts
//
// Frees the response header vector if the parser grew it beyond the inline
// array. Body/statusText point into the connection buffer and are NOT freed
// here. Idempotent; call once per completed corRestClientSend.
//
extern void  corRestClientResponseCleanup(CorRestClientResponse* resp);



// -----------------------------------------------------------------------------
//
// Multi (concurrent requests)
//
extern CorRestClientMulti*       corRestClientMultiCreate(int capacity);
extern int                      corRestClientMultiAdd(CorRestClientMulti* multi, CorRestVerb verb, const char* url,
                                                     CorRestKeyValue* headers, int headerCount,
                                                     const char* body, int bodyLen,
                                                     KAlloc* allocP, void* userData);
extern int                      corRestClientMultiPerform(CorRestClientMulti* multi, int timeoutMs);
extern CorRestClientResponse*    corRestClientMultiResponse(CorRestClientMulti* multi, int index);
extern void*                    corRestClientMultiUserData(CorRestClientMulti* multi, int index);
extern void                     corRestClientMultiDestroy(CorRestClientMulti* multi);



// -----------------------------------------------------------------------------
//
// Pool management (used internally, exposed for testing)
//
extern int                corRestClientPoolInit(int maxIdlePerHost, int idleTimeoutSec);
extern void               corRestClientPoolDestroy(void);
extern CorRestClientConn*  corRestClientPoolGet(const char* host, unsigned short port, bool tls);
extern void               corRestClientPoolPut(CorRestClientConn* conn);



// -----------------------------------------------------------------------------
//
// TLS (used internally, exposed for testing)
//
extern int   corRestClientTlsInit(void);
extern void  corRestClientTlsInsecureSet(bool onoff);
extern void  corRestClientTlsCleanup(void);
extern int   corRestClientTlsConnect(CorRestClientConn* conn);
extern int   corRestClientTlsRead(CorRestClientConn* conn, char* buf, int len);
extern int   corRestClientTlsWrite(CorRestClientConn* conn, const char* buf, int len);
extern void  corRestClientTlsClose(CorRestClientConn* conn);

#endif  // CORREST_CLIENT_H_
