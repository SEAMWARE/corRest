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
SwRestHook            swRestRequestStartHook  = hookNoop;
SwRestHook            swRestPayloadParseHook  = hookNoop;
SwRestHook            swRestPayloadRenderHook = hookNoop;
SwRestParamHook       swRestParamHookF        = NULL;
SwRestPreServiceHook  swRestPreServiceHookF   = preServiceHookNoop;
SwRestHook            swRestPostResponseHook  = hookNoop;
int                   swRestDefaultPrettySpaces = 0;
unsigned long long    swRestMaxRequestSize    = 2 * 1024 * 1024;  // § 6.3.4 / § 6.3.2 — 413 threshold; default 2 MiB



// -----------------------------------------------------------------------------
//
// swRestSetRequestStartHook -
//
void swRestSetRequestStartHook(SwRestHook fn)
{
  swRestRequestStartHook = (fn != NULL) ? fn : hookNoop;
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
// swRestSetPrettySpaces -
//
void swRestSetPrettySpaces(int spaces)
{
  swRestDefaultPrettySpaces = spaces;
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
