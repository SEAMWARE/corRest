//
// FILE            SwRestState.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_STATE_H_
#define SWREST_SW_REST_STATE_H_

#include <microhttpd.h>

#include "kalloc/KAlloc.h"
#include "kjson/kjson.h"

#include "swRest/SwRestService.h"
#include "swRest/SwRestIn.h"
#include "swRest/SwRestOut.h"



// -----------------------------------------------------------------------------
//
// SwRestState - thread-local per-request state
//
// This struct holds everything about the current request.
// Stored in a __thread global variable, so no need to pass it around.
//
typedef struct SwRestState
{
  struct MHD_Connection*  mhdConnection;

  // Allocator: pool-based, bulk-free after request completes
  KAlloc                  kalloc;
  char                    kallocBuffer[8 * 1024];   // initial inline buffer

  // JSON parser
  Kjson                   kjson;
  Kjson*                  kjsonP;

  // Incoming request
  SwRestIn                in;

  // Outgoing response
  SwRestOut               out;

  // Matched service
  SwRestService*          serviceP;

  // Payload accumulation (during MHD body reads)
  int                     payloadBufSize;

  // Request timing (microseconds since epoch)
  uint64_t                requestStartTime;

  // User-defined context
  void*                   userData;
} SwRestState;



// -----------------------------------------------------------------------------
//
// swRest - the thread-local state variable
//
extern __thread SwRestState swRest;

#endif  // SWREST_SW_REST_STATE_H_
