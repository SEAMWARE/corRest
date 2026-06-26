//
// FILE            swRestHooks.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                     // NULL
#include <string.h>                     // memset

#include "swRest/swRestHooks.h"         // Own interface



// -----------------------------------------------------------------------------
//
// Defaults
//
static void hookNoop(void) {}
static bool preServiceHookNoop(void) { return true; }



// -----------------------------------------------------------------------------
//
// Globals (accessed from swRestInit.c during request handling)
//
SwRestHook            swRestPreDispatchHook   = hookNoop;
SwRestHook            swRestPayloadParseHook  = hookNoop;
SwRestHook            swRestPayloadRenderHook = hookNoop;
SwRestParamHook       swRestParamHookF        = NULL;
SwRestPreServiceHook  swRestPreServiceHookF   = preServiceHookNoop;
SwRestHook            swRestPostResponseHook  = hookNoop;
SwRestServiceInitHook swRestServiceInitHookF  = NULL;
SwRestUserDataAllocHook swRestUserDataAllocHookF = NULL;
SwRestUserDataFreeHook  swRestUserDataFreeHookF  = NULL;
int                   swRestDefaultPrettySpaces = 0;
unsigned long long    swRestMaxRequestSize    = 2 * 1024 * 1024;  // § 6.3.4 / § 6.3.2 — 413 threshold; default 2 MiB

// Opt-in: also accept application/geo+json on body-bearing POST/PUT/PATCH (the
// § 6.3.4 415 gate otherwise allows only json/ld+json). OFF for the broker (a
// geo+json entity input is correctly 415'd); a notification RECEIVER (ftClient)
// turns it ON so it can accept geo+json NOTIFICATIONS on its /notify endpoint.
bool                  swRestAcceptGeoJsonInput = false;



// -----------------------------------------------------------------------------
//
// swRestSetPreDispatchHook -
//
// Fires once per request at the START of dispatch (the final MHD callback,
// just before the request is parsed and routed) — NOT at first-byte. That
// keeps the application's per-request state reset, populate and read all
// inside the atomic dispatch, so the epoll pool interleaving another request
// between this one's body-read callbacks can't leave stale state behind.
//
void swRestSetPreDispatchHook(SwRestHook fn)
{
  swRestPreDispatchHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// swRestSetPayloadParseHook -
//
void swRestSetPayloadParseHook(SwRestHook fn)
{
  swRestPayloadParseHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// swRestSetPayloadRenderHook -
//
void swRestSetPayloadRenderHook(SwRestHook fn)
{
  swRestPayloadRenderHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// swRestSetParamHook -
//
void swRestSetParamHook(SwRestParamHook fn)
{
  swRestParamHookF = fn;
}



// -----------------------------------------------------------------------------
//
// swRestSetPreServiceHook -
//
void swRestSetPreServiceHook(SwRestPreServiceHook fn)
{
  swRestPreServiceHookF = (fn != NULL) ? fn : preServiceHookNoop;
}



// -----------------------------------------------------------------------------
//
// swRestSetServiceInitHook -
//
void swRestSetServiceInitHook(SwRestServiceInitHook fn)
{
  swRestServiceInitHookF = fn;
}



// -----------------------------------------------------------------------------
//
// swRestSetPostResponseHook -
//
// Called on the same thread as the request handler, AFTER MHD has delivered
// the response bytes but BEFORE the per-request arena is released. Gives
// the application a chance to run "fire-and-forget" work (like dispatching
// subscription notifications) off the client's critical path while still
// having access to the request-scoped allocations.
//
void swRestSetPostResponseHook(SwRestHook fn)
{
  swRestPostResponseHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// swRestSetUserDataHooks - register create/destroy for per-connection userData.
//
void swRestSetUserDataHooks(SwRestUserDataAllocHook allocFn, SwRestUserDataFreeHook freeFn)
{
  swRestUserDataAllocHookF = allocFn;
  swRestUserDataFreeHookF  = freeFn;
}



// -----------------------------------------------------------------------------
//
// swRestSetPrettySpaces -
//
void swRestSetPrettySpaces(int spaces)
{
  swRestDefaultPrettySpaces = spaces;
}



// -----------------------------------------------------------------------------
//
// swRestAcceptGeoJsonInputSet - accept application/geo+json on POST/PUT/PATCH
//
void swRestAcceptGeoJsonInputSet(bool on)
{
  swRestAcceptGeoJsonInput = on;
}



// -----------------------------------------------------------------------------
//
// swRestSetMaxRequestSize - § 6.3.2 413 threshold (in bytes).
//
// 0 = disable the cap (let MHD's transport-level limits kick in).
//
void swRestSetMaxRequestSize(unsigned long long bytes)
{
  swRestMaxRequestSize = bytes;
}



// -----------------------------------------------------------------------------
//
// swRestCorsConfig -
//
SwRestCorsConfig swRestCors = { NULL, NULL, NULL, 0 };

void swRestCorsConfig(const SwRestCorsConfig* config)
{
  if (config != NULL)
    swRestCors = *config;
  else
    memset(&swRestCors, 0, sizeof(swRestCors));
}
