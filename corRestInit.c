//
// FILE            corRestInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                     // calloc, realloc, free, atoi
#include <string.h>                     // strncpy, strstr, strcmp, memcpy, strcasecmp, strlen
#include <stdio.h>                      // fprintf, snprintf
#include <time.h>                       // clock_gettime, CLOCK_MONOTONIC
#include <pthread.h>                    // pthread_* (async worker pool)

#include <microhttpd.h>

#include "kalloc/kaAlloc.h"             // kaAlloc
#include "kalloc/kaStrdup.h"            // kaStrdup
#include "kjson/kjParse.h"              // kjParse
#include "kjson/kjRender.h"             // kjFastRender
#include "kjson/kjRenderSize.h"         // kjFastRenderSize
#include "kjson/kjBuilder.h"            // kjObject, kjString, kjChildAdd
#include "ktrace/kTrace.h"              // KT_V

#include "corRest/CorRestVerb.h"          // CorVerbs, corRestVerbToString
#include "corRest/CorRestService.h"       // CorRestService, CorRestServiceVector
#include "corRest/CorRestState.h"         // CorRestState, corRest
#include "corRest/corRestStateInit.h"     // corRestStateInit, corRestStateRelease
#include "corRest/corRestServiceLookup.h" // corRestServiceLookup
#include "corRest/corRestHooks.h"         // CorRestHook, etc.
#include "corRest/corRestProblem.h"       // COR_REST_ERROR_*, corRestProblem
#include "corRest/corRestParamRegistry.h" // corRestParamLookup
#include "corRest/CorRestStats.h"         // CorRestStats, corRestStats
#include "corRest/corRestInit.h"          // Own interface



// -----------------------------------------------------------------------------
//
// Globals
//
CorRestServiceVector   corRestServiceV[CorVerbs];
struct MHD_Daemon*    corRestDaemon = NULL;
static CorRestMetrics* metricsP = NULL;



// -----------------------------------------------------------------------------
//
// HTTPS server credentials (PEM contents, NULL == plain HTTP)
//
// The broker listens over plain HTTP and never sets these. A test server such
// as ftClient that must receive notifications over TLS calls
// corRestHttpsServerCredentialsSet() with the key+certificate PEM before
// corRestInit; the MHD daemon is then started with TLS enabled.
//
static char* httpsServerKey  = NULL;
static char* httpsServerCert = NULL;

void corRestHttpsServerCredentialsSet(char* keyPem, char* certPem)
{
  httpsServerKey  = keyPem;
  httpsServerCert = certPem;
}



// -----------------------------------------------------------------------------
//
// corRestMetricsSet -
//
void corRestMetricsSet(CorRestMetrics* metrics)
{
  metricsP = metrics;
}



// -----------------------------------------------------------------------------
//
// Hook globals (defined in corRestHooks.c)
//
extern CorRestHook            corRestPreDispatchHook;
extern CorRestHook            corRestPayloadParseHook;
extern CorRestHook            corRestPayloadRenderHook;
extern CorRestParamHook       corRestParamHookF;
extern CorRestPreServiceHook  corRestPreServiceHookF;
extern CorRestServiceInitHook corRestServiceInitHookF;
extern CorRestHook            corRestPostResponseHook;
extern CorRestUserDataAllocHook corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook  corRestUserDataFreeHookF;
extern unsigned long long    corRestMaxRequestSize;



// -----------------------------------------------------------------------------
//
// servicePrepare - expand a simplified service descriptor into a full one
//
static void servicePrepare(CorRestService* serviceP, CorRestServiceSimplified* simpleP)
{
  serviceP->url              = (char*) simpleP->url;
  serviceP->serviceRoutine   = simpleP->serviceRoutine;
  serviceP->payloadCheck     = simpleP->payloadCheck;
  serviceP->supportedParams  = simpleP->supportedParams;
  serviceP->ldOp             = simpleP->ldOp;
  serviceP->options          = simpleP->options;   // author-declared features; init hook adds wildcard bits below
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
  serviceP->matchAfterLastWildcardLen     = 0;
  serviceP->matchAfterLastWildcard[0]     = 0;

  char*  wildCardStart    = NULL;  // start of literal between *#1 and *#2
  char*  wildCardEnd      = NULL;  // end of literal between *#1 and *#2
  char*  thirdWildStart   = NULL;  // start of literal between *#2 and *#3
  char*  thirdWildEnd     = NULL;  // end of literal between *#2 and *#3
  char*  afterLastWild    = NULL;  // start of literal AFTER the last '*' (suffix, e.g. "/value")
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

      afterLastWild = &serviceP->url[ix + 1];  // text after THIS '*' — ends up pointing past the final wildcard

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

    // Literal suffix AFTER the last '*' (e.g. ".../attrs/*/value"). Only for
    // 2+ wildcards — for a single wildcard the after-* literal is already held
    // in matchForSecondWildcard (the "wildcard not at end" case). 0-length =>
    // the last wildcard runs to end-of-string (the common case), a no-op.
    if (serviceP->wildcards >= 2 && afterLastWild != NULL)
    {
      int suffixLen = (int) (&serviceP->url[ix] - afterLastWild);
      if (suffixLen > 0 && suffixLen < (int) sizeof(serviceP->matchAfterLastWildcard))
      {
        strncpy(serviceP->matchAfterLastWildcard, afterLastWild, suffixLen);
        serviceP->matchAfterLastWildcard[suffixLen] = 0;
        serviceP->matchAfterLastWildcardLen = suffixLen;
      }
    }
  }

  // Let the embedding library (corNgsild) inspect the URL pattern and set
  // any per-route options it cares about. Called once per service at init.
  if (corRestServiceInitHookF != NULL)
    corRestServiceInitHookF(serviceP);
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
  if (corRest.in.uriParamCount >= corRest.in.uriParamSize)
  {
    int newSize = corRest.in.uriParamSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV = (CorRestKeyValue*) kaAlloc(&corRest.kalloc, newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, corRest.in.uriParamV, corRest.in.uriParamCount * sizeof(CorRestKeyValue));

    corRest.in.uriParamV    = newV;
    corRest.in.uriParamSize = newSize;
  }

  corRest.in.uriParamV[corRest.in.uriParamCount].key   = name;
  corRest.in.uriParamV[corRest.in.uriParamCount].value  = value;
  corRest.in.uriParamV[corRest.in.uriParamCount].bit    = 0;   // resolved in the allowlist pass
  corRest.in.uriParamCount++;
}



