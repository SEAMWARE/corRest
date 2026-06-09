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
extern SwRestHook            swRestPreDispatchHook;
extern SwRestHook            swRestPayloadParseHook;
extern SwRestHook            swRestPayloadRenderHook;
extern SwRestParamHook       swRestParamHookF;
extern SwRestPreServiceHook  swRestPreServiceHookF;
extern SwRestServiceInitHook swRestServiceInitHookF;
extern SwRestHook            swRestPostResponseHook;
extern unsigned long long    swRestMaxRequestSize;



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
  serviceP->options          = 0;     // embedder fills via swRestServiceInitHookF below
  serviceP->wildcards        = 0;
  serviceP->greedy           = false;

  serviceP->charsBeforeFirstWildcard      = 0;
  serviceP->charsBeforeFirstWildcardSum   = 0;
  serviceP->charsBeforeSecondWildcard     = 0;
  serviceP->charsBeforeSecondWildcardSum  = 0;
  serviceP->matchForSecondWildcardLen     = 0;
  serviceP->matchForSecondWildcard[0]     = 0;
  serviceP->matchForThirdWildcardLen      = 0;
  serviceP->matchForThirdWildcard[0]      = 0;

  char*  wildCardStart    = NULL;  // start of literal between *#1 and *#2
  char*  wildCardEnd      = NULL;  // end of literal between *#1 and *#2
  char*  thirdWildStart   = NULL;  // start of literal between *#2 and *#3
  char*  thirdWildEnd     = NULL;  // end of literal between *#2 and *#3
  int    ix               = 0;

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
      {
        wildCardEnd    = &serviceP->url[ix];
        thirdWildStart = &serviceP->url[ix + 1];
      }
      else if (serviceP->wildcards == 2)
        thirdWildEnd = &serviceP->url[ix];

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

    if (serviceP->wildcards >= 3 && thirdWildStart != NULL)
    {
      if (thirdWildEnd == NULL)
        thirdWildEnd = &serviceP->url[ix];

      serviceP->matchForThirdWildcardLen = thirdWildEnd - thirdWildStart;

      if (serviceP->matchForThirdWildcardLen > 0 &&
          serviceP->matchForThirdWildcardLen < (int) sizeof(serviceP->matchForThirdWildcard))
      {
        strncpy(serviceP->matchForThirdWildcard, thirdWildStart, serviceP->matchForThirdWildcardLen);
        serviceP->matchForThirdWildcard[serviceP->matchForThirdWildcardLen] = 0;
      }
    }
  }

  // Let the embedding library (swNgsild) inspect the URL pattern and set
  // any per-route options it cares about. Called once per service at init.
  if (swRestServiceInitHookF != NULL)
    swRestServiceInitHookF(serviceP);
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
// addHttpHeader - append a request header to swRest.in, extracting well-knowns
//
// Shared by the MHD header iterator and the in-process self-forward path. The
// key/value pointers are borrowed (not copied) — the caller keeps them alive
// for the request's lifetime.
//
static void addHttpHeader(const char* key, const char* value)
{
  if (swRest.in.httpHeaderCount >= swRest.in.httpHeaderSize)
  {
    int newSize = swRest.in.httpHeaderSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV = (SwRestKeyValue*) kaAlloc(&swRest.kalloc, newSize * sizeof(SwRestKeyValue));

    if (newV == NULL)
      return;

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
  addHttpHeader(key, value);
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
// swRestProcessRequest - run the request-dispatch core on the bound swRest state
//
// Connection-free: consumes swRest.in (verb, url, headers, params, requestTree)
// and produces swRest.out (status, headers, problem / responseTree) plus the
// final rendered body in swRest.out.payload / payloadSize. The MHD send stays in
// the connection handler. Reusable for an in-process self-forward (a distop
// whose endpoint is this broker): bind swRestP to an inner state, populate its
// .in, call this, read its .out — no socket round-trip.
//
void swRestProcessRequest(void)
{
  // Pre-dispatch hook: reset per-request application state (e.g. swNgsild)
  // HERE, at the start of the atomic dispatch — not at first-byte. With the
  // epoll pool another connection's request may run on this thread between
  // our first and final callbacks; resetting at dispatch keeps that state's
  // reset/populate/read all inside this atomic dispatch, so no stale values
  // leak in. (The whole final call runs without yielding to the pool.)
  swRestPreDispatchHook();

  // § 6.3.4 — POST/PATCH/PUT without Content-Length: emit 411 with no
  // body and skip everything else.
  if (swRest.in.contentLengthMissing)
  {
    swRest.out.httpStatusCode = 411;
    goto respond;
  }

  // 413 already set by first-call or middle-call: short-circuit to render.
  if (swRest.out.problemType != NULL && swRest.out.httpStatusCode == 413)
    goto respond;

  // Lookup service early — the parse hook needs to know the route so it can
  // tailor validation (e.g. batch ops surface per-element @context errors as
  // 207 entries instead of a global 400). serviceP may stay NULL here; the
  // 404/405 handling below still runs in that case.
  swRest.serviceP = swRestServiceLookup(&swRestServiceV[swRest.in.verb]);

  // § 6.3.4 — Unsupported Media Type. POST / PATCH / PUT on a body-bearing
  // NGSI-LD endpoint with a Content-Type that isn't application/json or
  // application/ld+json (with or without a charset parameter): 415, body
  // empty (spec wording: "shall result in just a 415 HTTP status code
  // (without any payload body)"). Run before the JSON parse so we don't
  // 400 InvalidRequest on a body that was never meant to be JSON.
  if (swRest.in.payloadSize > 0 && swRest.in.contentType != NULL &&
      (swRest.in.verb == SwVerbPost  ||
       swRest.in.verb == SwVerbPut   ||
       swRest.in.verb == SwVerbPatch))
  {
    const char* ct = swRest.in.contentType;
    bool ok = (strncasecmp(ct, "application/json",    16) == 0 &&
               (ct[16] == 0 || ct[16] == ';' || ct[16] == ' ')) ||
              (strncasecmp(ct, "application/ld+json", 19) == 0 &&
               (ct[19] == 0 || ct[19] == ';' || ct[19] == ' '));

    // PATCH also accepts application/merge-patch+json (RFC 7396 / NGSI-LD
    // § 5.6.18 Merge / § 5.6.5 Partial Update). Other media types remain 415.
    if (!ok && swRest.in.verb == SwVerbPatch &&
        strncasecmp(ct, "application/merge-patch+json", 28) == 0 &&
        (ct[28] == 0 || ct[28] == ';' || ct[28] == ' '))
      ok = true;

    if (!ok)
    {
      swRest.out.httpStatusCode = 415;
      goto respond;
    }
  }

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

  if (swRest.serviceP == NULL)
  {
    // Empty path segment (`//` somewhere in the path) almost always
    // means the client URL-built the path with an empty variable in a
    // wildcard slot — `/entities//attrs` is "POST attrs on entity id
    // <empty>". Surface this as 400 BadRequestData rather than 404
    // ResourceNotFound: there's no route the path could have matched,
    // but the request is malformed, not pointing at the wrong place.
    if (swRest.in.urlPath != NULL && strstr(swRest.in.urlPath, "//") != NULL)
    {
      swRestProblem(400, SW_REST_ERROR_BAD_REQUEST, "Bad Request",
                    "empty path segment in '%s' (likely a missing URL "
                    "variable)", swRest.in.urlPath);
      goto respond;
    }

    // § 6.3.2: 404 ResourceNotFound when the path matches NO route, but
    // 405 MethodNotAllowed when the path is registered for some other
    // verb. The Allow: header lists the verbs that ARE allowed on this
    // path (RFC 7231 § 7.4.1).
    char allow[128];
    int  pos = 0;
    for (int v = 0; v < SwVerbs; v++)
    {
      if (v == swRest.in.verb) continue;
      SwRestService* probe = swRestServiceLookup(&swRestServiceV[v]);
      if (probe == NULL) continue;
      if (pos > 0) { allow[pos++] = ','; allow[pos++] = ' '; }
      const char* vs = swRestVerbToString((SwRestVerb) v);
      int         vl = strlen(vs);
      if (pos + vl >= (int) sizeof(allow)) break;
      memcpy(&allow[pos], vs, vl);
      pos += vl;
    }
    allow[pos] = 0;

    if (pos > 0)
    {
      swRest.out.headerV[swRest.out.headerCount].key   = "Allow";
      swRest.out.headerV[swRest.out.headerCount].value = kaStrdup(&swRest.kalloc, allow);
      swRest.out.headerCount++;
      swRestProblem(405, SW_REST_ERROR_METHOD, "Method Not Allowed",
                    "%s %s — supported methods: %s",
                    swRest.in.verbString ? swRest.in.verbString : "?",
                    swRest.in.urlPath    ? swRest.in.urlPath    : "?",
                    allow);
    }
    else
    {
      swRestProblem(404, SW_REST_ERROR_NOT_FOUND, "Not Found",
                    "%s %s is not a recognized resource",
                    swRest.in.verbString ? swRest.in.verbString : "?",
                    swRest.in.urlPath    ? swRest.in.urlPath    : "?");
    }
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
        // § 6.3.20: an unknown query parameter is an InvalidRequest, not
        // BadRequestData (which the spec reserves for semantic errors in
        // the NGSI-LD payload).
        swRestProblem(400, SW_REST_ERROR_INVALID_REQ, "Invalid Request",
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

  // Wildcard semantic validation (entity/sub/reg/ctx ids, attr name, instance
  // id) belongs to the NGSI-LD layer — swNgsild's preServiceHook does it.

  // Body-presence check for write verbs (POST / PUT / PATCH) on services
  // that carry an LdOp OR live under /ngsi-ld/ (jsonldContexts and
  // entityMaps have ldOp=0 but still need a body). Admin paths (/admin/,
  // anything else not under /ngsi-ld/) opt out — some are body-less by
  // design (POST /admin/subStats/flush, etc.).
  bool isNgsildPath = (swRest.serviceP->url != NULL &&
                       strncmp(swRest.serviceP->url, "/ngsi-ld/", 9) == 0);
  bool needsBody    = (swRest.serviceP->ldOp != 0 || isNgsildPath);
  if (needsBody &&
      (swRest.in.verb == SwVerbPost  ||
       swRest.in.verb == SwVerbPut   ||
       swRest.in.verb == SwVerbPatch))
  {
    if (swRest.in.payload != NULL && swRest.in.requestTree == NULL)
    {
      // Failed JSON parse: per § 4.9 / § 6.3.4 this is InvalidRequest at 400,
      // not 415 (which is reserved for unsupported Content-Type).
      swRestProblem(400, SW_REST_ERROR_INVALID_REQ, "Invalid Request",
                    "request body is not valid JSON");
      goto respond;
    }
    if (swRest.in.requestTree == NULL)
    {
      // § 5.5.4: an empty body is "not a valid JSON document" → InvalidRequest.
      swRestProblem(400, SW_REST_ERROR_INVALID_REQ, "Invalid Request", "no payload");
      goto respond;
    }
  }

  // Skip the service routine when an earlier hook (parseHook, paramHook,
  // preServiceHook) already set a problem detail — the response is fully
  // determined and the service routine has nothing to do.
  if (swRest.out.problemType != NULL)
    goto respond;

  // Dispatch to service routine
  swRest.serviceP->serviceRoutine();

  // Call render hook unconditionally — the renderHook does the
  // § 6.3.4 Accept-negotiation 406 check, which must override an
  // earlier 4xx (e.g. retrieving a non-existent entity with an
  // unacceptable Accept header should answer 406, not 404).
  // The hook short-circuits its body-formatting work when problemType
  // is already set, so the only effect on the error path is the 406
  // check at the top.
  swRestPayloadRenderHook();

  // If service set a problem type but forgot status code, default to 500
  if (swRest.out.httpStatusCode == 200 && swRest.out.problemType != NULL)
    swRest.out.httpStatusCode = 500;

 respond:
  ;

  // Build problem details response tree if needed (RFC 7807 / § 5.2.16).
  // `status` SHOULD carry the HTTP status code — clients (e.g. the ETSI
  // testsuite's `${response.json()['errors'][0]['error']['status']}`
  // accessor) rely on it.
  if (swRest.out.problemType != NULL && swRest.out.responseTree == NULL)
  {
    swRest.out.responseTree = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(swRest.out.responseTree, kjString (swRest.kjsonP, "type",   swRest.out.problemType));
    kjChildAdd(swRest.out.responseTree, kjString (swRest.kjsonP, "title",  swRest.out.problemTitle));
    kjChildAdd(swRest.out.responseTree, kjInteger(swRest.kjsonP, "status", swRest.out.httpStatusCode));
    kjChildAdd(swRest.out.responseTree, kjString (swRest.kjsonP, "detail", swRest.out.problemDetail));
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

  // Hand the rendered body back through swRest.out so both the connection
  // handler and in-process self-forward callers read it uniformly.
  swRest.out.payload     = responseBody;
  swRest.out.payloadSize = responseBodySize;
}



// -----------------------------------------------------------------------------
//
// swRestSelfForwardDepth - per-thread guard against runaway in-process forwards
//
#define SW_REST_SELF_FORWARD_MAX_DEPTH 8
static __thread int swRestSelfForwardDepth = 0;



// -----------------------------------------------------------------------------
//
// swRestProcessInProcess - run a distributed-op forward in-process (self-forward)
//
// The NGSI-LD layer detected that a forward targets this broker's own endpoint.
// Rather than open a socket back to ourselves (a blocking round-trip that stalls
// the epoll pool thread), run the request directly on a fresh inner SwRestState
// and hand back the rendered result — exactly as if it had come off a socket.
//
// The inner request runs the full dispatch pipeline plus the post-response hook,
// so its own deferred work (e.g. notifications for the forwarded write) fires
// before the inner arena is freed. The inner pipeline resets thread-local
// application state (swNgsild + deferred caches); the CALLER must save/restore
// that around this call — see ldDistOp.c.
//
// Outputs are borrowed into respAllocP, which the caller must keep alive.
// Returns the HTTP status code, or -1 if the inner request could not be set up.
//
int swRestProcessInProcess(SwRestVerb       verb,
                           const char*      path,
                           SwRestKeyValue*  headerV,
                           int              headerCount,
                           const char*      body,
                           int              bodyLen,
                           KAlloc*          respAllocP,
                           char**           respBodyP,
                           int*             respBodyLenP,
                           SwRestKeyValue** respHeaderVP,
                           int*             respHeaderCountP)
{
  if (respBodyP        != NULL) *respBodyP        = NULL;
  if (respBodyLenP     != NULL) *respBodyLenP     = 0;
  if (respHeaderVP     != NULL) *respHeaderVP     = NULL;
  if (respHeaderCountP != NULL) *respHeaderCountP = 0;

  // Via-based loop detection normally stops an in-process forward from matching
  // the same CSR and self-forwarding again; guard the recursion depth too in
  // case alias matching is ever misconfigured.
  if (swRestSelfForwardDepth >= SW_REST_SELF_FORWARD_MAX_DEPTH)
    return -1;

  SwRestState* outerP = swRestP;
  SwRestState* innerP = (SwRestState*) malloc(sizeof(SwRestState));
  if (innerP == NULL)
    return -1;

  swRestP = innerP;
  swRestSelfForwardDepth++;

  swRestStateInit(NULL, path, swRestVerbToString(verb));

  // Same logical request as the outer — reuse its start time so the inner's
  // createdAt/modifiedAt stay consistent with the originating request.
  swRest.requestStartTime     = outerP->requestStartTime;
  swRest.requestStartTimeMono = outerP->requestStartTimeMono;

  // URI params (swRestStateInit split a trailing ?query into swRest.in.urlParams)
  parseUriParams();

  // Request headers — borrowed; the (outer) arena they live in stays alive
  for (int i = 0; i < headerCount; i++)
    addHttpHeader(headerV[i].key, headerV[i].value);

  // Body — copy into the inner arena (freed by swRestStateRelease)
  if (body != NULL && bodyLen > 0)
  {
    swRest.in.payload = (char*) kaAlloc(&swRest.kalloc, bodyLen + 1);
    if (swRest.in.payload != NULL)
    {
      memcpy(swRest.in.payload, body, bodyLen);
      swRest.in.payload[bodyLen] = 0;
      swRest.in.payloadSize      = bodyLen;
    }
  }

  swRestProcessRequest();

  int status = swRest.out.httpStatusCode;

  // Copy the rendered body + response headers into the caller's arena BEFORE the
  // inner arena (where they currently live) is released.
  if (respBodyP != NULL && swRest.out.payload != NULL && swRest.out.payloadSize > 0)
  {
    char* b = (char*) kaAlloc(respAllocP, swRest.out.payloadSize + 1);
    if (b != NULL)
    {
      memcpy(b, swRest.out.payload, swRest.out.payloadSize);
      b[swRest.out.payloadSize] = 0;
      *respBodyP = b;
      if (respBodyLenP != NULL) *respBodyLenP = swRest.out.payloadSize;
    }
  }

  if (respHeaderVP != NULL && swRest.out.headerCount > 0)
  {
    int n = swRest.out.headerCount;
    SwRestKeyValue* hv = (SwRestKeyValue*) kaAlloc(respAllocP, n * sizeof(SwRestKeyValue));
    if (hv != NULL)
    {
      for (int i = 0; i < n; i++)
      {
        hv[i].key   = kaStrdup(respAllocP, swRest.out.headerV[i].key);
        hv[i].value = kaStrdup(respAllocP, swRest.out.headerV[i].value);
      }
      *respHeaderVP = hv;
      if (respHeaderCountP != NULL) *respHeaderCountP = n;
    }
  }

  // Flush the inner request's deferred post-response work (notifications for the
  // forwarded write, etc.) while its arena is still alive.
  swRestPostResponseHook();

  swRestStateRelease();
  free(innerP);

  swRestSelfForwardDepth--;
  swRestP = outerP;

  return status;
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
  // --- First call: allocate this connection's per-request state ---
  if (*con_cls == NULL)
  {
    // Each connection owns its SwRestState (hung on con_cls), so when the
    // epoll pool thread interleaves connection B's callbacks between
    // connection A's body-read callbacks it can no longer clobber A's state.
    // swRestP is bound to it before swRestStateInit (whose memset/init runs
    // through the swRest macro).
    SwRestState* conP = (SwRestState*) malloc(sizeof(SwRestState));
    if (conP == NULL)
      return MHD_NO;
    *con_cls = conP;
    swRestP  = conP;

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

    // § 6.3.4 — POST / PATCH / PUT to NGSI-LD endpoints must carry
    // Content-Length. § 6.3.2 — 413 when the announced body exceeds
    // the broker cap. Non-NGSI-LD endpoints (e.g. /admin/*) are
    // outside the spec's scope and accept bodyless POSTs.
    if ((swRest.in.verb == SwVerbPost ||
         swRest.in.verb == SwVerbPut  ||
         swRest.in.verb == SwVerbPatch) &&
        url != NULL &&
        strncmp(url, "/ngsi-ld/", 9) == 0)
    {
      const char* clHdr = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length");
      if (clHdr == NULL)
      {
        // § 6.3.4 — just a 411 status, no body.
        swRest.out.httpStatusCode    = 411;
        swRest.in.contentLengthMissing = true;
      }
      else if (swRestMaxRequestSize > 0)
      {
        unsigned long long cl = strtoull(clHdr, NULL, 10);
        if (cl > swRestMaxRequestSize)
        {
          swRestProblem(413, SW_REST_ERROR_REQUEST_LENGTH, "Request Entity Too Large",
                        "request body of %llu bytes exceeds broker limit of %llu bytes",
                        cl, swRestMaxRequestSize);
        }
      }
    }

    return MHD_YES;
  }

  // Subsequent calls (body chunks, final dispatch): rebind swRestP to THIS
  // connection's state — another connection's callback may have re-pointed
  // swRestP on this pool thread since our last invocation here.
  swRestP = (SwRestState*) *con_cls;

  // --- Middle calls: accumulate payload ---
  if (*uploadDataSize > 0)
  {
    // If the first-call check flagged 411/413, drop the bytes.
    if (swRest.in.contentLengthMissing || swRest.out.httpStatusCode == 413)
    {
      *uploadDataSize = 0;
      return MHD_YES;
    }

    // Streaming size cap — defends against clients that lie in
    // Content-Length or use chunked encoding without a length.
    if (swRestMaxRequestSize > 0 &&
        (unsigned long long)(swRest.in.payloadSize + *uploadDataSize) > swRestMaxRequestSize)
    {
      swRestProblem(413, SW_REST_ERROR_REQUEST_LENGTH, "Request Entity Too Large",
                    "request body exceeds broker limit of %llu bytes", swRestMaxRequestSize);
      *uploadDataSize = 0;
      return MHD_YES;
    }

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
  swRestProcessRequest();

  char* responseBody     = (swRest.out.payload != NULL) ? swRest.out.payload : (char*) "";
  int   responseBodySize = swRest.out.payloadSize;

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
  {
    // TS 104-176 specifies bare media types throughout (§ 6.2.3, § 6.3.3,
    // § 6.4.7.2 "exactly equal to the media type"); RFC 8259 defines no charset
    // parameter for application/json. So emit the Content-Type verbatim — no
    // charset is appended. Callers (e.g. metrics) own any qualifier they set.
    MHD_add_response_header(response, "Content-Type", swRest.out.contentType);
  }

  // § 6.3.6 Prefer / Preference-Applied: when the client sent a
  // "Prefer: ngsi-ld=<x>" header, echo "Preference-Applied: ngsi-ld=<our-version>"
  // so they know what spec version the response body conforms to.
  // We respond with our supported version regardless of <x> — full v1.9.1
  // degradation per § 4.3.6.8 is not implemented.
  for (int hi = 0; hi < swRest.in.httpHeaderCount; hi++)
  {
    if (swRest.in.httpHeaderV[hi].key != NULL &&
        strcasecmp(swRest.in.httpHeaderV[hi].key, "Prefer") == 0 &&
        swRest.in.httpHeaderV[hi].value != NULL &&
        strncasecmp(swRest.in.httpHeaderV[hi].value, "ngsi-ld=", 8) == 0)
    {
      MHD_add_response_header(response, "Preference-Applied", "ngsi-ld=1.9.1");
      break;
    }
  }

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
    // Bind to this connection's state before the hook / release touch swRest.
    SwRestState* conP = (SwRestState*) *con_cls;
    swRestP = conP;

    // Run post-response hook BEFORE releasing the per-request arena so the
    // hook can still touch arena-allocated data (e.g. deferred notification
    // dispatch reading the entity tree built by the service routine).
    swRestPostResponseHook();

    // Free payload buffer (malloc'd during accumulation, not in kalloc)
    free(swRest.in.payload);
    swRest.in.payload = NULL;

    swRestStateRelease();

    free(conP);
    *con_cls = NULL;
    swRestP  = NULL;   // no dangling pointer to freed state on this thread
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
