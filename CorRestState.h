//
// FILE            CorRestState.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_STATE_H_
#define CORREST_STATE_H_

#include <stdbool.h>
#include <stddef.h>                       // NULL - came in via microhttpd.h until this header stopped including it

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
  //
  // The HTTP backend's handle for the connection this request came in on -
  // an `struct MHD_Connection*` under libmicrohttpd, a `CorHttpConn*` under the
  // built-in server. Opaque HERE on purpose: this header is included by ~2000
  // call sites in the layers above, and naming one server's type in it would
  // make every one of them depend on that server being the one in the build.
  // The backend that put it here is the only code that casts it back.
  //
  void*                   connection;

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

  // Payload accumulation (during the backend's body reads)
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

  //
  // ...and the SECOND thing a worker does for a request: the post-response
  // phase - the deferred notifications, and releasing the arena they are built
  // in - once the response is on the wire.
  //
  // A phase and not a second queue, because it is the same work item at a later
  // moment. It is off the I/O thread for the same reason the dispatch is, and
  // one reason more: a notification's @context can be one the broker HOSTS
  // ITSELF, so this phase can issue a request the broker has to answer. On a
  // single-threaded event loop, running it there is a deadlock that resolves
  // itself as a timeout - a notification sent ten seconds late with an
  // uncompacted body, and nothing in the log saying why.
  //
  bool                    asyncFinishing;
  struct CorRestState*     asyncNext;
} CorRestState;



// -----------------------------------------------------------------------------
//
// corRest - per-request state, reached through a thread-local pointer
//
// Historically `corRest` was a plain __thread object. It is now a __thread
// POINTER (corRestP) behind the `corRest` macro, so the state can later be
// relocated off the thread (into the backend's per-connection slot) without touching
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
