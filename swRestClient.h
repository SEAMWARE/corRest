//
// FILE            swRestClient.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// HTTP client with connection pooling, TLS, redirects, and concurrent requests.
//
#ifndef SWREST_SW_REST_CLIENT_H_
#define SWREST_SW_REST_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "kalloc/KAlloc.h"
#include "kjson/KjNode.h"

#include "swRest/SwRestVerb.h"
#include "swRest/SwRestKeyValue.h"
#include "swRest/SwRestIn.h"              // SW_REST_INITIAL_KV_SLOTS, SW_REST_KV_GROW_SIZE



// -----------------------------------------------------------------------------
//
// Client error codes
//
#define SWC_OK                  0
#define SWC_ERR_URL            -1
#define SWC_ERR_CONNECT        -2
#define SWC_ERR_TIMEOUT        -3
#define SWC_ERR_SEND           -4
#define SWC_ERR_RECV           -5
#define SWC_ERR_PARSE          -6
#define SWC_ERR_ALLOC          -7
#define SWC_ERR_TLS            -8
#define SWC_ERR_TOO_MANY_REDIR -9
#define SWC_ERR_CLOSED         -10



// -----------------------------------------------------------------------------
//
// SwRestClientConn - outbound connection
//
typedef struct SwRestClientConn
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
  struct SwRestClientConn*    next;
} SwRestClientConn;



// -----------------------------------------------------------------------------
//
// SwRestClientPool - thread-safe connection pool
//
#define SWC_POOL_BUCKETS  64

typedef struct SwRestClientPool
{
  struct { SwRestClientConn* head; int count; } buckets[SWC_POOL_BUCKETS];
  pthread_mutex_t   mutex;
  int               maxIdlePerHost;
  int               idleTimeoutSec;
  int               totalIdle;
} SwRestClientPool;



// -----------------------------------------------------------------------------
//
// SwRestClientRequest - outbound request description
//
typedef struct SwRestClientRequest
{
  SwRestVerb        verb;
  char*             url;

  // Parsed from URL
  char              scheme[8];
  char              host[256];
  unsigned short    port;
  char*             path;

  // Headers (dynamic array, inline for small counts)
  SwRestKeyValue    headers[SW_REST_INITIAL_KV_SLOTS];
  SwRestKeyValue*   headerV;
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
} SwRestClientRequest;



// -----------------------------------------------------------------------------
//
// SwRestClientResponse - response from server
//
typedef struct SwRestClientResponse
{
  int               statusCode;
  char*             statusText;

  SwRestKeyValue    headers[SW_REST_INITIAL_KV_SLOTS];
  SwRestKeyValue*   headerV;
  int               headerCount;
  int               headerSize;

  char*             body;
  int               bodyLen;
  KjNode*           bodyJson;

  int               error;
  char              errorDetail[256];
} SwRestClientResponse;



// -----------------------------------------------------------------------------
//
// SwRestClientMulti - concurrent request engine (opaque)
//
typedef struct SwRestClientMulti SwRestClientMulti;



// -----------------------------------------------------------------------------
//
// Init / Cleanup
//
extern int   swRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent);
extern void  swRestClientCleanup(void);

// Default per-request timeout used by swRestClientRequestInit when the
// caller doesn't override it via swRestClientRequestTimeout. Settable at
// boot (e.g. via swBroker's --distOpTimeout/-dtmo CLI flag); applies to
// every HTTP-client call the broker makes (distop forwards, subscription
// notifications, JSON-LD context downloads, …).
extern int   swRestClientDefaultRequestTimeoutMs;



// -----------------------------------------------------------------------------
//
// Synchronous request building
//
extern void  swRestClientRequestInit(SwRestClientRequest* req, SwRestVerb verb, const char* url, KAlloc* allocP);
extern void  swRestClientRequestHeader(SwRestClientRequest* req, const char* name, const char* value);
extern void  swRestClientRequestBody(SwRestClientRequest* req, const char* body, int bodyLen);
extern void  swRestClientRequestJsonBody(SwRestClientRequest* req, KjNode* json);
extern void  swRestClientRequestTimeout(SwRestClientRequest* req, int connectMs, int requestMs);



// -----------------------------------------------------------------------------
//
// Synchronous send
//
extern int   swRestClientSend(SwRestClientRequest* req, SwRestClientResponse* resp);



// -----------------------------------------------------------------------------
//
// Convenience functions
//
extern int   swRestClientGet(const char* url, KAlloc* allocP, KjNode** responseP);
extern int   swRestClientPost(const char* url, KjNode* body, KAlloc* allocP, KjNode** responseP);



// -----------------------------------------------------------------------------
//
// Response helpers
//
extern const char* swRestClientResponseHeader(SwRestClientResponse* resp, const char* name);

// -----------------------------------------------------------------------------
//
// swRestClientResponseCleanup - release a completed response's heap-owned parts
//
// Frees the response header vector if the parser grew it beyond the inline
// array. Body/statusText point into the connection buffer and are NOT freed
// here. Idempotent; call once per completed swRestClientSend.
//
extern void  swRestClientResponseCleanup(SwRestClientResponse* resp);



// -----------------------------------------------------------------------------
//
// Multi (concurrent requests)
//
extern SwRestClientMulti*       swRestClientMultiCreate(int capacity);
extern int                      swRestClientMultiAdd(SwRestClientMulti* multi, SwRestVerb verb, const char* url,
                                                     SwRestKeyValue* headers, int headerCount,
                                                     const char* body, int bodyLen,
                                                     KAlloc* allocP, void* userData);
extern int                      swRestClientMultiPerform(SwRestClientMulti* multi, int timeoutMs);
extern SwRestClientResponse*    swRestClientMultiResponse(SwRestClientMulti* multi, int index);
extern void*                    swRestClientMultiUserData(SwRestClientMulti* multi, int index);
extern void                     swRestClientMultiDestroy(SwRestClientMulti* multi);



// -----------------------------------------------------------------------------
//
// Pool management (used internally, exposed for testing)
//
extern int                swRestClientPoolInit(int maxIdlePerHost, int idleTimeoutSec);
extern void               swRestClientPoolDestroy(void);
extern SwRestClientConn*  swRestClientPoolGet(const char* host, unsigned short port, bool tls);
extern void               swRestClientPoolPut(SwRestClientConn* conn);



// -----------------------------------------------------------------------------
//
// TLS (used internally, exposed for testing)
//
extern int   swRestClientTlsInit(void);
extern void  swRestClientTlsInsecureSet(bool onoff);
extern void  swRestClientTlsCleanup(void);
extern int   swRestClientTlsConnect(SwRestClientConn* conn);
extern int   swRestClientTlsRead(SwRestClientConn* conn, char* buf, int len);
extern int   swRestClientTlsWrite(SwRestClientConn* conn, const char* buf, int len);
extern void  swRestClientTlsClose(SwRestClientConn* conn);

#endif  // SWREST_SW_REST_CLIENT_H_
