//
// FILE            corRestRun.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // memcpy
#include <time.h>                                     // clock_gettime

#include "corAlloc/corAlloc.h"                        // corAlloc, corAllocStrdup
#include "corRest/corRest.h"                          // corRest, corRestP
#include "corRest/CorRestState.h"                     // CorRestState
#include "corRest/corRestHooks.h"                     // CorRestHook, CorRestUserData*Hook
#include "corRest/corRestStateInit.h"                 // corRestStateInit, corRestStateRelease, corRestUrlPathNormalize
#include "corRest/corRestUrlValueEncode.h"            // corRestUrlValueDecode
#include "corRest/corRestBackend.h"                   // corRestHttpHeaderAdd, corRestUriParamsParse, corRestProcessRequest, ...
#include "corRest/corRestRun.h"                       // Own interface

extern CorRestUserDataAllocHook  corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook   corRestUserDataFreeHookF;
extern CorRestHook               corRestPostResponseHook;



// -----------------------------------------------------------------------------
//
// corRestRunJson -
//
// What an HTTP backend does for a request, in one go on this thread: the state, the request's time,
// the path decoded, the parameters, the headers and body copied into the request's arena, the
// processing, the response handed over, the post-response phase, the release.
//
void corRestRunJson
(
  const char*        verb,
  const char*        path,
  CorRestKeyValue*   headerV,
  int                headers,
  const char*        body,
  int                bodyLen,
  CorRestRunRespond  respond,
  void*              ctx
)
{
  CorRestState* savedP = corRestP;
  CorRestState* stateP = (CorRestState*) malloc(sizeof(CorRestState));

  if (stateP == NULL)
  {
    respond(503, NULL, 0, NULL, 0, ctx);
    return;
  }

  corRestP = stateP;
  corRestStateInit(NULL, path, verb);

  struct timespec ts;
  struct timespec tsM;

  clock_gettime(CLOCK_REALTIME,  &ts);
  clock_gettime(CLOCK_MONOTONIC, &tsM);
  corRest.requestStartTime     = (uint64_t) ts.tv_sec  * 1000000000ULL + (uint64_t) ts.tv_nsec;
  corRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

  corRest.in.verbString = corAllocStrdup(&corRest.kalloc, verb);

  corRestUrlValueDecode(corRest.in.urlPath);
  corRestUrlPathNormalize();

  if (corRestUserDataAllocHookF != NULL)
    corRest.userData = corRestUserDataAllocHookF();

  corRestUriParamsParse();

  for (int i = 0; i < headers; i++)
  {
    char* key   = corAllocStrdup(&corRest.kalloc, headerV[i].key);
    char* value = corAllocStrdup(&corRest.kalloc, headerV[i].value);

    if ((key != NULL) && (value != NULL))
      corRestHttpHeaderAdd(key, value);
  }

  char contentLength[16];

  snprintf(contentLength, sizeof(contentLength), "%d", bodyLen);
  corRestBodyPolicyCheck(corRest.in.urlPath, (bodyLen > 0) ? contentLength : NULL);

  if ((body != NULL) && (bodyLen > 0))
  {
    char* copy = (char*) corAlloc(&corRest.kalloc, bodyLen + 1);

    if (copy != NULL)
    {
      memcpy(copy, body, bodyLen);
      copy[bodyLen] = 0;

      corRest.in.payload     = copy;
      corRest.in.payloadSize = bodyLen;
    }
  }

  corRestProcessRequest();

  CorRestKeyValue hv[64];
  int             hc = corRestResponseHeaderVBuild(hv, 64);

  respond(corRest.out.httpStatusCode, hv, hc, corRest.out.payload, corRest.out.payloadSize, ctx);

  corRestPostResponseHook();
  corRestStateRelease();
  if ((corRestUserDataFreeHookF != NULL) && (corRest.userData != NULL))
    corRestUserDataFreeHookF(corRest.userData);
  free(stateP);

  corRestP = savedP;
}