// -----------------------------------------------------------------------------
//
// parseUriParams - parse query string into key-value pairs with percent-decoding
//
static void parseUriParams(void)
{
  if (corRest.in.urlParams == NULL)
    return;

  char* p = corRest.in.urlParams;

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
// addHttpHeader - append a request header to corRest.in, extracting well-knowns
//
// Shared by the MHD header iterator and the in-process self-forward path. The
// key/value pointers are borrowed (not copied) — the caller keeps them alive
// for the request's lifetime.
//
static void addHttpHeader(const char* key, const char* value)
{
  if (corRest.in.httpHeaderCount >= corRest.in.httpHeaderSize)
  {
    int newSize = corRest.in.httpHeaderSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV = (CorRestKeyValue*) kaAlloc(&corRest.kalloc, newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, corRest.in.httpHeaderV, corRest.in.httpHeaderCount * sizeof(CorRestKeyValue));

    corRest.in.httpHeaderV    = newV;
    corRest.in.httpHeaderSize = newSize;
  }

  corRest.in.httpHeaderV[corRest.in.httpHeaderCount].key   = (char*) key;
  corRest.in.httpHeaderV[corRest.in.httpHeaderCount].value  = (char*) value;
  corRest.in.httpHeaderCount++;

  // Extract well-known headers
  if (strcasecmp(key, "Content-Type") == 0)
  {
    corRest.in.contentType = (char*) value;
    corRest.in.contentMime = corMimeTypeParse(value);
  }
  else if (strcasecmp(key, "Accept") == 0)
    corRest.in.accept = (char*) value;
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
  if (corRest.in.uriParamCount >= corRest.in.uriParamSize)
  {
    int newSize = corRest.in.uriParamSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV = (CorRestKeyValue*) kaAlloc(&corRest.kalloc, newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return MHD_YES;

    memcpy(newV, corRest.in.uriParamV, corRest.in.uriParamCount * sizeof(CorRestKeyValue));

    corRest.in.uriParamV    = newV;
    corRest.in.uriParamSize = newSize;
  }

  corRest.in.uriParamV[corRest.in.uriParamCount].key   = (char*) key;
  corRest.in.uriParamV[corRest.in.uriParamCount].value  = (char*) (value ? value : "");
  corRest.in.uriParamV[corRest.in.uriParamCount].bit    = 0;   // resolved in the allowlist pass
  corRest.in.uriParamCount++;

  return MHD_YES;
}



// -----------------------------------------------------------------------------
//
// corRestProcessRequest - run the request-dispatch core on the bound corRest state
//
// Connection-free: consumes corRest.in (verb, url, headers, params, requestTree)
// and produces corRest.out (status, headers, problem / responseTree) plus the
// final rendered body in corRest.out.payload / payloadSize. The MHD send stays in
// the connection handler. Reusable for an in-process self-forward (a distop
// whose endpoint is this broker): bind corRestP to an inner state, populate its
// .in, call this, read its .out — no socket round-trip.
//
void corRestProcessRequest(void)
{
  // Pre-dispatch hook: reset per-request application state (e.g. corNgsild)
  // HERE, at the start of the atomic dispatch — not at first-byte. With the
  // epoll pool another connection's request may run on this thread between
  // our first and final callbacks; resetting at dispatch keeps that state's
  // reset/populate/read all inside this atomic dispatch, so no stale values
  // leak in. (The whole final call runs without yielding to the pool.)
  corRestPreDispatchHook();

  // § 6.3.4 — POST/PATCH/PUT without Content-Length: emit 411 with no
  // body and skip everything else.
  if (corRest.in.contentLengthMissing)
  {
    corRest.out.httpStatusCode = 411;
    goto respond;
  }

  // 413 already set by first-call or middle-call: short-circuit to render.
  if (corRest.out.problemType != NULL && corRest.out.httpStatusCode == 413)
    goto respond;

  // A method outside NGSI-LD's seven verbs (corRestVerbFromString → CorVerbs)
  // splits two ways (§ 6.2.1 + RFC 9110 § 9.3):
  //   - a VALID HTTP method the broker does not use (TRACE, CONNECT) is
  //     "recognised but not allowed on this path" → leave serviceP NULL and
  //     fall through to the 405 + Allow handling below;
  //   - anything else is not an HTTP method at all → malformed → 400.
  // Either way, guard before the verb-indexed lookup, which would otherwise
  // read corRestServiceV[] out of bounds at index CorVerbs.
  if (corRest.in.verb == CorVerbs)
  {
    const char* m = corRest.in.verbString;
    if (m == NULL || (strcmp(m, "TRACE") != 0 && strcmp(m, "CONNECT") != 0))
    {
      corRestProblem(400, COR_REST_ERROR_BAD_REQUEST, "Invalid HTTP Method",
                    "unrecognized HTTP method '%s'", m ? m : "?");
      goto respond;
    }
    corRest.serviceP = NULL;   // TRACE/CONNECT — no NGSI-LD service → 405 below
  }
  else
    // Lookup service early — the parse hook needs to know the route so it can
    // tailor validation (e.g. batch ops surface per-element @context errors as
    // 207 entries instead of a global 400). serviceP may stay NULL here; the
    // 404/405 handling below still runs in that case.
    corRest.serviceP = corRestServiceLookup(&corRestServiceV[corRest.in.verb]);

  // § 6.3.4 — Unsupported Media Type. POST / PATCH / PUT on a body-bearing
  // NGSI-LD endpoint with a Content-Type that isn't application/json or
  // application/ld+json (with or without a charset parameter): 415, body
  // empty (spec wording: "shall result in just a 415 HTTP status code
  // (without any payload body)"). Run before the JSON parse so we don't
  // 400 InvalidRequest on a body that was never meant to be JSON.
  if (corRest.in.payloadSize > 0 && corRest.in.contentType != NULL &&
      (corRest.in.verb == CorVerbPost  ||
       corRest.in.verb == CorVerbPut   ||
       corRest.in.verb == CorVerbPatch))
  {
    CorMimeType mime = corRest.in.contentMime;
    bool ok = (mime == CorMimeJson || mime == CorMimeLdJson);

    // PATCH also accepts application/merge-patch+json (RFC 7396 / NGSI-LD
    // § 5.6.18 Merge / § 5.6.5 Partial Update). Other media types remain 415.
    if (!ok && corRest.in.verb == CorVerbPatch && mime == CorMimeMergePatchJson)
      ok = true;

    // A notification receiver (ftClient) accepts geo+json notifications.
    if (!ok && corRestAcceptGeoJsonInput && mime == CorMimeGeoJson)
      ok = true;

    if (!ok)
    {
      corRest.out.httpStatusCode = 415;
      goto respond;
    }
  }

  // Parse incoming JSON payload (if any)
  if (corRest.in.payloadSize > 0)
  {
    corRest.in.requestTree = kjParse(corRest.kjsonP, corRest.in.payload);

    if (corRest.in.requestTree != NULL)
      corRestPayloadParseHook();
  }

  // Extract ?pretty=N (remove from param list so it doesn't hit validation)
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    if (strcmp(corRest.in.uriParamV[i].key, "pretty") == 0)
    {
      corRest.in.prettySpaces = atoi(corRest.in.uriParamV[i].value);

      // Remove from param list
      corRest.in.uriParamCount--;
      if (i < corRest.in.uriParamCount)
        corRest.in.uriParamV[i] = corRest.in.uriParamV[corRest.in.uriParamCount];
      i--;
    }
  }

  if (corRest.serviceP == NULL)
  {
    // Empty path segment (`//` somewhere in the path) almost always
    // means the client URL-built the path with an empty variable in a
    // wildcard slot — `/entities//attrs` is "POST attrs on entity id
    // <empty>". Surface this as 400 BadRequestData rather than 404
    // ResourceNotFound: there's no route the path could have matched,
    // but the request is malformed, not pointing at the wrong place.
    if (corRest.in.urlPath != NULL && strstr(corRest.in.urlPath, "//") != NULL)
    {
      corRestProblem(400, COR_REST_ERROR_BAD_REQUEST, "Invalid URL Path",
                    "empty path segment in '%s' (likely a missing URL "
                    "variable)", corRest.in.urlPath);
      goto respond;
    }

    // § 6.3.2: 404 ResourceNotFound when the path matches NO route, but
    // 405 MethodNotAllowed when the path is registered for some other
    // verb. The Allow: header lists the verbs that ARE allowed on this
    // path (RFC 7231 § 7.4.1).
    //
    // The Allow-probe below calls corRestServiceLookup for the OTHER verbs;
    // a matching wildcard route NUL-terminates urlPath in place to delimit
    // its wildcard value. Capture the full path first so the 404/405 detail
    // reports the resource the client actually requested, not a truncation.
    const char* reqPath = (corRest.in.urlPath != NULL) ? kaStrdup(&corRest.kalloc, corRest.in.urlPath) : "?";

    char allow[128];
    int  pos = 0;
    for (int v = 0; v < CorVerbs; v++)
    {
      if (v == corRest.in.verb) continue;
      CorRestService* probe = corRestServiceLookup(&corRestServiceV[v]);
      if (probe == NULL) continue;
      if (pos > 0) { allow[pos++] = ','; allow[pos++] = ' '; }
      const char* vs = corRestVerbToString((CorRestVerb) v);
      int         vl = strlen(vs);
      if (pos + vl >= (int) sizeof(allow)) break;
      memcpy(&allow[pos], vs, vl);
      pos += vl;
    }
    allow[pos] = 0;

    if (pos > 0)
    {
      corRest.out.headerV[corRest.out.headerCount].key   = "Allow";
      corRest.out.headerV[corRest.out.headerCount].value = kaStrdup(&corRest.kalloc, allow);
      corRest.out.headerCount++;
      corRestProblem(405, COR_REST_ERROR_METHOD, "Method Not Allowed",
                    "%s %s — supported methods: %s",
                    corRest.in.verbString ? corRest.in.verbString : "?",
                    reqPath,
                    allow);
    }
    else
    {
      corRestProblem(404, COR_REST_ERROR_NOT_FOUND, "Not Found",
                    "%s %s is not a recognized resource",
                    corRest.in.verbString ? corRest.in.verbString : "?",
                    reqPath);
    }
    goto respond;
  }

  // Validate URL parameters against the service's supported set.
  // Sentinel: supportedParams == ~0ULL ("all bits set") means the
  // service accepts any URL param without validation — used by mocks
  // (ftClient) and other passthrough tools that don't pre-register
  // a vocabulary of known params.
  if (corRest.in.uriParamCount > 0 && corRest.serviceP->supportedParams != ~(uint64_t)0)
  {
    for (int i = 0; i < corRest.in.uriParamCount; i++)
    {
      uint64_t bit = corRestParamLookup(corRest.in.uriParamV[i].key);

      // Resolve the param's bit once, here: carry it on the entry and OR it into
      // the request-wide mask, so downstream code tests params by bit, not name.
      corRest.in.uriParamV[i].bit = bit;
      corRest.in.uriParamMask    |= bit;

      if (bit == 0 || (bit & corRest.serviceP->supportedParams) == 0)
      {
        // § 6.3.20: an unknown query parameter is an InvalidRequest, not
        // BadRequestData (which the spec reserves for semantic errors in
        // the NGSI-LD payload).
        corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request",
                      "Unknown/unsupported URL parameter: %s", corRest.in.uriParamV[i].key);
        goto respond;
      }

      if (corRestParamHookF != NULL)
        corRestParamHookF(corRest.in.uriParamV[i].key, corRest.in.uriParamV[i].value);
    }
  }

  // Pre-service hook (tenant resolution, etc.)
  if (!corRestPreServiceHookF())
    goto respond;

  // Wildcard semantic validation (entity/sub/reg/ctx ids, attr name, instance
  // id) belongs to the NGSI-LD layer — corNgsild's preServiceHook does it.

  // Body-presence check for write verbs (POST / PUT / PATCH) on services
  // that carry an LdOp OR live under /ngsi-ld/ (jsonldContexts and
  // entityMaps have ldOp=0 but still need a body). Admin paths (/admin/,
  // anything else not under /ngsi-ld/) opt out — some are body-less by
  // design (POST /admin/subStats/flush, etc.).
  bool isNgsildPath = (corRest.serviceP->url != NULL &&
                       strncmp(corRest.serviceP->url, "/ngsi-ld/", 9) == 0);
  bool needsBody    = (corRest.serviceP->ldOp != 0 || isNgsildPath);
  if (needsBody &&
      (corRest.in.verb == CorVerbPost  ||
       corRest.in.verb == CorVerbPut   ||
       corRest.in.verb == CorVerbPatch))
  {
    if (corRest.in.payload != NULL && corRest.in.requestTree == NULL)
    {
      // Failed JSON parse: per § 4.9 / § 6.3.4 this is InvalidRequest at 400,
      // not 415 (which is reserved for unsupported Content-Type).
      corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request",
                    "request body is not valid JSON");
      goto respond;
    }
    if (corRest.in.requestTree == NULL)
    {
      // § 5.5.4: an empty body is "not a valid JSON document" → InvalidRequest.
      corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request", "no payload");
      goto respond;
    }
  }

  // Skip the service routine when an earlier hook (parseHook, paramHook,
  // preServiceHook) already set a problem detail — the response is fully
  // determined and the service routine has nothing to do.
  if (corRest.out.problemType != NULL)
    goto respond;

  // Per-service payload-body validator (NGSI-LD pCheckXxx). Runs on the parsed
  // requestTree before the service routine, so a malformed body is rejected with
  // a pinpointed 400 instead of being half-ignored by the handler. NULL = none.
  if (corRest.serviceP->payloadCheck != NULL)
  {
    corRest.serviceP->payloadCheck();
    if (corRest.out.problemType != NULL)
      goto respond;
  }

  // Dispatch to service routine
  corRest.serviceP->serviceRoutine();

  // Call render hook unconditionally — the renderHook does the
  // § 6.3.4 Accept-negotiation 406 check, which must override an
  // earlier 4xx (e.g. retrieving a non-existent entity with an
  // unacceptable Accept header should answer 406, not 404).
  // The hook short-circuits its body-formatting work when problemType
  // is already set, so the only effect on the error path is the 406
  // check at the top.
  corRestPayloadRenderHook();

  // If service set a problem type but forgot status code, default to 500
  if (corRest.out.httpStatusCode == 200 && corRest.out.problemType != NULL)
    corRest.out.httpStatusCode = 500;

 respond:
  ;

  // Build problem details response tree if needed (RFC 7807 / § 5.2.16).
  // `status` SHOULD carry the HTTP status code — clients (e.g. the ETSI
  // testsuite's `${response.json()['errors'][0]['error']['status']}`
  // accessor) rely on it.
  if (corRest.out.problemType != NULL && corRest.out.responseTree == NULL)
  {
    corRest.out.responseTree = kjObject(corRest.kjsonP, NULL);
    kjChildAdd(corRest.out.responseTree, kjString (corRest.kjsonP, "type",   corRest.out.problemType));
    kjChildAdd(corRest.out.responseTree, kjString (corRest.kjsonP, "title",  corRest.out.problemTitle));
    kjChildAdd(corRest.out.responseTree, kjInteger(corRest.kjsonP, "status", corRest.out.httpStatusCode));
    kjChildAdd(corRest.out.responseTree, kjString (corRest.kjsonP, "detail", corRest.out.problemDetail));

    // RFC 9457 §3.2 extension members (e.g. registrationId of a failed forward).
    if (corRest.out.problemExtras != NULL)
    {
      KjNode* m = corRest.out.problemExtras->value.firstChildP;
      while (m != NULL)
      {
        KjNode* next = m->next;
        kjChildAdd(corRest.out.responseTree, m);   // splice across (re-parents m, clears m->next)
        m = next;
      }
    }
  }

  // Render response tree to JSON
  char*  responseBody     = NULL;
  int    responseBodySize = 0;

  if (corRest.out.responseTree != NULL)
  {
    if (corRest.in.prettySpaces > 0)
    {
      corRest.kjsonP->spacesPerIndent = corRest.in.prettySpaces;
      responseBodySize = kjRenderSize(corRest.kjsonP, corRest.out.responseTree) + 1;
      responseBody     = kaAlloc(&corRest.kalloc, responseBodySize);
      if (responseBody == NULL)
      {
        fprintf(stderr, "corRest: response body alloc failed (need %d bytes) — arena chunk too small?\n", responseBodySize);
        corRest.out.httpStatusCode = 500;
        responseBody     = (char*) "";
        responseBodySize = 0;
      }
      else
      {
        kjRender(corRest.kjsonP, corRest.out.responseTree, responseBody);
      }
    }
    else
    {
      responseBodySize = kjFastRenderSize(corRest.out.responseTree) + 1;
      responseBody     = kaAlloc(&corRest.kalloc, responseBodySize);
      if (responseBody == NULL)
      {
        fprintf(stderr, "corRest: response body alloc failed (need %d bytes) — arena chunk too small?\n", responseBodySize);
        corRest.out.httpStatusCode = 500;
        responseBody     = (char*) "";
        responseBodySize = 0;
      }
      else
      {
        kjFastRender(corRest.out.responseTree, responseBody);
      }
    }
    if (responseBody != NULL && responseBody[0] != 0)
      responseBodySize = strlen(responseBody);
  }
  else if (corRest.out.payload != NULL)
  {
    responseBody     = corRest.out.payload;
    responseBodySize = corRest.out.payloadSize;
  }
  else
  {
    responseBody     = (char*) "";
    responseBodySize = 0;
  }

  // Hand the rendered body back through corRest.out so both the connection
  // handler and in-process self-forward callers read it uniformly.
  corRest.out.payload     = responseBody;
  corRest.out.payloadSize = responseBodySize;
}



