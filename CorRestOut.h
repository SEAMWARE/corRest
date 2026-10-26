//
// FILE            CorRestOut.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_OUT_H_
#define CORREST_OUT_H_

#include "corTree/CorNode.h"
#include "corRest/CorRestKeyValue.h"
#include "corRest/CorRestIn.h"       // COR_REST_INITIAL_KV_SLOTS

struct CorRestStream;                // corRestStream.h



// -----------------------------------------------------------------------------
//
// CorRestOut - outgoing response data
//
typedef struct CorRestOut
{
  int         httpStatusCode;
  CorNode*    responseTree;       // built by service routine, rendered before sending
  char*       contentType;        // e.g. "application/json"

  // Response headers (dynamic array, inline for small counts)
  CorRestKeyValue  headers[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue* headerV;
  int             headerCount;
  int             headerSize;

  // Response body (when set directly, not via responseTree)
  char*       payload;
  int         payloadSize;
  bool        noRender;           // the transport carries responseTree as it is (cor://): no JSON render

  // A body written over time (corRestStream.h) - set by corRestStreamResponse; in every build, so that
  // the struct is the same with COR_WITH_STREAM=0
  void      (*streamOpenF)(struct CorRestStream* streamP, void* ctx);
  void*       streamCtx;

  // Problem details (RFC 9457) - set by service routines on error
  const char*  problemType;         // Error type URI (NULL = no error)
  const char*  problemTitle;        // Short title
  char         problemDetail[512];  // Detail message (formatted)
  CorNode*     problemExtras;       // Optional extension members (RFC 9457 §3.2) spliced into the body; NULL = none

  // A late problem (corRestProblemLate) - raised by a layer under the service routine (a store) about
  // what the routine has already done: it replaces the routine's answer, whatever that is. 0: none
  int          lateStatus;
  const char*  lateType;
  const char*  lateTitle;
  char         lateDetail[256];
} CorRestOut;

#endif  // CORREST_OUT_H_
