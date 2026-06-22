# swRest — REST Server & HTTP Client Library

A compact REST layer for C: an HTTP **server** built on libmicrohttpd with a
flat, verb-indexed service table and fast wildcard URL matching, plus a full HTTP
**client** (blocking, parallel-multi, connection-pooled, TLS). It is the transport
layer under the swBroker NGSI-LD context broker, but has no NGSI-LD dependency of
its own.

- **Version:** 0.1.0
- **Language:** C
- **License:** Apache License 2.0 — Copyright 2026 Seamware

## Features

- **Service table** — register a flat array of `(verb, url, routine)` services;
  the library splits them by HTTP method into per-verb vectors at init.
- **Fast URL matching** — character-sum hashing and pre-computed wildcard offsets
  avoid `strcmp` on the hot path; supports single (`*`) and greedy (`**`) wildcards.
- **Per-request state** — a thread-local request context (`swRest`) bound for the
  duration of a connection.
- **HTTP client** — request builder, JSON bodies, per-request timeouts, response
  header access; plus one-line `GET`/`POST` convenience wrappers.
- **Parallel multi-client** — fire N requests and harvest them together (used for
  distributed-operation fan-out).
- **Connection pool + TLS** — keep-alive pooling per host and an OpenSSL transport.
- **Prometheus stats** — request counters via `kprom`.

## API reference

The umbrella header pulls in the whole API:

```c
#include "swRest/swRest.h"
```

### Types

```c
// HTTP method — also the index into the per-verb service vectors (SwRestVerb.h)
typedef enum SwRestVerb {
  SwVerbGet = 0, SwVerbPut, SwVerbPost, SwVerbDelete,
  SwVerbPatch, SwVerbHead, SwVerbOptions
} SwRestVerb;

// A service routine returns true on success (SwRestService.h)
typedef bool (*SwRestServiceRoutine)(void);

// One service entry, as authored by the embedding app/plugin
typedef struct SwRestServiceSimplified {
  SwRestVerb            verb;
  const char*           url;             // path pattern, may contain * / **
  SwRestServiceRoutine  serviceRoutine;
  uint64_t              supportedParams; // bitmask of accepted URL params
  uint64_t              ldOp;            // atomic-op bit (0 for non-NGSI-LD routes)
  SwRestServiceRoutine  payloadCheck;    // optional body validator (NULL = none)
} SwRestServiceSimplified;

// URL-param name → bitmask bit (for supportedParams)
typedef struct SwRestParam { const char* name; uint64_t bit; } SwRestParam;
```

### Server

```c
int  swRestInit(SwRestServiceSimplified serviceV[], int services,
                unsigned short port, int poolSize);
void swRestStop(void);

void swRestStateInit(struct MHD_Connection* connection, const char* url, const char* method);
void swRestStateRelease(void);
```

`swRestInit` expands the flat service array into per-verb vectors and starts the
microhttpd listener on `port`. `swRestStateInit` / `swRestStateRelease` manage the
thread-local per-request state.

### Client

```c
int  swRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent);
void swRestClientCleanup(void);

// Request builder
void swRestClientRequestInit(SwRestClientRequest* req, SwRestVerb verb, const char* url, KAlloc* allocP);
void swRestClientRequestHeader(SwRestClientRequest* req, const char* name, const char* value);
void swRestClientRequestBody(SwRestClientRequest* req, const char* body, int bodyLen);
void swRestClientRequestJsonBody(SwRestClientRequest* req, KjNode* json);
void swRestClientRequestTimeout(SwRestClientRequest* req, int connectMs, int requestMs);
int  swRestClientSend(SwRestClientRequest* req, SwRestClientResponse* resp);

// Convenience one-liners
int  swRestClientGet(const char* url, KAlloc* allocP, KjNode** responseP);
int  swRestClientPost(const char* url, KjNode* body, KAlloc* allocP, KjNode** responseP);

const char* swRestClientResponseHeader(SwRestClientResponse* resp, const char* name);
```

A default per-request timeout is exposed as `int swRestClientDefaultRequestTimeoutMs`.

### Parallel multi-client

```c
SwRestClientMulti*    swRestClientMultiCreate(int capacity);
int                   swRestClientMultiAdd(SwRestClientMulti* multi, SwRestVerb verb, const char* url, /* … */);
int                   swRestClientMultiPerform(SwRestClientMulti* multi, int timeoutMs);
SwRestClientResponse* swRestClientMultiResponse(SwRestClientMulti* multi, int index);
void                  swRestClientMultiDestroy(SwRestClientMulti* multi);
```

A connection pool (`swRestClientPool*`) and an OpenSSL TLS transport
(`swRestClientTls*`) sit underneath for keep-alive and `https://`.

## Usage example

```c
#include "swRest/swRest.h"

static bool pingGet(void)
{
  // … build the response via the SwRestOut API …
  return true;
}

static SwRestServiceSimplified services[] =
{
  { SwVerbGet, "/ping",            pingGet, 0, 0, NULL },
  { SwVerbGet, "/things/*",        thingGet, 0, 0, NULL },   // single-wildcard
};

int main(void)
{
  if (swRestInit(services, sizeof(services)/sizeof(services[0]), 1026, 16) != 0)
    return 1;

  // … serve until shutdown …
  swRestStop();
  return 0;
}
```

## Building

```bash
make            # build libswRest.a (+ .so) and the test binary
make ci         # clean + install
make di         # debug + install
```

`libswRest.a` links statically into its consumers. Sibling k-lib repos must be
present (the build references `../<lib>/lib<lib>.a`).

## Dependencies

Sibling k-lib repos (one `.a` each):

- [`kalloc`](../kalloc) — arena allocator (`KAlloc`)
- [`kjson`](../kjson) — JSON parsing / trees (`KjNode`)
- [`kbase`](../kbase) — core utilities
- [`klog`](../klog) — logging
- [`ktrace`](../ktrace) — trace levels
- [`kprom`](../kprom) — Prometheus metrics

System libraries: `libmicrohttpd` (HTTP server), `openssl` (`ssl`/`crypto`, TLS
client), `pthread`, `m`.