// -----------------------------------------------------------------------------
//
// corRestSelfForwardDepth - per-thread guard against runaway in-process forwards
//
#define COR_REST_SELF_FORWARD_MAX_DEPTH 8
static __thread int corRestSelfForwardDepth = 0;



// -----------------------------------------------------------------------------
//
// corRestProcessInProcess - run a distributed-op forward in-process (self-forward)
//
// The NGSI-LD layer detected that a forward targets this broker's own endpoint.
// Rather than open a socket back to ourselves (a blocking round-trip that stalls
// the epoll pool thread), run the request directly on a fresh inner CorRestState
// and hand back the rendered result — exactly as if it had come off a socket.
//
// The inner request runs the full dispatch pipeline plus the post-response hook,
// so its own deferred work (e.g. notifications for the forwarded write) fires
// before the inner arena is freed. The inner pipeline resets thread-local
// application state (corNgsild + deferred caches); the CALLER must save/restore
// that around this call — see ldDistOp.c.
//
// Outputs are borrowed into respAllocP, which the caller must keep alive.
// Returns the HTTP status code, or -1 if the inner request could not be set up.
//
int corRestProcessInProcess(CorRestVerb       verb,
                           const char*      path,
                           CorRestKeyValue*  headerV,
                           int              headerCount,
                           const char*      body,
                           int              bodyLen,
                           KAlloc*          respAllocP,
                           char**           respBodyP,
                           int*             respBodyLenP,
                           CorRestKeyValue** respHeaderVP,
                           int*             respHeaderCountP)
{
  if (respBodyP        != NULL) *respBodyP        = NULL;
  if (respBodyLenP     != NULL) *respBodyLenP     = 0;
  if (respHeaderVP     != NULL) *respHeaderVP     = NULL;
  if (respHeaderCountP != NULL) *respHeaderCountP = 0;

  // Via-based loop detection normally stops an in-process forward from matching
  // the same CSR and self-forwarding again; guard the recursion depth too in
  // case alias matching is ever misconfigured.
  if (corRestSelfForwardDepth >= COR_REST_SELF_FORWARD_MAX_DEPTH)
    return -1;

  CorRestState* outerP = corRestP;
  CorRestState* innerP = (CorRestState*) malloc(sizeof(CorRestState));
  if (innerP == NULL)
    return -1;

  corRestP = innerP;
  corRestSelfForwardDepth++;

  corRestStateInit(NULL, path, corRestVerbToString(verb));

  // The inner request gets its OWN application state (per-conn corNgsild), so it
  // can't clobber the paused outer's — this is what lets the distop layer drop
  // its corNgsild save/restore around the self-forward.
  if (corRestUserDataAllocHookF != NULL)
    corRest.userData = corRestUserDataAllocHookF();

  // Same logical request as the outer — reuse its start time so the inner's
  // createdAt/modifiedAt stay consistent with the originating request.
  corRest.requestStartTime     = outerP->requestStartTime;
  corRest.requestStartTimeMono = outerP->requestStartTimeMono;

  // URI params (corRestStateInit split a trailing ?query into corRest.in.urlParams)
  parseUriParams();

  // Request headers — borrowed; the (outer) arena they live in stays alive
  for (int i = 0; i < headerCount; i++)
    addHttpHeader(headerV[i].key, headerV[i].value);

  // Body — copy into the inner arena (freed by corRestStateRelease)
  if (body != NULL && bodyLen > 0)
  {
    corRest.in.payload = (char*) kaAlloc(&corRest.kalloc, bodyLen + 1);
    if (corRest.in.payload != NULL)
    {
      memcpy(corRest.in.payload, body, bodyLen);
      corRest.in.payload[bodyLen] = 0;
      corRest.in.payloadSize      = bodyLen;
    }
  }

  corRestProcessRequest();

  int status = corRest.out.httpStatusCode;

  // Copy the rendered body + response headers into the caller's arena BEFORE the
  // inner arena (where they currently live) is released.
  if (respBodyP != NULL && corRest.out.payload != NULL && corRest.out.payloadSize > 0)
  {
    char* b = (char*) kaAlloc(respAllocP, corRest.out.payloadSize + 1);
    if (b != NULL)
    {
      memcpy(b, corRest.out.payload, corRest.out.payloadSize);
      b[corRest.out.payloadSize] = 0;
      *respBodyP = b;
      if (respBodyLenP != NULL) *respBodyLenP = corRest.out.payloadSize;
    }
  }

  if (respHeaderVP != NULL && corRest.out.headerCount > 0)
  {
    int n = corRest.out.headerCount;
    CorRestKeyValue* hv = (CorRestKeyValue*) kaAlloc(respAllocP, n * sizeof(CorRestKeyValue));
    if (hv != NULL)
    {
      for (int i = 0; i < n; i++)
      {
        hv[i].key   = kaStrdup(respAllocP, corRest.out.headerV[i].key);
        hv[i].value = kaStrdup(respAllocP, corRest.out.headerV[i].value);
      }
      *respHeaderVP = hv;
      if (respHeaderCountP != NULL) *respHeaderCountP = n;
    }
  }

  // Flush the inner request's deferred post-response work (notifications for the
  // forwarded write, etc.) while its arena is still alive.
  corRestPostResponseHook();

  corRestStateRelease();
  if (corRestUserDataFreeHookF != NULL && corRest.userData != NULL)
    corRestUserDataFreeHookF(corRest.userData);
  free(innerP);

  corRestSelfForwardDepth--;
  corRestP = outerP;

  return status;
}



