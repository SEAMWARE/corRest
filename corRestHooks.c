//
// FILE            corRestHooks.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                     // NULL
#include <string.h>                     // memset

#include "corRest/corRestHooks.h"         // Own interface



// -----------------------------------------------------------------------------
//
// Defaults
//
static void hookNoop(void) {}
static bool preServiceHookNoop(void) { return true; }



// -----------------------------------------------------------------------------
//
// Globals (accessed from corRestInit.c during request handling)
//
CorRestHook            corRestPreDispatchHook   = hookNoop;
CorRestHook            corRestPayloadParseHook  = hookNoop;
CorRestHook            corRestPayloadRenderHook = hookNoop;
CorRestParamHook       corRestParamHookF        = NULL;
CorRestPreServiceHook  corRestPreServiceHookF   = preServiceHookNoop;
CorRestHook            corRestPostResponseHook  = hookNoop;
CorRestServiceInitHook corRestServiceInitHookF  = NULL;
CorRestUserDataAllocHook corRestUserDataAllocHookF = NULL;
CorRestUserDataFreeHook  corRestUserDataFreeHookF  = NULL;
int                   corRestDefaultPrettySpaces = 0;
unsigned long long    corRestMaxRequestSize    = 2 * 1024 * 1024;  // § 6.3.4 / § 6.3.2 — 413 threshold; default 2 MiB

// Opt-in: also accept application/geo+json on body-bearing POST/PUT/PATCH (the
// § 6.3.4 415 gate otherwise allows only json/ld+json). OFF for the broker (a
// geo+json entity input is correctly 415'd); a notification RECEIVER (ftClient)
// turns it ON so it can accept geo+json NOTIFICATIONS on its /notify endpoint.
bool                  corRestAcceptGeoJsonInput = false;



// -----------------------------------------------------------------------------
//
// corRestSetPreDispatchHook -
//
// Fires once per request at the START of dispatch (the final MHD callback,
// just before the request is parsed and routed) — NOT at first-byte. That
// keeps the application's per-request state reset, populate and read all
// inside the atomic dispatch, so the epoll pool interleaving another request
// between this one's body-read callbacks can't leave stale state behind.
//
void corRestSetPreDispatchHook(CorRestHook fn)
{
  corRestPreDispatchHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// corRestSetPayloadParseHook -
//
void corRestSetPayloadParseHook(CorRestHook fn)
{
  corRestPayloadParseHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// corRestSetPayloadRenderHook -
//
void corRestSetPayloadRenderHook(CorRestHook fn)
{
  corRestPayloadRenderHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// corRestSetParamHook -
//
void corRestSetParamHook(CorRestParamHook fn)
{
  corRestParamHookF = fn;
}



// -----------------------------------------------------------------------------
//
// corRestSetPreServiceHook -
//
void corRestSetPreServiceHook(CorRestPreServiceHook fn)
{
  corRestPreServiceHookF = (fn != NULL) ? fn : preServiceHookNoop;
}



// -----------------------------------------------------------------------------
//
// corRestSetServiceInitHook -
//
void corRestSetServiceInitHook(CorRestServiceInitHook fn)
{
  corRestServiceInitHookF = fn;
}



// -----------------------------------------------------------------------------
//
// corRestSetPostResponseHook -
//
// Called on the same thread as the request handler, AFTER MHD has delivered
// the response bytes but BEFORE the per-request arena is released. Gives
// the application a chance to run "fire-and-forget" work (like dispatching
// subscription notifications) off the client's critical path while still
// having access to the request-scoped allocations.
//
void corRestSetPostResponseHook(CorRestHook fn)
{
  corRestPostResponseHook = (fn != NULL) ? fn : hookNoop;
}



// -----------------------------------------------------------------------------
//
// corRestSetUserDataHooks - register create/destroy for per-connection userData.
//
void corRestSetUserDataHooks(CorRestUserDataAllocHook allocFn, CorRestUserDataFreeHook freeFn)
{
  corRestUserDataAllocHookF = allocFn;
  corRestUserDataFreeHookF  = freeFn;
}



// -----------------------------------------------------------------------------
//
// corRestSetPrettySpaces -
//
void corRestSetPrettySpaces(int spaces)
{
  corRestDefaultPrettySpaces = spaces;
}



// -----------------------------------------------------------------------------
//
// corRestAcceptGeoJsonInputSet - accept application/geo+json on POST/PUT/PATCH
//
void corRestAcceptGeoJsonInputSet(bool on)
{
  corRestAcceptGeoJsonInput = on;
}



// -----------------------------------------------------------------------------
//
// corRestSetMaxRequestSize - § 6.3.2 413 threshold (in bytes).
//
// 0 = disable the cap (let MHD's transport-level limits kick in).
//
void corRestSetMaxRequestSize(unsigned long long bytes)
{
  corRestMaxRequestSize = bytes;
}



// -----------------------------------------------------------------------------
//
// corRestCorsConfig -
//
CorRestCorsConfig corRestCors = { NULL, NULL, NULL, 0 };

void corRestCorsConfig(const CorRestCorsConfig* config)
{
  if (config != NULL)
    corRestCors = *config;
  else
    memset(&corRestCors, 0, sizeof(corRestCors));
}
