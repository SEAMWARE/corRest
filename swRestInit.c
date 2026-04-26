//
// FILE            swRestInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                     // calloc, realloc, free, atoi
#include <string.h>                     // strncpy, strstr, strcmp, memcpy, strcasecmp, strlen
#include <stdio.h>                      // fprintf, snprintf
#include <time.h>                       // clock_gettime, CLOCK_MONOTONIC

#include <microhttpd.h>

#include "kalloc/kaAlloc.h"             // kaAlloc
#include "kalloc/kaStrdup.h"            // kaStrdup
#include "kjson/kjParse.h"              // kjParse
#include "kjson/kjRender.h"             // kjFastRender
#include "kjson/kjRenderSize.h"         // kjFastRenderSize
#include "kjson/kjBuilder.h"            // kjObject, kjString, kjChildAdd

#include "swRest/SwRestVerb.h"          // SwVerbs, swRestVerbToString
#include "swRest/SwRestService.h"       // SwRestService, SwRestServiceVector
#include "swRest/SwRestState.h"         // SwRestState, swRest
#include "swRest/swRestStateInit.h"     // swRestStateInit, swRestStateRelease
#include "swRest/swRestServiceLookup.h" // swRestServiceLookup
#include "swRest/swRestHooks.h"         // SwRestHook, etc.
#include "swRest/swRestProblem.h"       // SW_REST_ERROR_*, swRestProblem
#include "swRest/swRestParamRegistry.h" // swRestParamLookup
#include "swRest/SwRestStats.h"         // SwRestStats, swRestStats
#include "swRest/swRestInit.h"          // Own interface



// -----------------------------------------------------------------------------
//
// Globals
//
SwRestServiceVector   swRestServiceV[SwVerbs];
struct MHD_Daemon*    swRestDaemon = NULL;
static SwRestMetrics* metricsP = NULL;



// -----------------------------------------------------------------------------
//
// swRestMetricsSet -
//
void swRestMetricsSet(SwRestMetrics* metrics)
{
  metricsP = metrics;
}



// -----------------------------------------------------------------------------
//
// Hook globals (defined in swRestHooks.c)
//
extern SwRestHook            swRestRequestStartHook;
extern SwRestHook            swRestPayloadParseHook;
extern SwRestHook            swRestPayloadRenderHook;
extern SwRestParamHook       swRestParamHookF;
extern SwRestPreServiceHook  swRestPreServiceHookF;
extern SwRestHook            swRestPostResponseHook;



// -----------------------------------------------------------------------------
//
// servicePrepare - expand a simplified service descriptor into a full one
//
static void servicePrepare(SwRestService* serviceP, SwRestServiceSimplified* simpleP)
{
  serviceP->url              = (char*) simpleP->url;
  serviceP->serviceRoutine   = simpleP->serviceRoutine;
  serviceP->supportedParams  = simpleP->supportedParams;
  serviceP->ldOp             = simpleP->ldOp;
  serviceP->wildcards        = 0;
  serviceP->greedy           = false;

  serviceP->charsBeforeFirstWildcard      = 0;
  serviceP->charsBeforeFirstWildcardSum   = 0;
  serviceP->charsBeforeSecondWildcard     = 0;
  serviceP->charsBeforeSecondWildcardSum  = 0;
  serviceP->matchForSecondWildcardLen     = 0;
  serviceP->matchForSecondWildcard[0]     = 0;

  char*  wildCardStart = NULL;
  char*  wildCardEnd   = NULL;
  int    ix            = 0;

  while (serviceP->url[ix] != 0)
  {
    char c = serviceP->url[ix];

    if (c == '*')
    {
      // Check for ** (greedy wildcard) - skip the second '*'
      if (serviceP->url[ix + 1] == '*')
      {
        serviceP->greedy = true;
        ++ix;  // skip second '*'
      }

      if (serviceP->wildcards == 0)
        wildCardStart = &serviceP->url[ix + 1];
      else if (serviceP->wildcards == 1)
        wildCardEnd = &serviceP->url[ix];

      serviceP->wildcards += 1;
      ++ix;
      continue;
    }

    if (serviceP->wildcards == 0)
    {
      ++serviceP->charsBeforeFirstWildcard;
      serviceP->charsBeforeFirstWildcardSum += c;
    }
    else if (serviceP->wildcards == 1)
    {
      ++serviceP->charsBeforeSecondWildcard;
      serviceP->charsBeforeSecondWildcardSum += c;
    }

    ++ix;
  }

  if (serviceP->wildcards != 0)
  {
    if (wildCardEnd == NULL)
      wildCardEnd = &serviceP->url[ix];

    serviceP->matchForSecondWildcardLen = wildCardEnd - wildCardStart;

    if (serviceP->matchForSecondWildcardLen > 0 &&
        serviceP->matchForSecondWildcardLen < (int) sizeof(serviceP->matchForSecondWildcard))
    {
      strncpy(serviceP->matchForSecondWildcard, wildCardStart, serviceP->matchForSecondWildcardLen);
      serviceP->matchForSecondWildcard[serviceP->matchForSecondWildcardLen] = 0;
    }
  }
}



