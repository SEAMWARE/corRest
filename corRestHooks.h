//
// FILE            corRestHooks.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
// Hook setters
//
extern void corRestSetPreDispatchHook(CorRestHook fn);
extern void corRestSetPayloadParseHook(CorRestHook fn);
extern void corRestSetPayloadRenderHook(CorRestHook fn);
extern void corRestSetParamHook(CorRestParamHook fn);
extern void corRestSetPreServiceHook(CorRestPreServiceHook fn);
extern void corRestSetServiceInitHook(CorRestServiceInitHook fn);
extern void corRestSetPostResponseHook(CorRestHook fn);
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
