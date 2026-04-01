//
// FILE            swRestServiceLookup.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_SERVICE_LOOKUP_H_
#define SWREST_SW_REST_SERVICE_LOOKUP_H_

#include "swRest/SwRestService.h"



// -----------------------------------------------------------------------------
//
// swRestServiceLookup -
//
// Looks up a service in the given vector using swRest.in.urlPath.
// On match, sets swRest.in.wildcard[] and returns the service.
//
extern SwRestService* swRestServiceLookup(SwRestServiceVector* serviceV);

#endif  // SWREST_SW_REST_SERVICE_LOOKUP_H_
