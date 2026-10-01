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
