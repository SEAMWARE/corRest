//
// FILE            swRestParamRegistry.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_PARAM_REGISTRY_H_
#define SWREST_SW_REST_PARAM_REGISTRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "swRest/SwRestService.h"       // SwRestParam



// -----------------------------------------------------------------------------
//
// swRestParamInit - Register URL parameters for validation
//
// Builds a hash table mapping parameter names to bitmask values.
// Must be called before swRestInit.  The paramV array is not copied.
//
extern bool swRestParamInit(const SwRestParam* paramV);



// -----------------------------------------------------------------------------
//
// swRestParamAdd - Add parameters to the existing registry (creates on first call)
//
extern bool swRestParamAdd(const SwRestParam* paramV);



// -----------------------------------------------------------------------------
//
// swRestParamLookup - Look up the bitmask for a URL parameter name
//
// Returns the parameter's bit value, or 0 if not found.
//
extern uint64_t swRestParamLookup(const char* name);

#endif  // SWREST_SW_REST_PARAM_REGISTRY_H_
