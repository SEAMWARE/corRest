//
// FILE            corRestHooks.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_HOOKS_H_
#define CORREST_HOOKS_H_

#include <stdbool.h>



// -----------------------------------------------------------------------------
//
// CorRestHook - generic hook (no args, no return)
//
typedef void (*CorRestHook)(void);



// -----------------------------------------------------------------------------
//
// CorRestParamHook - called for each validated URL parameter
//
typedef void (*CorRestParamHook)(const char* name, const char* value);



// -----------------------------------------------------------------------------
//
// CorRestPreServiceHook - called before service dispatch
//
// Returns true to continue, false to skip the service routine
// (caller must set problemType/statusCode before returning false).
//
typedef bool (*CorRestPreServiceHook)(void);



// -----------------------------------------------------------------------------
//
// CorRestServiceInitHook - called once per expanded CorRestService at init time
// so the embedding library (e.g. corNgsild) can populate service->options with
// per-route flags derived from the URL pattern. Per-request validation then
// reads the cached bits instead of re-scanning the URL on every call.
//
struct CorRestService;
typedef void (*CorRestServiceInitHook)(struct CorRestService* service);



// -----------------------------------------------------------------------------
//
// CorRestUserData hooks - create/destroy the application's per-connection state
// (e.g. corNgsild) alongside the per-connection CorRestState. The allocator runs
// when a connection's state is created (first MHD callback, or in-process
// self-forward setup) and its result is stored in corRest.userData; the
// destructor runs when that state is released. This lets the app hold its
// per-request state per-CONNECTION rather than __thread — required once requests
// are processed off the I/O thread (multiple in-flight per thread).
//
typedef void* (*CorRestUserDataAllocHook)(void);
typedef void  (*CorRestUserDataFreeHook)(void* userData);



// -----------------------------------------------------------------------------
//
// CorRestInlineHook - may this request run on the I/O thread that read it?
//
// Asked once per request, where the backend would otherwise suspend the connection and hand the
// request to a worker. The hand-off exists so that a request that WAITS - a database round trip, a
// distributed operation, an @context download - never stops the other connections of its I/O
// thread. It is also two thread switches per request, and for a request that waits on nothing it
// costs more than the request itself (a corDB retrieve: ~93k cycles with the hop, ~50k without).
//
// The app knows what can wait, corRest does not: true = run it here, false = hand it off as
// before. The request is parsed only as far as the backend got - verb, URL, headers, raw body - and
// the answer must come from those, cheaply. No hook = every request is handed off, as it always was.
// The post-response phase (deferred notifications) is not affected.
//
typedef bool (*CorRestInlineHook)(void);




// -----------------------------------------------------------------------------
//
// CorRestFinishInlineHook - may this request's post-response phase run on the I/O thread?
//
// Asked by the built-in server (corHttp) once the response is on the wire. Its event loop is ONE
// thread, so the phase - deferred notifications above all - normally goes to a worker: a
// notification compacted with an @context the broker hosts itself would otherwise wait for an
// answer only that loop can give. But most requests leave nothing behind for the phase (every read,
// every write no subscription matched), and then the hop to the worker is pure cost: two more thread
// switches per request, measured at a third of the server's throughput.
//
// true = run it here; false = hand it to a worker, as before. Called with the request's state bound
// (corRest). No hook = always a worker. libmicrohttpd runs the phase on its own I/O threads anyway.
//
typedef bool (*CorRestFinishInlineHook)(void);



// -----------------------------------------------------------------------------
//
// CorRestCoroutineHook - may this request, one that can wait, run as a coroutine of the loop?
//
// A coroutine yields where it waits for a socket through corRestWaitFd - corRest's clients, the
// @context download. A wait that is no socket of corRest's - a database driver's own (libmongoc), a
// condition variable (a bridge service's reply) - would stop the whole loop instead. Only the
// application knows which of those a request can meet.
//
// true = a coroutine of the loop that read it; false = as before: a worker (the builtin server), a
// thread of its own for the connection (cor://). Called with the request's state bound. No hook = no
// coroutines.
//
typedef bool (*CorRestCoroutineHook)(void);



// -----------------------------------------------------------------------------
//
// CorRestFinishCoroutineHook - may this request's post-response phase run as a coroutine of the loop?
//
// Asked where CorRestFinishInlineHook said no - the phase has something that can wait (a notification,
// above all). As a coroutine it yields where it waits, and the loop serves the other connections
// meanwhile - an @context the broker hosts itself included, which is why the phase never ran on a
// loop before. The same caveat as CorRestCoroutineHook: a wait that is not a socket of corRest's (a
// database driver's own) would stop the loop; only the application knows which the phase can meet.
//
// true = a coroutine of the loop the response went out on; false = a worker, as before. Called with
// the request's state bound. No hook = always a worker.
//
typedef bool (*CorRestFinishCoroutineHook)(void);