// -----------------------------------------------------------------------------
//
// -----------------------------------------------------------------------------
//
// Async worker pool (6d)
//
// The MHD I/O threads accept, parse headers and accumulate the body, then
// suspend the connection and hand its CorRestState to this pool. A worker binds
// corRestP to that state and runs the (potentially slow: DB, distops) dispatch
// off the I/O thread, then resumes the connection — MHD re-invokes the handler,
// which sends the already-built response. This decouples per-request processing
// latency from epoll I/O throughput: a slow request no longer blocks the I/O
// thread from servicing every other connection pinned to it.
//
// All request state lives in the per-connection CorRestState (arena, kjson,
// in/out, userData -> per-conn corNgsild), so a worker needs nothing but
// `corRestP = conP` — the I/O thread already ran corRestStateInit. The deferred
// notification caches are per-connection too, and still flushed by the
// post-response hook in mhdRequestCompleted, so notification ordering is
// unchanged.
//
static pthread_t*       corRestWorkerV    = NULL;
static int              corRestWorkerCount = 0;
static CorRestState*     corRestQueueHead  = NULL;
static CorRestState*     corRestQueueTail  = NULL;
static pthread_mutex_t  corRestQueueMtx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   corRestQueueCond  = PTHREAD_COND_INITIALIZER;
static volatile bool    corRestWorkersRun = false;

