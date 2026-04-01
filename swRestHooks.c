//
// FILE            swRestHooks.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                     // NULL

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
int                   swRestDefaultPrettySpaces = 0;



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
// swRestSetPrettySpaces -
//
void swRestSetPrettySpaces(int spaces)
{
  swRestDefaultPrettySpaces = spaces;
}
