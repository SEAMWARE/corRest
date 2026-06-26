//
// FILE            swRestHooks.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_HOOKS_H_
#define SWREST_SW_REST_HOOKS_H_

#include <stdbool.h>



// -----------------------------------------------------------------------------
//
// SwRestHook - generic hook (no args, no return)
//
typedef void (*SwRestHook)(void);



// -----------------------------------------------------------------------------
//
// SwRestParamHook - called for each validated URL parameter
//
typedef void (*SwRestParamHook)(const char* name, const char* value);



// -----------------------------------------------------------------------------
//
// SwRestPreServiceHook - called before service dispatch
//
// Returns true to continue, false to skip the service routine
// (caller must set problemType/statusCode before returning false).
//
typedef bool (*SwRestPreServiceHook)(void);



// -----------------------------------------------------------------------------
//
// SwRestServiceInitHook - called once per expanded SwRestService at init time
// so the embedding library (e.g. swNgsild) can populate service->options with
// per-route flags derived from the URL pattern. Per-request validation then
// reads the cached bits instead of re-scanning the URL on every call.
//
struct SwRestService;
typedef void (*SwRestServiceInitHook)(struct SwRestService* service);



// -----------------------------------------------------------------------------
//
// SwRestUserData hooks - create/destroy the application's per-connection state
// (e.g. swNgsild) alongside the per-connection SwRestState. The allocator runs
// when a connection's state is created (first MHD callback, or in-process
// self-forward setup) and its result is stored in swRest.userData; the
// destructor runs when that state is released. This lets the app hold its
// per-request state per-CONNECTION rather than __thread — required once requests
// are processed off the I/O thread (multiple in-flight per thread).
//
typedef void* (*SwRestUserDataAllocHook)(void);
typedef void  (*SwRestUserDataFreeHook)(void* userData);



// -----------------------------------------------------------------------------
//
// Hook setters
//
extern void swRestSetPreDispatchHook(SwRestHook fn);
extern void swRestSetPayloadParseHook(SwRestHook fn);
extern void swRestSetPayloadRenderHook(SwRestHook fn);
extern void swRestSetParamHook(SwRestParamHook fn);
extern void swRestSetPreServiceHook(SwRestPreServiceHook fn);
extern void swRestSetServiceInitHook(SwRestServiceInitHook fn);
extern void swRestSetPostResponseHook(SwRestHook fn);
extern void swRestSetUserDataHooks(SwRestUserDataAllocHook allocFn, SwRestUserDataFreeHook freeFn);
extern void swRestSetPrettySpaces(int spaces);
extern void swRestSetMaxRequestSize(unsigned long long bytes);

// Accept application/geo+json on body-bearing POST/PUT/PATCH (the § 6.3.4 415
// gate otherwise allows only json/ld+json). OFF for the broker; a notification
// receiver (ftClient) sets it ON to accept geo+json notifications.
extern bool swRestAcceptGeoJsonInput;
extern void swRestAcceptGeoJsonInputSet(bool on);



// -----------------------------------------------------------------------------
//
// SwRestCorsConfig - CORS configuration
//
// Set allowOrigin to "*" for open access, or a specific origin.
// Set to NULL to disable CORS headers entirely (default).
//
typedef struct SwRestCorsConfig
{
  const char*  allowOrigin;       // e.g. "*" or "https://example.com" (NULL = disabled)
  const char*  allowHeaders;      // e.g. "Content-Type, NGSILD-Tenant, Link" (NULL = use default)
  const char*  exposeHeaders;     // e.g. "Location, NGSILD-Results-Count, Link" (NULL = none)
  int          maxAge;            // preflight cache seconds (0 = omit header)
} SwRestCorsConfig;

extern void swRestCorsConfig(const SwRestCorsConfig* config);

#endif  // SWREST_SW_REST_HOOKS_H_