static void corRestWorkerEnqueue(CorRestState* conP)
{
  conP->asyncNext = NULL;

  pthread_mutex_lock(&corRestQueueMtx);
  if (corRestQueueTail != NULL)
    corRestQueueTail->asyncNext = conP;
  else
    corRestQueueHead = conP;
  corRestQueueTail = conP;
  pthread_cond_signal(&corRestQueueCond);
  pthread_mutex_unlock(&corRestQueueMtx);
}

static void* corRestWorkerMain(void* unused)
{
  (void) unused;

  while (true)
  {
    pthread_mutex_lock(&corRestQueueMtx);
    while (corRestQueueHead == NULL && corRestWorkersRun)
      pthread_cond_wait(&corRestQueueCond, &corRestQueueMtx);

    if (corRestQueueHead == NULL)   // woken with an empty queue => shutdown drain done
    {
      pthread_mutex_unlock(&corRestQueueMtx);
      break;
    }

    CorRestState* conP = corRestQueueHead;
    corRestQueueHead = conP->asyncNext;
    if (corRestQueueHead == NULL)
      corRestQueueTail = NULL;
    pthread_mutex_unlock(&corRestQueueMtx);

    // Run the dispatch on this worker, bound to the connection's own state.
    // A self-targeted forward (corRestProcessInProcess) runs synchronously
    // inside this same call on this same worker thread — never re-enqueued.
    corRestP = conP;
    corRestProcessRequest();
    conP->asyncProcessed = true;
    corRestP = NULL;                // drop the bind; the request leaves this thread

    // Hand the connection back to MHD (ITC wakes the polling thread); MHD
    // re-invokes mhdConnectionHandler, which sends corRest.out.
    MHD_resume_connection(conP->mhdConnection);
  }

  return NULL;
}

