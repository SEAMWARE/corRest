//
// FILE            corRestServiceLookup.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef CORREST_SERVICE_LOOKUP_H_
#define CORREST_SERVICE_LOOKUP_H_

#include "corRest/CorRestService.h"



// -----------------------------------------------------------------------------
//
// corRestServiceLookup -
//
// Looks up a service in the given vector using corRest.in.urlPath.
// On match, sets corRest.in.wildcard[] and returns the service.
//
extern CorRestService* corRestServiceLookup(CorRestServiceVector* serviceV);

#endif  // CORREST_SERVICE_LOOKUP_H_