// -----------------------------------------------------------------------------
//
// CorRestUpgradeHook - a request asks to switch the connection to another protocol (WebSocket, ...)
//
// Asked when a request carries an "Upgrade" header, after its headers are in (corRest.in) and before
// anything else is done with it - no service routine, no worker. 'protocol' is the header's value.
//
// To ACCEPT, the application sets the 101 and the protocol's handshake headers (corRestOutHeaderAdd)
// in corRest.out, and returns the function that takes the socket, with *ctxP for it. Once the 101 is
// written, the socket leaves the HTTP server: CorRestUpgradeTake is called - on the server's I/O
// thread - with it, the bytes the client sent behind the request (valid only during the call), and
// the function that closes it. The socket is the application's from then on, until it calls
// closeFn(closeArg) - never close() itself: under libmicrohttpd the socket is released through it.
//
// To REFUSE, it sets an error (corRestProblem) and returns NULL: the request ends as that response.
// No hook = an upgrade request is a request like any other.
//
typedef void (*CorRestUpgradeClose)(void* closeArg);
typedef void (*CorRestUpgradeTake)(int fd, const char* extra, int extraLen, CorRestUpgradeClose closeFn, void* closeArg, void* ctx);
typedef CorRestUpgradeTake (*CorRestUpgradeHook)(const char* protocol, void** ctxP);



// -----------------------------------------------------------------------------
//
// Hook setters
//
extern void corRestSetPreDispatchHook(CorRestHook fn);
//
// corRestSetPrePayloadParseHook - called right BEFORE the request body is parsed, with the
// route already resolved (corRest.serviceP) - the place to configure corRest.corJsonP for
// this body (e.g. its keyF member-name hook). NOT called when there is no body.
//
extern void corRestSetPrePayloadParseHook(CorRestHook fn);
extern void corRestSetPayloadParseHook(CorRestHook fn);
extern void corRestSetPayloadRenderHook(CorRestHook fn);
extern void corRestSetParamHook(CorRestParamHook fn);
extern void corRestSetPreServiceHook(CorRestPreServiceHook fn);
extern void corRestSetServiceInitHook(CorRestServiceInitHook fn);
extern void corRestSetPostResponseHook(CorRestHook fn);
extern void corRestSetInlineHook(CorRestInlineHook fn);
extern void corRestSetUpgradeHook(CorRestUpgradeHook fn);
extern void corRestSetFinishInlineHook(CorRestFinishInlineHook fn);
extern void corRestSetCoroutineHook(CorRestCoroutineHook fn);
extern bool corRestCoroutineAllowed(void);           // the hook's answer for the bound request - false with no hook
extern void corRestSetFinishCoroutineHook(CorRestFinishCoroutineHook fn);
extern bool corRestFinishCoroutineAllowed(void);     // the hook's answer for the bound request - false with no hook
extern void corRestSetUserDataHooks(CorRestUserDataAllocHook allocFn, CorRestUserDataFreeHook freeFn);
extern void corRestSetPrettySpaces(int spaces);
extern void corRestSetMaxRequestSize(unsigned long long bytes);

// Accept application/geo+json on body-bearing POST/PUT/PATCH (the § 6.3.4 415
// gate otherwise allows only json/ld+json). OFF for the broker; a notification
// receiver (ftClient) sets it ON to accept geo+json notifications.
extern bool corRestAcceptGeoJsonInput;
extern void corRestAcceptGeoJsonInputSet(bool on);



// -----------------------------------------------------------------------------
//
// CorRestCorsConfig - CORS configuration
//
// Set allowOrigin to "*" for open access, or a specific origin.
// Set to NULL to disable CORS headers entirely (default).
//
typedef struct CorRestCorsConfig
{
  const char*  allowOrigin;       // e.g. "*" or "https://example.com" (NULL = disabled)
  const char*  allowHeaders;      // e.g. "Content-Type, NGSILD-Tenant, Link" (NULL = use default)
  const char*  exposeHeaders;     // e.g. "Location, NGSILD-Results-Count, Link" (NULL = none)
  int          maxAge;            // preflight cache seconds (0 = omit header)
} CorRestCorsConfig;

extern void corRestCorsConfig(const CorRestCorsConfig* config);

#endif  // CORREST_HOOKS_H_
