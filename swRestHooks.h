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
// Hook setters
//
extern void swRestSetRequestStartHook(SwRestHook fn);
extern void swRestSetPayloadParseHook(SwRestHook fn);
extern void swRestSetPayloadRenderHook(SwRestHook fn);
extern void swRestSetParamHook(SwRestParamHook fn);
extern void swRestSetPreServiceHook(SwRestPreServiceHook fn);
extern void swRestSetPrettySpaces(int spaces);

#endif  // SWREST_SW_REST_HOOKS_H_