// -----------------------------------------------------------------------------
//
// hexVal - return value of a hex digit, or -1 if not hex
//
static int hexVal(char c)
{
  if (c >= '0' && c <= '9')  return c - '0';
  if (c >= 'A' && c <= 'F')  return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')  return c - 'a' + 10;
  return -1;
}



// -----------------------------------------------------------------------------
//
// percentDecode - decode %XX sequences in-place
//
static void percentDecode(char* s)
{
  char* out = s;

  for (char* p = s; *p != '\0'; )
  {
    if (p[0] == '%' && p[1] != '\0' && p[2] != '\0')
    {
      int hi = hexVal(p[1]);
      int lo = hexVal(p[2]);

      if (hi >= 0 && lo >= 0)
      {
        *out++ = (char)((hi << 4) | lo);
        p += 3;
        continue;
      }
    }

    *out++ = *p++;
  }

  *out = '\0';
}



// -----------------------------------------------------------------------------
//
// addUriParam - add URI parameter to dynamic array, growing if needed
//
static void addUriParam(char* name, char* value)
{
  if (swRest.in.uriParamCount >= swRest.in.uriParamSize)
  {
    int newSize = swRest.in.uriParamSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV = (SwRestKeyValue*) kaAlloc(&swRest.kalloc, newSize * sizeof(SwRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, swRest.in.uriParamV, swRest.in.uriParamCount * sizeof(SwRestKeyValue));

    swRest.in.uriParamV    = newV;
    swRest.in.uriParamSize = newSize;
  }

  swRest.in.uriParamV[swRest.in.uriParamCount].key   = name;
  swRest.in.uriParamV[swRest.in.uriParamCount].value  = value;
  swRest.in.uriParamCount++;
}



// -----------------------------------------------------------------------------
//
// parseUriParams - parse query string into key-value pairs with percent-decoding
//
static void parseUriParams(void)
{
  if (swRest.in.urlParams == NULL)
    return;

  char* p = swRest.in.urlParams;

  while (*p != '\0')
  {
    char* name = p;

    while (*p != '\0' && *p != '=' && *p != '&')
      p++;

    char* value = "";

    if (*p == '=')
    {
      *p = '\0';
      p++;
      value = p;

      while (*p != '\0' && *p != '&')
        p++;

      if (*p == '&')
      {
        *p = '\0';
        p++;
      }
    }
    else if (*p == '&')
    {
      *p = '\0';
      p++;
    }

    // Percent-decode name and value
    percentDecode(name);
    if (value[0] != '\0')
      percentDecode(value);

    addUriParam(name, value);
  }
}



// -----------------------------------------------------------------------------
//
// mhdHeaderIterator - MHD callback to collect request headers
//
static enum MHD_Result mhdHeaderIterator
(
  void*              cls,
  enum MHD_ValueKind kind,
  const char*        key,
  const char*        value
)
{
  if (swRest.in.httpHeaderCount >= swRest.in.httpHeaderSize)
  {
    int newSize = swRest.in.httpHeaderSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV = (SwRestKeyValue*) kaAlloc(&swRest.kalloc, newSize * sizeof(SwRestKeyValue));

    if (newV == NULL)
      return MHD_YES;

    memcpy(newV, swRest.in.httpHeaderV, swRest.in.httpHeaderCount * sizeof(SwRestKeyValue));

    swRest.in.httpHeaderV    = newV;
    swRest.in.httpHeaderSize = newSize;
  }

  swRest.in.httpHeaderV[swRest.in.httpHeaderCount].key   = (char*) key;
  swRest.in.httpHeaderV[swRest.in.httpHeaderCount].value  = (char*) value;
  swRest.in.httpHeaderCount++;

  // Extract well-known headers
  if (strcasecmp(key, "Content-Type") == 0)
    swRest.in.contentType = (char*) value;
  else if (strcasecmp(key, "Accept") == 0)
    swRest.in.accept = (char*) value;

  return MHD_YES;
}



// -----------------------------------------------------------------------------
//
// mhdUriParamIterator - MHD callback to collect URI query parameters
//
// MHD strips query params from the URL and provides them via MHD_GET_ARGUMENT_KIND.
// The values are already percent-decoded by MHD.
//
static enum MHD_Result mhdUriParamIterator
(
  void*              cls,
  enum MHD_ValueKind kind,
  const char*        key,
  const char*        value
)
{
  if (swRest.in.uriParamCount >= swRest.in.uriParamSize)
  {
    int newSize = swRest.in.uriParamSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV = (SwRestKeyValue*) kaAlloc(&swRest.kalloc, newSize * sizeof(SwRestKeyValue));

    if (newV == NULL)
      return MHD_YES;

    memcpy(newV, swRest.in.uriParamV, swRest.in.uriParamCount * sizeof(SwRestKeyValue));

    swRest.in.uriParamV    = newV;
    swRest.in.uriParamSize = newSize;
  }

  swRest.in.uriParamV[swRest.in.uriParamCount].key   = (char*) key;
  swRest.in.uriParamV[swRest.in.uriParamCount].value  = (char*) (value ? value : "");
  swRest.in.uriParamCount++;

  return MHD_YES;
}



// -----------------------------------------------------------------------------
//
// mhdConnectionHandler - MHD callback, called for each incoming request
//
// MHD calls this multiple times per request:
//   1. First call:  *con_cls == NULL  -> init swRest state
//   2. Middle calls: upload_data_size > 0 -> accumulate payload
//   3. Final call:   upload_data_size == 0 -> parse, dispatch, render, respond
//
static enum MHD_Result mhdConnectionHandler
(
  void*                  cls,
  struct MHD_Connection* connection,
  const char*            url,
  const char*            method,
  const char*            version,
  const char*            uploadData,
  size_t*                uploadDataSize,
  void**                 con_cls
)
{
  // --- First call: initialize thread-local state ---
  if (*con_cls == NULL)
  {
    swRestStateInit(connection, url, method);

    // Capture request start time — REALTIME for timestamps, MONOTONIC for duration metrics
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    swRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;

    struct timespec tsM;
    clock_gettime(CLOCK_MONOTONIC, &tsM);
    swRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

    // Collect request headers from MHD
    MHD_get_connection_values(connection, MHD_HEADER_KIND, mhdHeaderIterator, NULL);

    // Collect URI query parameters from MHD (already percent-decoded by MHD)
    MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, mhdUriParamIterator, NULL);

    // Request-start hook
    swRestRequestStartHook();

    *con_cls = (void*) 1;  // non-NULL marker
    return MHD_YES;
  }

  // --- Middle calls: accumulate payload ---
  if (*uploadDataSize > 0)
  {
    int needed = swRest.in.payloadSize + *uploadDataSize + 1;

    if (needed > swRest.payloadBufSize)
    {
      int newSize = (needed + 4096) & ~4095;
      char* newBuf = (char*) realloc(swRest.in.payload, newSize);
      if (newBuf == NULL)
        return MHD_NO;
      swRest.in.payload     = newBuf;
      swRest.payloadBufSize = newSize;
    }

    memcpy(swRest.in.payload + swRest.in.payloadSize, uploadData, *uploadDataSize);
    swRest.in.payloadSize += *uploadDataSize;
    swRest.in.payload[swRest.in.payloadSize] = 0;

    *uploadDataSize = 0;
    return MHD_YES;
  }

  // --- Final call: parse, dispatch, render, respond ---

  // Parse incoming JSON payload (if any)
  if (swRest.in.payloadSize > 0)
  {
    swRest.in.requestTree = kjParse(swRest.kjsonP, swRest.in.payload);

    if (swRest.in.requestTree != NULL)
      swRestPayloadParseHook();
  }

  // Extract ?pretty=N (remove from param list so it doesn't hit validation)
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    if (strcmp(swRest.in.uriParamV[i].key, "pretty") == 0)
    {
      swRest.in.prettySpaces = atoi(swRest.in.uriParamV[i].value);

      // Remove from param list
      swRest.in.uriParamCount--;
      if (i < swRest.in.uriParamCount)
        swRest.in.uriParamV[i] = swRest.in.uriParamV[swRest.in.uriParamCount];
      i--;
    }
  }

  // Lookup service
  swRest.serviceP = swRestServiceLookup(&swRestServiceV[swRest.in.verb]);

  if (swRest.serviceP == NULL)
  {
    swRestProblem(404, SW_REST_ERROR_NOT_FOUND, "Not Found",
                  "%s %s is not a recognized resource",
                  swRest.in.verbString ? swRest.in.verbString : "?",
                  swRest.in.urlPath    ? swRest.in.urlPath    : "?");
    goto respond;
  }

  // Validate URL parameters against the service's supported set.
  // Sentinel: supportedParams == ~0ULL ("all bits set") means the
  // service accepts any URL param without validation — used by mocks
  // (ftClient) and other passthrough tools that don't pre-register
  // a vocabulary of known params.
  if (swRest.in.uriParamCount > 0 && swRest.serviceP->supportedParams != ~(uint64_t)0)
  {
    for (int i = 0; i < swRest.in.uriParamCount; i++)
    {
      uint64_t bit = swRestParamLookup(swRest.in.uriParamV[i].key);

      if (bit == 0 || (bit & swRest.serviceP->supportedParams) == 0)
      {
        swRestProblem(400, SW_REST_ERROR_BAD_REQUEST, "Bad Request Data",
                      "Unknown/unsupported URL parameter: %s", swRest.in.uriParamV[i].key);
        goto respond;
      }

      if (swRestParamHookF != NULL)
        swRestParamHookF(swRest.in.uriParamV[i].key, swRest.in.uriParamV[i].value);
    }
  }

  // Pre-service hook (tenant resolution, etc.)
  if (!swRestPreServiceHookF())
    goto respond;

  // Dispatch to service routine
  swRest.serviceP->serviceRoutine();

  // Call render hook (compaction, etc.) if no error
  if (swRest.out.problemType == NULL)
    swRestPayloadRenderHook();

  // If service set a problem type but forgot status code, default to 500
  if (swRest.out.httpStatusCode == 200 && swRest.out.problemType != NULL)
    swRest.out.httpStatusCode = 500;

 respond:
  ;

  // Build problem details response tree if needed
  if (swRest.out.problemType != NULL && swRest.out.responseTree == NULL)
  {
    swRest.out.responseTree = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "type",   swRest.out.problemType));
    kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "title",  swRest.out.problemTitle));
    kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "detail", swRest.out.problemDetail));
  }

  // Render response tree to JSON
  char*  responseBody     = NULL;
  int    responseBodySize = 0;

  if (swRest.out.responseTree != NULL)
  {
    if (swRest.in.prettySpaces > 0)
    {
      swRest.kjsonP->spacesPerIndent = swRest.in.prettySpaces;
      responseBodySize = kjRenderSize(swRest.kjsonP, swRest.out.responseTree) + 1;
      responseBody     = kaAlloc(&swRest.kalloc, responseBodySize);
      if (responseBody == NULL)
      {
        fprintf(stderr, "swRest: response body alloc failed (need %d bytes) — arena chunk too small?\n", responseBodySize);
        swRest.out.httpStatusCode = 500;
        responseBody     = (char*) "";
        responseBodySize = 0;
      }
      else
      {
        kjRender(swRest.kjsonP, swRest.out.responseTree, responseBody);
      }
    }
    else
    {
      responseBodySize = kjFastRenderSize(swRest.out.responseTree) + 1;
      responseBody     = kaAlloc(&swRest.kalloc, responseBodySize);
      if (responseBody == NULL)
      {
        fprintf(stderr, "swRest: response body alloc failed (need %d bytes) — arena chunk too small?\n", responseBodySize);
        swRest.out.httpStatusCode = 500;
        responseBody     = (char*) "";
        responseBodySize = 0;
      }
      else
      {
        kjFastRender(swRest.out.responseTree, responseBody);
      }
    }
    if (responseBody != NULL && responseBody[0] != 0)
      responseBodySize = strlen(responseBody);
  }
  else if (swRest.out.payload != NULL)
  {
    responseBody     = swRest.out.payload;
    responseBodySize = swRest.out.payloadSize;
  }
  else
  {
    responseBody     = (char*) "";
    responseBodySize = 0;
  }

  // Send HTTP response
  // For HEAD: pass the full body — MHD will set Content-Length correctly
  // but suppress the body in the actual response.
  struct MHD_Response* response;

  response = MHD_create_response_from_buffer(
    responseBodySize,
    (void*) responseBody,
    MHD_RESPMEM_MUST_COPY
  );

  if (swRest.out.contentType != NULL)
    MHD_add_response_header(response, "Content-Type", swRest.out.contentType);

  // Add custom response headers
  for (int i = 0; i < swRest.out.headerCount; i++)
    MHD_add_response_header(response, swRest.out.headerV[i].key, swRest.out.headerV[i].value);

  // CORS headers (if configured)
  extern SwRestCorsConfig swRestCors;
  if (swRestCors.allowOrigin != NULL)
  {
    MHD_add_response_header(response, "Access-Control-Allow-Origin", swRestCors.allowOrigin);

    if (swRest.in.verb == SwVerbOptions)
    {
      // Preflight: echo Allow as Access-Control-Allow-Methods
      for (int i = 0; i < swRest.out.headerCount; i++)
      {
        if (strcmp(swRest.out.headerV[i].key, "Allow") == 0)
        {
          MHD_add_response_header(response, "Access-Control-Allow-Methods", swRest.out.headerV[i].value);
          break;
        }
      }

      const char* ah = swRestCors.allowHeaders ? swRestCors.allowHeaders : "Content-Type, Accept, Link";
      MHD_add_response_header(response, "Access-Control-Allow-Headers", ah);

      if (swRestCors.maxAge > 0)
      {
        char maxAgeBuf[16];
        snprintf(maxAgeBuf, sizeof(maxAgeBuf), "%d", swRestCors.maxAge);
        MHD_add_response_header(response, "Access-Control-Max-Age", maxAgeBuf);
      }
    }

    if (swRestCors.exposeHeaders != NULL)
      MHD_add_response_header(response, "Access-Control-Expose-Headers", swRestCors.exposeHeaders);
  }

  enum MHD_Result ret = MHD_queue_response(connection, swRest.out.httpStatusCode, response);
  MHD_destroy_response(response);

  // Update Prometheus metrics (if registered by application)
  if (metricsP != NULL)
  {
    if (metricsP->requests != NULL)
      kpromCounterInc(metricsP->requests);

    if (metricsP->responseBytes != NULL)
      kpromCounterAdd(metricsP->responseBytes, responseBodySize);

    if (swRest.out.httpStatusCode >= 400 && metricsP->requestErrors != NULL)
      kpromCounterInc(metricsP->requestErrors);

    if (metricsP->requestDuration != NULL)
    {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t nowNs  = (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
      double   durSec = (double)(nowNs - swRest.requestStartTimeMono) / 1000000000.0;
      kpromHistogramObserve(metricsP->requestDuration, durSec);
    }
  }

  return ret;
}



