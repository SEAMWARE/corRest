//
// FILE            CorRestState.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef CORREST_STATE_H_
#define CORREST_STATE_H_

#include <microhttpd.h>
#include <stdbool.h>

#include "kalloc/KAlloc.h"
#include "kjson/kjson.h"

#include "corRest/CorRestService.h"
#include "corRest/CorRestIn.h"
#include "corRest/CorRestOut.h"



// -----------------------------------------------------------------------------
//
// CorRestState - thread-local per-request state
//
// This struct holds everything about the current request.
// Stored in a __thread global variable, so no need to pass it around.
//
typedef struct CorRestState
{
  struct MHD_Connection*  mhdConnection;

  // Allocator: pool-based, bulk-free after request completes
  KAlloc                  kalloc;
  char                    kallocBuffer[8 * 1024];   // initial inline buffer

  // JSON parser
  Kjson                   kjson;
  Kjson*                  kjsonP;

  // Incoming request
  CorRestIn                in;

  // Outgoing response
  CorRestOut               out;

  // Matched service
  CorRestService*          serviceP;

  // Payload accumulation (during MHD body reads)
  int                     payloadBufSize;

  // Request timing
  uint64_t                requestStartTime;      // CLOCK_REALTIME microseconds (for timestamps)
  uint64_t                requestStartTimeMono;  // CLOCK_MONOTONIC microseconds (for duration metrics)

  // User-defined context
  void*                   userData;

  // Async worker pool (6d). The I/O thread suspends the connection and enqueues
  // this state; a worker runs corRestProcessRequest off the I/O thread, sets
  // asyncProcessed, and resumes the connection. asyncNext links the FIFO queue.
  bool                    asyncProcessed;
  struct CorRestState*     asyncNext;
} CorRestState;



// -----------------------------------------------------------------------------
//
// corRest - per-request state, reached through a thread-local pointer
//
// Historically `corRest` was a plain __thread object. It is now a __thread
// POINTER (corRestP) behind the `corRest` macro, so the state can later be
// relocated off the thread (into MHD per-connection con_cls) without touching
// the ~2000 `corRest.foo` call sites. Until a request handler binds corRestP to
// a connection's state, corRestBind() auto-binds it to a per-thread fallback
// object — making every access crash-proof and, for now, behaviourally
// identical to the old __thread object (still exactly one state per thread).
//
extern __thread CorRestState  corRestFallback;   // per-thread fallback storage
extern __thread CorRestState* corRestP;          // current state (NULL until first bound)

static inline CorRestState* corRestBind(void)
{
  if (corRestP == NULL)
    corRestP = &corRestFallback;
  return corRestP;
}

#define corRest (*corRestBind())

#endif  // CORREST_STATE_H_
