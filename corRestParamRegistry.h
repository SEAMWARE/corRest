//
// FILE            corRestParamRegistry.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_PARAM_REGISTRY_H_
#define CORREST_PARAM_REGISTRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "corRest/CorRestService.h"       // CorRestParam



// -----------------------------------------------------------------------------
//
// corRestParamInit - Register URL parameters for validation
//
// Builds a hash table mapping parameter names to bitmask values.
// Must be called before corRestInit.  The paramV array is not copied.
//
extern bool corRestParamInit(const CorRestParam* paramV);



// -----------------------------------------------------------------------------
//
// corRestParamAdd - Add parameters to the existing registry (creates on first call)
//
extern bool corRestParamAdd(const CorRestParam* paramV);



// -----------------------------------------------------------------------------
//
// corRestParamLookup - Look up the bitmask for a URL parameter name
//
// Returns the parameter's bit value, or 0 if not found.
//
extern uint64_t corRestParamLookup(const char* name);

#endif  // CORREST_PARAM_REGISTRY_H_
