# corRest — REST Server & HTTP Client Library

A compact REST layer for C: an HTTP **server** built on libmicrohttpd with a
flat, verb-indexed service table and fast wildcard URL matching, plus a full HTTP
**client** (blocking, parallel-multi, connection-pooled, TLS). It is the transport
layer under the coraine NGSI-LD context broker, but has no NGSI-LD dependency of
its own.

- **Version:** 0.1.0
- **Language:** C
- **License:** [Apache License 2.0](LICENSE) — Copyright 2026 Seamware

## Features

- **Service table** — register a flat array of `(verb, url, routine)` services;
  the library splits them by HTTP method into per-verb vectors at init.
- **Fast URL matching** — character-sum hashing and pre-computed wildcard offsets
  avoid `strcmp` on the hot path; supports single (`*`) and greedy (`**`) wildcards.
- **Per-request state** — a thread-local request context (`corRest`) bound for the
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
#include "corRest/corRest.h"
```

### Types

```c
// HTTP method — also the index into the per-verb service vectors (CorRestVerb.h)
typedef enum CorRestVerb {
  CorVerbGet = 0, CorVerbPut, CorVerbPost, CorVerbDelete,
  CorVerbPatch, CorVerbHead, CorVerbOptions
} CorRestVerb;

// A service routine returns true on success (CorRestService.h)
typedef bool (*CorRestServiceRoutine)(void);

// One service entry, as authored by the embedding app/plugin
typedef struct CorRestServiceSimplified {
  CorRestVerb            verb;
  const char*           url;             // path pattern, may contain * / **
  CorRestServiceRoutine  serviceRoutine;
  uint64_t              supportedParams; // bitmask of accepted URL params
  uint64_t              ldOp;            // atomic-op bit (0 for non-NGSI-LD routes)
  CorRestServiceRoutine  payloadCheck;    // optional body validator (NULL = none)
} CorRestServiceSimplified;

// URL-param name → bitmask bit (for supportedParams)
typedef struct CorRestParam { const char* name; uint64_t bit; } CorRestParam;
```

### Server

```c
int  corRestInit(CorRestServiceSimplified serviceV[], int services,
                unsigned short port, int poolSize);
void corRestStop(void);

void corRestStateInit(struct MHD_Connection* connection, const char* url, const char* method);
void corRestStateRelease(void);
```

`corRestInit` expands the flat service array into per-verb vectors and starts the
microhttpd listener on `port`. `corRestStateInit` / `corRestStateRelease` manage the
thread-local per-request state.

### Client

```c
int  corRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent);
void corRestClientCleanup(void);

// Request builder
void corRestClientRequestInit(CorRestClientRequest* req, CorRestVerb verb, const char* url, KAlloc* allocP);
void corRestClientRequestHeader(CorRestClientRequest* req, const char* name, const char* value);
void corRestClientRequestBody(CorRestClientRequest* req, const char* body, int bodyLen);
void corRestClientRequestJsonBody(CorRestClientRequest* req, KjNode* json);
void corRestClientRequestTimeout(CorRestClientRequest* req, int connectMs, int requestMs);
int  corRestClientSend(CorRestClientRequest* req, CorRestClientResponse* resp);

// Convenience one-liners
int  corRestClientGet(const char* url, KAlloc* allocP, KjNode** responseP);
int  corRestClientPost(const char* url, KjNode* body, KAlloc* allocP, KjNode** responseP);

const char* corRestClientResponseHeader(CorRestClientResponse* resp, const char* name);
```

A default per-request timeout is exposed as `int corRestClientDefaultRequestTimeoutMs`.

### Parallel multi-client

```c
CorRestClientMulti*    corRestClientMultiCreate(int capacity);
int                   corRestClientMultiAdd(CorRestClientMulti* multi, CorRestVerb verb, const char* url, /* … */);
int                   corRestClientMultiPerform(CorRestClientMulti* multi, int timeoutMs);
CorRestClientResponse* corRestClientMultiResponse(CorRestClientMulti* multi, int index);
void                  corRestClientMultiDestroy(CorRestClientMulti* multi);
```

A connection pool (`corRestClientPool*`) and an OpenSSL TLS transport
(`corRestClientTls*`) sit underneath for keep-alive and `https://`.

## Usage example

```c
#include "corRest/corRest.h"

static bool pingGet(void)
{
  // … build the response via the CorRestOut API …
  return true;
}

static CorRestServiceSimplified services[] =
{
  { CorVerbGet, "/ping",            pingGet, 0, 0, NULL },
  { CorVerbGet, "/things/*",        thingGet, 0, 0, NULL },   // single-wildcard
};

int main(void)
{
  if (corRestInit(services, sizeof(services)/sizeof(services[0]), 1026, 16) != 0)
    return 1;

  // … serve until shutdown …
  corRestStop();
  return 0;
}
```

## Building

```bash
make            # build libcorRest.a (+ .so) and the test binary
make ci         # clean + install
make di         # debug + install
```

`libcorRest.a` links statically into its consumers. Sibling k-lib repos must be
present (the build references `../<lib>/lib<lib>.a`).

## Dependencies

Sibling k-lib repos (one `.a` each):

- [`kalloc`](https://gitlab.com/kzangeli/kalloc) — arena allocator (`KAlloc`)
- [`kjson`](https://gitlab.com/kzangeli/kjson) — JSON parsing / trees (`KjNode`)
- [`kbase`](https://gitlab.com/kzangeli/kbase) — core utilities
- [`klog`](https://gitlab.com/kzangeli/klog) — logging
- [`ktrace`](https://gitlab.com/kzangeli/ktrace) — trace levels
- [`kprom`](https://gitlab.com/kzangeli/kprom) — Prometheus metrics

System libraries: `libmicrohttpd` (HTTP server), `openssl` (`ssl`/`crypto`, TLS
client), `pthread`, `m`.
