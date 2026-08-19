//
// FILE            CorRestOut.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef CORREST_OUT_H_
#define CORREST_OUT_H_

#include "kjson/KjNode.h"
#include "corRest/CorRestKeyValue.h"
#include "corRest/CorRestIn.h"       // COR_REST_INITIAL_KV_SLOTS



// -----------------------------------------------------------------------------
//
// CorRestOut - outgoing response data
//
typedef struct CorRestOut
{
  int         httpStatusCode;
  KjNode*     responseTree;       // built by service routine, rendered before sending
  char*       contentType;        // e.g. "application/json"

  // Response headers (dynamic array, inline for small counts)
  CorRestKeyValue  headers[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue* headerV;
  int             headerCount;
  int             headerSize;

  // Response body (when set directly, not via responseTree)
  char*       payload;
  int         payloadSize;

  // Problem details (RFC 9457) - set by service routines on error
  const char*  problemType;         // Error type URI (NULL = no error)
  const char*  problemTitle;        // Short title
  char         problemDetail[512];  // Detail message (formatted)
  KjNode*      problemExtras;       // Optional extension members (RFC 9457 §3.2) spliced into the body; NULL = none
} CorRestOut;

#endif  // CORREST_OUT_H_
