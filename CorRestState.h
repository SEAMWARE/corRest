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

#include "corAlloc/CorAlloc.h"
#include "corJson/CorJson.h"

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
  CorAlloc                kalloc;
  char                    kallocBuffer[8 * 1024];   // initial inline buffer

  //
  // kallocP - the allocator every tree for this request is built in: &kalloc
  // once the state is initialised, NULL before that. The corTree builders take
  // an allocator and read NULL as "use malloc", which is exactly what they did
  // when they took the parser handle and it was still NULL - so this is set
  // wherever corJsonP is, and a thread that never initialised its state keeps
  // both NULL, as before.
  //
  CorAlloc*               kallocP;

  // JSON parser
  CorJson                 corJson;
  CorJson*                corJsonP;

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

  //
  // resumeF / finishF - a transport of its own (cor://) in place of the HTTP backend's
  //
  // A worker hands a processed request back with corRestBackendResume and runs the post-response
  // phase with corRestBackendFinish - the HTTP backend's. A request that came in over another
  // transport sets these, and the worker calls them instead. NULL: the backend's, as always.
  //
  void                   (*resumeF)(struct CorRestState* stateP);
  void                   (*finishF)(struct CorRestState* stateP);

  //
  // shard - which work queue this request belongs to
  //
  // Set by the BACKEND, because only the backend knows how many event loops
  // there are and which one read this request. The builtin server derives it
  // from the connection's loop; libmicrohttpd sets 0 and has exactly one shard,
  // so nothing about that backend changes.
  //
  // It exists so a request stays on ONE loop's threads end to end - read by
  // loop i, queued to shard i, run by one of shard i's workers, resumed to
  // loop i, written by loop i - and never crosses a mutex another loop is
  // using.
  //
  int                      shard;

  //
  // selfForwardDepth - how deep in in-process forwards this request is (corRestProcessInProcess): the
  // request's, not the thread's - on a coroutine, another request of the same thread may be in one too
  //
  int                      selfForwardDepth;
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
