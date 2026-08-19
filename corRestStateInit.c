//
// FILE            corRestStateInit.c
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

#include "corRest/CorRestState.h"             // CorRestState, corRest
#include "corRest/CorRestVerb.h"              // corRestVerbFromString
#include "corRest/corRestStateInit.h"         // Own interface



// -----------------------------------------------------------------------------
//
// corRest backing storage — the per-thread fallback object and the current-
// state pointer the `corRest` macro dereferences (see CorRestState.h). corRestP
// stays NULL until first access on a thread, then auto-binds to corRestFallback.
//
__thread CorRestState  corRestFallback;
__thread CorRestState* corRestP = NULL;



// -----------------------------------------------------------------------------
//
// External: default pretty-print setting from corRestHooks.c
//
extern int corRestDefaultPrettySpaces;



// -----------------------------------------------------------------------------
//
// corRestStateInit -
//
void corRestStateInit(struct MHD_Connection* connection, const char* url, const char* method)
{
  memset(&corRest, 0, sizeof(CorRestState));

  corRest.mhdConnection = connection;

  // Initialize kalloc pool (inline buffer first, then malloc-based overflow).
  // 256KB grow chunks: keeps single-shot renderings of ~1000-entity responses
  // inside one block. kaAlloc silently returns NULL for any single request
  // >= chunk size, so this also caps the max single allocation.
  kaBufferInit(&corRest.kalloc, corRest.kallocBuffer, sizeof(corRest.kallocBuffer), 256 * 1024, NULL, "corRest");

  // Initialize kjson with kalloc
  corRest.kjsonP = kjBufferCreate(&corRest.kjson, &corRest.kalloc);

  // Verb
  corRest.in.verb       = corRestVerbFromString(method);
  corRest.in.verbString = (char*) method;

  // URL path (allocated via strdup - freed in corRestStateRelease)
  corRest.in.urlPath = strdup(url);

  // Separate query string from URL path
  char* qMark = strchr(corRest.in.urlPath, '?');
  if (qMark != NULL)
  {
    *qMark = 0;
    corRest.in.urlParams = qMark + 1;
  }

  corRest.in.urlPathLen = strlen(corRest.in.urlPath);

  // Strip a single trailing '/' from the path (except the root "/" itself).
  // Routes are registered without a trailing slash; clients that send one
  // (e.g. ETSI test suite: POST /ngsi-ld/v1/entities/) would otherwise
  // 404 against an exact-match service lookup.
  if (corRest.in.urlPathLen > 1 && corRest.in.urlPath[corRest.in.urlPathLen - 1] == '/')
  {
    corRest.in.urlPath[corRest.in.urlPathLen - 1] = 0;
    corRest.in.urlPathLen--;
  }

  // Initialize URI param dynamic array (starts with inline slots)
  corRest.in.uriParamV     = corRest.in.uriParams;
  corRest.in.uriParamCount = 0;
  corRest.in.uriParamSize  = COR_REST_INITIAL_KV_SLOTS;
  corRest.in.uriParamMask  = 0;

  // Initialize HTTP header dynamic array (starts with inline slots)
  corRest.in.httpHeaderV     = corRest.in.httpHeaders;
  corRest.in.httpHeaderCount = 0;
  corRest.in.httpHeaderSize  = COR_REST_INITIAL_KV_SLOTS;

  // Pretty-print default
  corRest.in.prettySpaces = corRestDefaultPrettySpaces;

  // Defaults for response
  corRest.out.httpStatusCode = 200;
  corRest.out.contentType    = (char*) corMimeString(CorMimeJson);
  corRest.out.headerV        = corRest.out.headers;
  corRest.out.headerCount    = 0;
  corRest.out.headerSize     = COR_REST_INITIAL_KV_SLOTS;

  // Payload accumulation
  corRest.in.payload      = NULL;
  corRest.in.payloadSize  = 0;
  corRest.payloadBufSize  = 0;
}



// -----------------------------------------------------------------------------
//
// corRestStateRelease - release per-request resources
//
void corRestStateRelease(void)
{
  // Free the URL path (strdup'd in corRestStateInit)
  if (corRest.in.urlPath != NULL)
  {
    free(corRest.in.urlPath);
    corRest.in.urlPath = NULL;
  }

  // Bulk-free all kalloc allocations
  kaBufferReset(&corRest.kalloc, KFALSE);
}
