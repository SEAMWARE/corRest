//
// FILE            swRestStateInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                         // free, atoi
#include <string.h>                         // memset, strdup, strchr, strlen

#include "kalloc/kaBufferInit.h"            // kaBufferInit
#include "kalloc/kaBufferReset.h"           // kaBufferReset
#include "kjson/kjBufferCreate.h"           // kjBufferCreate

#include "swRest/SwRestState.h"             // SwRestState, swRest
#include "swRest/SwRestVerb.h"              // swRestVerbFromString
#include "swRest/swRestStateInit.h"         // Own interface



// -----------------------------------------------------------------------------
//
// swRest backing storage — the per-thread fallback object and the current-
// state pointer the `swRest` macro dereferences (see SwRestState.h). swRestP
// stays NULL until first access on a thread, then auto-binds to swRestFallback.
//
__thread SwRestState  swRestFallback;
__thread SwRestState* swRestP = NULL;



// -----------------------------------------------------------------------------
//
// External: default pretty-print setting from swRestHooks.c
//
extern int swRestDefaultPrettySpaces;



// -----------------------------------------------------------------------------
//
// swRestStateInit -
//
void swRestStateInit(struct MHD_Connection* connection, const char* url, const char* method)
{
  memset(&swRest, 0, sizeof(SwRestState));

  swRest.mhdConnection = connection;

  // Initialize kalloc pool (inline buffer first, then malloc-based overflow).
  // 256KB grow chunks: keeps single-shot renderings of ~1000-entity responses
  // inside one block. kaAlloc silently returns NULL for any single request
  // >= chunk size, so this also caps the max single allocation.
  kaBufferInit(&swRest.kalloc, swRest.kallocBuffer, sizeof(swRest.kallocBuffer), 256 * 1024, NULL, "swRest");

  // Initialize kjson with kalloc
  swRest.kjsonP = kjBufferCreate(&swRest.kjson, &swRest.kalloc);

  // Verb
  swRest.in.verb       = swRestVerbFromString(method);
  swRest.in.verbString = (char*) method;

  // URL path (allocated via strdup - freed in swRestStateRelease)
  swRest.in.urlPath = strdup(url);

  // Separate query string from URL path
  char* qMark = strchr(swRest.in.urlPath, '?');
  if (qMark != NULL)
  {
    *qMark = 0;
    swRest.in.urlParams = qMark + 1;
  }

  swRest.in.urlPathLen = strlen(swRest.in.urlPath);

  // Strip a single trailing '/' from the path (except the root "/" itself).
  // Routes are registered without a trailing slash; clients that send one
  // (e.g. ETSI test suite: POST /ngsi-ld/v1/entities/) would otherwise
  // 404 against an exact-match service lookup.
  if (swRest.in.urlPathLen > 1 && swRest.in.urlPath[swRest.in.urlPathLen - 1] == '/')
  {
    swRest.in.urlPath[swRest.in.urlPathLen - 1] = 0;
    swRest.in.urlPathLen--;
  }

  // Initialize URI param dynamic array (starts with inline slots)
  swRest.in.uriParamV     = swRest.in.uriParams;
  swRest.in.uriParamCount = 0;
  swRest.in.uriParamSize  = SW_REST_INITIAL_KV_SLOTS;
  swRest.in.uriParamMask  = 0;

  // Initialize HTTP header dynamic array (starts with inline slots)
  swRest.in.httpHeaderV     = swRest.in.httpHeaders;
  swRest.in.httpHeaderCount = 0;
  swRest.in.httpHeaderSize  = SW_REST_INITIAL_KV_SLOTS;

  // Pretty-print default
  swRest.in.prettySpaces = swRestDefaultPrettySpaces;

  // Defaults for response
  swRest.out.httpStatusCode = 200;
  swRest.out.contentType    = (char*) swMimeString(SwMimeJson);
  swRest.out.headerV        = swRest.out.headers;
  swRest.out.headerCount    = 0;
  swRest.out.headerSize     = SW_REST_INITIAL_KV_SLOTS;

  // Payload accumulation
  swRest.in.payload      = NULL;
  swRest.in.payloadSize  = 0;
  swRest.payloadBufSize  = 0;
}



// -----------------------------------------------------------------------------
//
// swRestStateRelease - release per-request resources
//
void swRestStateRelease(void)
{
  // Free the URL path (strdup'd in swRestStateInit)
  if (swRest.in.urlPath != NULL)
  {
    free(swRest.in.urlPath);
    swRest.in.urlPath = NULL;
  }

  // Bulk-free all kalloc allocations
  kaBufferReset(&swRest.kalloc, KFALSE);
}
