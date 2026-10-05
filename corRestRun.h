#ifndef CORREST_CORRESTRUN_H_
#define CORREST_CORRESTRUN_H_

//
// FILE            corRestRun.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corRestRunJson - one request, from a transport that is not HTTP (a WebSocket message), run on the
// calling thread from start to finish: the same service routines as an HTTP request, JSON in and out.
//
#include "corRest/CorRestKeyValue.h"                 // CorRestKeyValue



// -----------------------------------------------------------------------------
//
// CorRestRunRespond - the response: its status, headers and body (valid only during the call)
//
// Called before the request's post-response phase - the notifications it causes go out after it, as
// for HTTP, so a client sees the 201 of a subscription before the first notification of it.
//
typedef void (*CorRestRunRespond)(int status, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen, void* ctx);



// -----------------------------------------------------------------------------
//
// corRestRunJson - verb, path (with its query, percent-encoded as on the wire), headers and body in;
// the response to 'respond'. Blocks the calling thread for as long as the request takes - never call it
// on an event loop of corRest's.
//
extern void corRestRunJson
(
  const char*        verb,
  const char*        path,
  CorRestKeyValue*   headerV,
  int                headers,
  const char*        body,
  int                bodyLen,
  CorRestRunRespond  respond,
  void*              ctx
);

#endif  // CORREST_CORRESTRUN_H_
