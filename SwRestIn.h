//
// FILE            SwRestIn.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_IN_H_
#define SWREST_SW_REST_IN_H_

#include <stdbool.h>

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
// SwMimeType - an NGSI-LD media type. The single enum for BOTH the request
// Content-Type (classified on reception by swMimeTypeParse) and the Accept
// header (negotiated by swAcceptParse) — so the code compares an enum instead
// of strcasecmp-ing header strings. Numeric values are fixed: Json = 0 (the
// zero-init / absent-Accept default, § 6.3.4), None = -1 ("not acceptable" /
// unsupported). MergePatchJson is Content-Type only.
//
typedef enum SwMimeType
{
  SwMimeNone           = -1,   // absent / not acceptable / unsupported
  SwMimeJson           =  0,   // application/json (default)
  SwMimeLdJson,                 // application/ld+json
  SwMimeGeoJson,                // application/geo+json
  SwMimeMergePatchJson          // application/merge-patch+json
} SwMimeType;

extern SwMimeType  swMimeTypeParse(const char* contentType);  // a Content-Type value
extern SwMimeType  swAcceptParse(const char* acceptHeader);   // a q-weighted Accept header
extern const char* swMimeString(SwMimeType mime);             // enum -> canonical media-type string



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
  uint64_t        uriParamMask;   // OR of all received URI params' registry bits ("was X given?")

  // HTTP headers (dynamic array, inline for small counts)
  SwRestKeyValue  httpHeaders[SW_REST_INITIAL_KV_SLOTS];
  SwRestKeyValue* httpHeaderV;
  int             httpHeaderCount;
  int             httpHeaderSize;

  // Pretty-print indentation (0 = compact, from ?pretty=N)
  int         prettySpaces;

  // § 6.3.4 — set when a POST/PATCH/PUT arrives without Content-Length;
  // the dispatcher emits a body-less 411 in that case.
  bool        contentLengthMissing;

  // Payload
  char*       payload;
  int         payloadSize;
  KjNode*     requestTree;                        // parsed JSON payload body

  // Content-Type header value (convenience pointer into httpHeaders), and its
  // media type classified on reception
  char*       contentType;
  SwMimeType  contentMime;
  char*       accept;
} SwRestIn;

#endif  // SWREST_SW_REST_IN_H_
