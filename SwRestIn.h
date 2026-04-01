//
// FILE            SwRestIn.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_IN_H_
#define SWREST_SW_REST_IN_H_

#include "kjson/KjNode.h"
#include "swRest/SwRestVerb.h"
#include "swRest/SwRestKeyValue.h"



// -----------------------------------------------------------------------------
//
// Constants
//
#define SW_REST_MAX_WILDCARDS       3
#define SW_REST_INITIAL_KV_SLOTS   10
#define SW_REST_KV_GROW_SIZE        5



// -----------------------------------------------------------------------------
//
// SwRestIn - incoming request data
//
typedef struct SwRestIn
{
  SwRestVerb  verb;
  char*       verbString;
  char*       urlPath;
  int         urlPathLen;
  char*       urlParams;                          // raw query string
  char*       wildcard[SW_REST_MAX_WILDCARDS];    // extracted wildcard values from URL
  int         wildcards;

  // URI query parameters (dynamic array, inline for small counts)
  SwRestKeyValue  uriParams[SW_REST_INITIAL_KV_SLOTS];
  SwRestKeyValue* uriParamV;
  int             uriParamCount;
  int             uriParamSize;

  // HTTP headers (dynamic array, inline for small counts)
  SwRestKeyValue  httpHeaders[SW_REST_INITIAL_KV_SLOTS];
  SwRestKeyValue* httpHeaderV;
  int             httpHeaderCount;
  int             httpHeaderSize;

  // Pretty-print indentation (0 = compact, from ?pretty=N)
  int         prettySpaces;

  // Payload
  char*       payload;
  int         payloadSize;
  KjNode*     requestTree;                        // parsed JSON payload body

  // Content-Type header value (convenience pointer into httpHeaders)
  char*       contentType;
  char*       accept;
} SwRestIn;

#endif  // SWREST_SW_REST_IN_H_
