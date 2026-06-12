//
// FILE            SwRestOut.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_OUT_H_
#define SWREST_SW_REST_OUT_H_

#include "kjson/KjNode.h"
#include "swRest/SwRestKeyValue.h"
#include "swRest/SwRestIn.h"       // SW_REST_INITIAL_KV_SLOTS



// -----------------------------------------------------------------------------
//
// SwRestOut - outgoing response data
//
typedef struct SwRestOut
{
  int         httpStatusCode;
  KjNode*     responseTree;       // built by service routine, rendered before sending
  char*       contentType;        // e.g. "application/json"

  // Response headers (dynamic array, inline for small counts)
  SwRestKeyValue  headers[SW_REST_INITIAL_KV_SLOTS];
  SwRestKeyValue* headerV;
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
} SwRestOut;

#endif  // SWREST_SW_REST_OUT_H_
