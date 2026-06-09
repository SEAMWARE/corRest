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
#include <stdbool.h>

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

  // Request timing
  uint64_t                requestStartTime;      // CLOCK_REALTIME microseconds (for timestamps)
  uint64_t                requestStartTimeMono;  // CLOCK_MONOTONIC microseconds (for duration metrics)

  // User-defined context
  void*                   userData;

  // Async worker pool (6d). The I/O thread suspends the connection and enqueues
  // this state; a worker runs swRestProcessRequest off the I/O thread, sets
  // asyncProcessed, and resumes the connection. asyncNext links the FIFO queue.
  bool                    asyncProcessed;
  struct SwRestState*     asyncNext;
} SwRestState;



// -----------------------------------------------------------------------------
//
// swRest - per-request state, reached through a thread-local pointer
//
// Historically `swRest` was a plain __thread object. It is now a __thread
// POINTER (swRestP) behind the `swRest` macro, so the state can later be
// relocated off the thread (into MHD per-connection con_cls) without touching
// the ~2000 `swRest.foo` call sites. Until a request handler binds swRestP to
// a connection's state, swRestBind() auto-binds it to a per-thread fallback
// object — making every access crash-proof and, for now, behaviourally
// identical to the old __thread object (still exactly one state per thread).
//
extern __thread SwRestState  swRestFallback;   // per-thread fallback storage
extern __thread SwRestState* swRestP;          // current state (NULL until first bound)

static inline SwRestState* swRestBind(void)
{
  if (swRestP == NULL)
    swRestP = &swRestFallback;
  return swRestP;
}

#define swRest (*swRestBind())

#endif  // SWREST_SW_REST_STATE_H_
