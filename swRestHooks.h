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
// Hook setters
//
extern void swRestSetPreDispatchHook(SwRestHook fn);
extern void swRestSetPayloadParseHook(SwRestHook fn);
extern void swRestSetPayloadRenderHook(SwRestHook fn);
extern void swRestSetParamHook(SwRestParamHook fn);
extern void swRestSetPreServiceHook(SwRestPreServiceHook fn);
extern void swRestSetServiceInitHook(SwRestServiceInitHook fn);
extern void swRestSetPostResponseHook(SwRestHook fn);
extern void swRestSetPrettySpaces(int spaces);
extern void swRestSetMaxRequestSize(unsigned long long bytes);



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