// -----------------------------------------------------------------------------
//
// mhdRequestCompleted - MHD callback when a request is fully handled
//
static void mhdRequestCompleted
(
  void*                          cls,
  struct MHD_Connection*         connection,
  void**                         con_cls,
  enum MHD_RequestTerminationCode toe
)
{
  if (*con_cls != NULL)
  {
    // Run post-response hook BEFORE releasing the per-request arena so the
    // hook can still touch arena-allocated data (e.g. deferred notification
    // dispatch reading the entity tree built by the service routine).
    swRestPostResponseHook();

    // Free payload buffer (malloc'd during accumulation, not in kalloc)
    free(swRest.in.payload);
    swRest.in.payload = NULL;

    swRestStateRelease();
    *con_cls = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// optionsHandler - auto-generated OPTIONS handler
//
// Builds an Allow header listing all verbs registered for the same URL pattern.
// Uses the matched service's URL pattern (not the actual request path),
// so wildcard URLs work correctly.
//
static bool optionsHandler(void)
{
  const char* matchedUrl = swRest.serviceP ? swRest.serviceP->url : swRest.in.urlPath;

  char allow[128];
  int  pos = 0;

  for (int verb = 0; verb < SwVerbs; verb++)
  {
    for (int ix = 0; ix < swRestServiceV[verb].services; ix++)
    {
      if (strcmp(swRestServiceV[verb].serviceV[ix].url, matchedUrl) == 0)
      {
        if (pos > 0)
        {
          allow[pos++] = ',';
          allow[pos++] = ' ';
        }

        const char* vs = swRestVerbToString((SwRestVerb) verb);
        int         vl = strlen(vs);

        memcpy(&allow[pos], vs, vl);
        pos += vl;
        break;
      }
    }
  }

  allow[pos] = 0;

  swRest.out.httpStatusCode = 204;
  swRest.out.headerV[swRest.out.headerCount].key   = "Allow";
  swRest.out.headerV[swRest.out.headerCount].value  = kaStrdup(&swRest.kalloc, allow);
  swRest.out.headerCount++;

  return true;
}



// -----------------------------------------------------------------------------
//
// swRestInit -
//
int swRestInit(SwRestServiceSimplified serviceV[], int services, unsigned short port, int poolSize)
{
  // --- Count services per verb ---
  int counts[SwVerbs];
  memset(counts, 0, sizeof(counts));

  for (int ix = 0; ix < services; ix++)
  {
    if (serviceV[ix].verb < SwVerbs)
      counts[serviceV[ix].verb]++;

    // GET services also get HEAD for free
    if (serviceV[ix].verb == SwVerbGet)
      counts[SwVerbHead]++;
  }

  // OPTIONS gets one entry per unique URL path
  // (over-allocate: at most 'services' unique paths)
  counts[SwVerbOptions] += services;

  // --- Allocate per-verb arrays ---
  for (int verb = 0; verb < SwVerbs; verb++)
  {
    swRestServiceV[verb].services = 0;
    swRestServiceV[verb].serviceV = NULL;

    if (counts[verb] == 0)
      continue;

    swRestServiceV[verb].serviceV = (SwRestService*) calloc(counts[verb], sizeof(SwRestService));
    if (swRestServiceV[verb].serviceV == NULL)
      return -1;
  }

  // --- Distribute services into per-verb arrays ---
  for (int ix = 0; ix < services; ix++)
  {
    SwRestVerb verb = serviceV[ix].verb;
    if (verb >= SwVerbs)
      continue;

    SwRestServiceVector* sv = &swRestServiceV[verb];
    servicePrepare(&sv->serviceV[sv->services], &serviceV[ix]);
    sv->services++;

    // Auto-add HEAD for every GET (same handler; body suppressed at response time)
    if (verb == SwVerbGet)
    {
      SwRestServiceVector* headSv = &swRestServiceV[SwVerbHead];
      servicePrepare(&headSv->serviceV[headSv->services], &serviceV[ix]);
      headSv->services++;
    }
  }

  // --- Auto-generate OPTIONS for each unique URL path ---
  SwRestServiceVector* optSv = &swRestServiceV[SwVerbOptions];

  for (int ix = 0; ix < services; ix++)
  {
    // Check if this URL is already in the OPTIONS vector
    bool found = false;
    for (int ox = 0; ox < optSv->services; ox++)
    {
      if (strcmp(optSv->serviceV[ox].url, serviceV[ix].url) == 0)
      {
        found = true;
        break;
      }
    }

    if (!found)
    {
      SwRestServiceSimplified optService;

      optService.verb            = SwVerbOptions;
      optService.url             = serviceV[ix].url;
      optService.serviceRoutine  = optionsHandler;
      optService.supportedParams = 0;

      servicePrepare(&optSv->serviceV[optSv->services], &optService);
      optSv->services++;
    }
  }

  // Start MHD daemon with thread pool
  swRestDaemon = MHD_start_daemon(
    MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL,
    port,
    NULL,
    NULL,
    mhdConnectionHandler,
    NULL,
    MHD_OPTION_NOTIFY_COMPLETED,
    mhdRequestCompleted,
    NULL,
    MHD_OPTION_THREAD_POOL_SIZE,
    (unsigned int) poolSize,
    MHD_OPTION_CONNECTION_TIMEOUT,
    (unsigned int) 30,
    MHD_OPTION_END
  );

  if (swRestDaemon == NULL)
  {
    fprintf(stderr, "swRestInit: MHD_start_daemon failed on port %d\n", port);
    return -1;
  }

  return 0;
}