static int corRestWorkerPoolStart(int workers)
{
  if (workers < 1)
    workers = 1;

  corRestWorkerV = (pthread_t*) calloc(workers, sizeof(pthread_t));
  if (corRestWorkerV == NULL)
    return -1;

  corRestWorkersRun = true;

  for (int i = 0; i < workers; i++)
  {
    if (pthread_create(&corRestWorkerV[i], NULL, corRestWorkerMain, NULL) != 0)
    {
      corRestWorkerCount = i;    // join only the threads that started
      return -1;
    }
  }

  corRestWorkerCount = workers;
  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestWorkerPoolStop - stop new suspensions, drain the queue, join workers
//
// Clearing corRestWorkersRun first makes mhdConnectionHandler process inline
// again, so no NEW connection is suspended; the workers then drain whatever is
// already queued (processing + resuming each) before exiting. After this
// returns there are no suspended connections, so MHD_stop_daemon is safe
// (suspending across MHD_stop_daemon is an API violation).
//
void corRestWorkerPoolStop(void)
{
  if (corRestWorkerV == NULL)
    return;

  pthread_mutex_lock(&corRestQueueMtx);
  corRestWorkersRun = false;
  pthread_cond_broadcast(&corRestQueueCond);
  pthread_mutex_unlock(&corRestQueueMtx);

  for (int i = 0; i < corRestWorkerCount; i++)
    pthread_join(corRestWorkerV[i], NULL);

  free(corRestWorkerV);
  corRestWorkerV     = NULL;
  corRestWorkerCount = 0;
}



// -----------------------------------------------------------------------------
//
// mhdConnectionHandler - MHD callback, called for each incoming request
//
// MHD calls this multiple times per request:
//   1. First call:  *con_cls == NULL  -> init corRest state
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
    KT_V("Request: %s %s", method, url);  // one line per request (verbose mode, -v)

    // Each connection owns its CorRestState (hung on con_cls), so when the
    // epoll pool thread interleaves connection B's callbacks between
    // connection A's body-read callbacks it can no longer clobber A's state.
    // corRestP is bound to it before corRestStateInit (whose memset/init runs
    // through the corRest macro).
    CorRestState* conP = (CorRestState*) malloc(sizeof(CorRestState));
    if (conP == NULL)
      return MHD_NO;
    *con_cls = conP;
    corRestP  = conP;

    corRestStateInit(connection, url, method);

    // Create this connection's application state (e.g. per-conn corNgsild),
    // stored in corRest.userData and freed in mhdRequestCompleted.
    if (corRestUserDataAllocHookF != NULL)
      corRest.userData = corRestUserDataAllocHookF();

    // Capture request start time — REALTIME for timestamps, MONOTONIC for duration metrics
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    corRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;

    struct timespec tsM;
    clock_gettime(CLOCK_MONOTONIC, &tsM);
    corRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

    // Collect request headers from MHD
    MHD_get_connection_values(connection, MHD_HEADER_KIND, mhdHeaderIterator, NULL);

    // Collect URI query parameters from MHD (already percent-decoded by MHD)
    MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, mhdUriParamIterator, NULL);

    // § 6.3.4 — POST / PATCH / PUT to NGSI-LD endpoints must carry
    // Content-Length. § 6.3.2 — 413 when the announced body exceeds
    // the broker cap. Non-NGSI-LD endpoints (e.g. /admin/*) are
    // outside the spec's scope and accept bodyless POSTs.
    if ((corRest.in.verb == CorVerbPost ||
         corRest.in.verb == CorVerbPut  ||
         corRest.in.verb == CorVerbPatch) &&
        url != NULL &&
        strncmp(url, "/ngsi-ld/", 9) == 0)
    {
      const char* clHdr = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length");
      if (clHdr == NULL)
      {
        // § 6.3.4 — just a 411 status, no body.
        corRest.out.httpStatusCode    = 411;
        corRest.in.contentLengthMissing = true;
      }
      else if (corRestMaxRequestSize > 0)
      {
        unsigned long long cl = strtoull(clHdr, NULL, 10);
        if (cl > corRestMaxRequestSize)
        {
          corRestProblem(413, COR_REST_ERROR_REQUEST_LENGTH, "Request Entity Too Large",
                        "request body of %llu bytes exceeds broker limit of %llu bytes",
                        cl, corRestMaxRequestSize);
        }
      }
    }

    return MHD_YES;
  }

  // Subsequent calls (body chunks, final dispatch): rebind corRestP to THIS
  // connection's state — another connection's callback may have re-pointed
  // corRestP on this pool thread since our last invocation here.
  corRestP = (CorRestState*) *con_cls;

  // --- Middle calls: accumulate payload ---
  if (*uploadDataSize > 0)
  {
    // If the first-call check flagged 411/413, drop the bytes.
    if (corRest.in.contentLengthMissing || corRest.out.httpStatusCode == 413)
    {
      *uploadDataSize = 0;
      return MHD_YES;
    }

    // Streaming size cap — defends against clients that lie in
    // Content-Length or use chunked encoding without a length.
    if (corRestMaxRequestSize > 0 &&
        (unsigned long long)(corRest.in.payloadSize + *uploadDataSize) > corRestMaxRequestSize)
    {
      corRestProblem(413, COR_REST_ERROR_REQUEST_LENGTH, "Request Entity Too Large",
                    "request body exceeds broker limit of %llu bytes", corRestMaxRequestSize);
      *uploadDataSize = 0;
      return MHD_YES;
    }

    int needed = corRest.in.payloadSize + *uploadDataSize + 1;

    if (needed > corRest.payloadBufSize)
    {
      int newSize = (needed + 4096) & ~4095;
      char* newBuf = (char*) realloc(corRest.in.payload, newSize);
      if (newBuf == NULL)
        return MHD_NO;
      corRest.in.payload     = newBuf;
      corRest.payloadBufSize = newSize;
    }

    memcpy(corRest.in.payload + corRest.in.payloadSize, uploadData, *uploadDataSize);
    corRest.in.payloadSize += *uploadDataSize;
    corRest.in.payload[corRest.in.payloadSize] = 0;

    *uploadDataSize = 0;
    return MHD_YES;
  }

  // --- Final call: process (off the I/O thread when the pool is up), respond ---
  if (corRestWorkersRun && !corRest.asyncProcessed)
  {
    // Suspend this connection and hand it to a worker so DB/distop latency
    // doesn't block this epoll thread. The worker runs corRestProcessRequest and
    // resumes us; MHD then re-invokes this handler with asyncProcessed set and
    // we fall through to build + send the response. Suspend BEFORE enqueue so a
    // worker can never resume a not-yet-suspended connection.
    MHD_suspend_connection(connection);
    corRestWorkerEnqueue(corRestP);
    return MHD_YES;
  }

  if (!corRest.asyncProcessed)
    corRestProcessRequest();    // pool down (shutdown / tests): run inline here

  char* responseBody     = (corRest.out.payload != NULL) ? corRest.out.payload : (char*) "";
  int   responseBodySize = corRest.out.payloadSize;

  // Send HTTP response
  // For HEAD: pass the full body — MHD will set Content-Length correctly
  // but suppress the body in the actual response.
  struct MHD_Response* response;

  response = MHD_create_response_from_buffer(
    responseBodySize,
    (void*) responseBody,
    MHD_RESPMEM_MUST_COPY
  );

  // Content-Type policy:
  //  - No body (size 0): omit Content-Type — it describes a body there is none
  //    of (204 No Content, empty-body 201 Created, ...).
  //  - NGSI-LD § 6.3.x: 201 Created and error (4xx/5xx) responses are always
  //    application/json, regardless of the request's Accept.
  //  - Otherwise: the negotiated type (application/json / ld+json / geo+json).
  //
  // TS 104-176 specifies bare media types throughout (§ 6.2.3, § 6.3.3,
  // § 6.4.7.2 "exactly equal to the media type"); RFC 8259 defines no charset
  // parameter for application/json — so emit the type verbatim, no charset.
  if (responseBodySize > 0)
  {
    int         code = corRest.out.httpStatusCode;
    const char* ct   = (code == 201 || (code >= 400 && code <= 599))
                       ? "application/json"
                       : corRest.out.contentType;
    if (ct != NULL)
      MHD_add_response_header(response, "Content-Type", ct);
  }

  // § 6.3.6 Prefer / Preference-Applied: when the client sent a
  // "Prefer: ngsi-ld=<x>" header, echo "Preference-Applied: ngsi-ld=<our-version>"
  // so they know what spec version the response body conforms to.
  // We respond with our supported version regardless of <x> — full v1.9.1
  // degradation per § 4.3.6.8 is not implemented.
  for (int hi = 0; hi < corRest.in.httpHeaderCount; hi++)
  {
    if (corRest.in.httpHeaderV[hi].key != NULL &&
        strcasecmp(corRest.in.httpHeaderV[hi].key, "Prefer") == 0 &&
        corRest.in.httpHeaderV[hi].value != NULL &&
        strncasecmp(corRest.in.httpHeaderV[hi].value, "ngsi-ld=", 8) == 0)
    {
      MHD_add_response_header(response, "Preference-Applied", "ngsi-ld=1.9.1");
      break;
    }
  }

  // Add custom response headers
  for (int i = 0; i < corRest.out.headerCount; i++)
    MHD_add_response_header(response, corRest.out.headerV[i].key, corRest.out.headerV[i].value);

  // CORS headers (if configured)
  extern CorRestCorsConfig corRestCors;
  if (corRestCors.allowOrigin != NULL)
  {
    MHD_add_response_header(response, "Access-Control-Allow-Origin", corRestCors.allowOrigin);

    if (corRest.in.verb == CorVerbOptions)
    {
      // Preflight: echo Allow as Access-Control-Allow-Methods
      for (int i = 0; i < corRest.out.headerCount; i++)
      {
        if (strcmp(corRest.out.headerV[i].key, "Allow") == 0)
        {
          MHD_add_response_header(response, "Access-Control-Allow-Methods", corRest.out.headerV[i].value);
          break;
        }
      }

      const char* ah = corRestCors.allowHeaders ? corRestCors.allowHeaders : "Content-Type, Accept, Link";
      MHD_add_response_header(response, "Access-Control-Allow-Headers", ah);

      if (corRestCors.maxAge > 0)
      {
        char maxAgeBuf[16];
        snprintf(maxAgeBuf, sizeof(maxAgeBuf), "%d", corRestCors.maxAge);
        MHD_add_response_header(response, "Access-Control-Max-Age", maxAgeBuf);
      }
    }

    if (corRestCors.exposeHeaders != NULL)
      MHD_add_response_header(response, "Access-Control-Expose-Headers", corRestCors.exposeHeaders);
  }

  enum MHD_Result ret = MHD_queue_response(connection, corRest.out.httpStatusCode, response);
  MHD_destroy_response(response);

  // Update Prometheus metrics (if registered by application)
  if (metricsP != NULL)
  {
    if (metricsP->requests != NULL)
      kpromCounterInc(metricsP->requests);

    if (metricsP->responseBytes != NULL)
      kpromCounterAdd(metricsP->responseBytes, responseBodySize);

    if (corRest.out.httpStatusCode >= 400 && metricsP->requestErrors != NULL)
      kpromCounterInc(metricsP->requestErrors);

    if (metricsP->requestDuration != NULL)
    {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t nowNs  = (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
      double   durSec = (double)(nowNs - corRest.requestStartTimeMono) / 1000000000.0;
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
    // Bind to this connection's state before the hook / release touch corRest.
    CorRestState* conP = (CorRestState*) *con_cls;
    corRestP = conP;

    // Run post-response hook BEFORE releasing the per-request arena so the
    // hook can still touch arena-allocated data (e.g. deferred notification
    // dispatch reading the entity tree built by the service routine).
    corRestPostResponseHook();

    // Free payload buffer (malloc'd during accumulation, not in kalloc)
    free(corRest.in.payload);
    corRest.in.payload = NULL;

    corRestStateRelease();

    // Destroy this connection's application state (per-conn corNgsild, ...).
    if (corRestUserDataFreeHookF != NULL && corRest.userData != NULL)
      corRestUserDataFreeHookF(corRest.userData);

    free(conP);
    *con_cls = NULL;
    corRestP  = NULL;   // no dangling pointer to freed state on this thread
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
  const char* matchedUrl = corRest.serviceP ? corRest.serviceP->url : corRest.in.urlPath;

  char allow[128];
  int  pos = 0;

  for (int verb = 0; verb < CorVerbs; verb++)
  {
    for (int ix = 0; ix < corRestServiceV[verb].services; ix++)
    {
      if (strcmp(corRestServiceV[verb].serviceV[ix].url, matchedUrl) == 0)
      {
        if (pos > 0)
        {
          allow[pos++] = ',';
          allow[pos++] = ' ';
        }

        const char* vs = corRestVerbToString((CorRestVerb) verb);
        int         vl = strlen(vs);

        memcpy(&allow[pos], vs, vl);
        pos += vl;
        break;
      }
    }
  }

  allow[pos] = 0;

  corRest.out.httpStatusCode = 204;
  corRest.out.headerV[corRest.out.headerCount].key   = "Allow";
  corRest.out.headerV[corRest.out.headerCount].value  = kaStrdup(&corRest.kalloc, allow);
  corRest.out.headerCount++;

  return true;
}



// -----------------------------------------------------------------------------
//
// corRestInit -
//
int corRestInit(CorRestServiceSimplified serviceV[], int services, unsigned short port, int poolSize)
{
  // --- Count services per verb ---
  int counts[CorVerbs];
  memset(counts, 0, sizeof(counts));

  for (int ix = 0; ix < services; ix++)
  {
    if (serviceV[ix].verb < CorVerbs)
      counts[serviceV[ix].verb]++;

    // GET services also get HEAD for free
    if (serviceV[ix].verb == CorVerbGet)
      counts[CorVerbHead]++;
  }

  // OPTIONS gets one entry per unique URL path
  // (over-allocate: at most 'services' unique paths)
  counts[CorVerbOptions] += services;

  // --- Allocate per-verb arrays ---
  for (int verb = 0; verb < CorVerbs; verb++)
  {
    corRestServiceV[verb].services = 0;
    corRestServiceV[verb].serviceV = NULL;

    if (counts[verb] == 0)
      continue;

    corRestServiceV[verb].serviceV = (CorRestService*) calloc(counts[verb], sizeof(CorRestService));
    if (corRestServiceV[verb].serviceV == NULL)
      return -1;
  }

  // --- Distribute services into per-verb arrays ---
  for (int ix = 0; ix < services; ix++)
  {
    CorRestVerb verb = serviceV[ix].verb;
    if (verb >= CorVerbs)
      continue;

    CorRestServiceVector* sv = &corRestServiceV[verb];
    servicePrepare(&sv->serviceV[sv->services], &serviceV[ix]);
    sv->services++;

    // Auto-add HEAD for every GET (same handler; body suppressed at response time)
    if (verb == CorVerbGet)
    {
      CorRestServiceVector* headSv = &corRestServiceV[CorVerbHead];
      servicePrepare(&headSv->serviceV[headSv->services], &serviceV[ix]);
      headSv->services++;
    }
  }

  // --- Auto-generate OPTIONS for each unique URL path ---
  CorRestServiceVector* optSv = &corRestServiceV[CorVerbOptions];

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
      CorRestServiceSimplified optService = { 0 };   // zero ldOp + payloadCheck (no body validation on OPTIONS)

      optService.verb            = CorVerbOptions;
      optService.url             = serviceV[ix].url;
      optService.serviceRoutine  = optionsHandler;
      optService.supportedParams = 0;

      servicePrepare(&optSv->serviceV[optSv->services], &optService);
      optSv->services++;
    }
  }

  // Start MHD daemon with thread pool. MHD_ALLOW_SUSPEND_RESUME (which bundles
  // MHD_USE_ITC) lets the I/O threads suspend a connection and hand it to the
  // async worker pool below; the worker resumes it once the response is built.
  //
  // When HTTPS server credentials have been set (a TLS test receiver, not the
  // broker), MHD_USE_TLS is added together with the in-memory key/cert.
  unsigned int flags = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL | MHD_ALLOW_SUSPEND_RESUME;

  if ((httpsServerKey != NULL) && (httpsServerCert != NULL))
    corRestDaemon = MHD_start_daemon(
      flags | MHD_USE_TLS,
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
      MHD_OPTION_HTTPS_MEM_KEY,
      httpsServerKey,
      MHD_OPTION_HTTPS_MEM_CERT,
      httpsServerCert,
      MHD_OPTION_END
    );
  else
    corRestDaemon = MHD_start_daemon(
      flags,
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

  if (corRestDaemon == NULL)
  {
    fprintf(stderr, "corRestInit: MHD_start_daemon failed on port %d\n", port);
    return -1;
  }

  // Async worker pool — one worker per I/O thread. corRestWorkersRun stays false
  // (handler processes inline) if the pool fails to start.
  if (corRestWorkerPoolStart(poolSize) != 0)
  {
    fprintf(stderr, "corRestInit: async worker pool failed to start\n");
    return -1;
  }

  return 0;
}
