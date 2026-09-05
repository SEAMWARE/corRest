//
// FILE            corRestBackendMhd.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The libmicrohttpd backend.
//
// Everything here is MHD's shape: its multi-call request callback, its value
// iterators, its response object, its suspend/resume. The request itself -
// what the URL means, what the body has to be, which headers go back and in
// what order - is on the other side of corRestBackend.h and is shared with the
// built-in server. This file translates, and decides nothing.
//
#include <stdlib.h>                       // strtoull, malloc, free, realloc
#include <string.h>                       // memcpy, strncmp
#include <stdio.h>                        // fprintf
#include <time.h>                         // clock_gettime

#include <microhttpd.h>

#include "ktrace/kTrace.h"                // KT_V

#include "corRest/CorRestVerb.h"          // CorVerbPost, ...
#include "corRest/CorRestState.h"         // CorRestState, corRest
#include "corRest/corRestStateInit.h"     // corRestStateInit, corRestStateRelease
#include "corRest/corRestHooks.h"         // CorRestHook, corRestSetMaxRequestSize
#include "corRest/corRestProblem.h"       // COR_REST_ERROR_*, corRestProblem
#include "corRest/corRestBackend.h"       // Own interface



// -----------------------------------------------------------------------------
//
// The daemon
//
static struct MHD_Daemon*  mhdDaemon = NULL;



// -----------------------------------------------------------------------------
//
// Hook globals (defined in corRestHooks.c)
//
extern CorRestUserDataAllocHook  corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook   corRestUserDataFreeHookF;
extern CorRestHook               corRestPostResponseHook;
extern unsigned long long        corRestMaxRequestSize;



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
  corRestHttpHeaderAdd(key, value);
  return MHD_YES;
}



// -----------------------------------------------------------------------------
//
// mhdUriParamIterator - MHD callback to collect URI query parameters
//
// MHD strips query params from the URL and provides them via MHD_GET_ARGUMENT_KIND,
// already percent-decoded — which is why this hands them straight over rather
// than going through corRestUriParamsParse (the built-in backend's route, since
// that server decodes nothing).
//
static enum MHD_Result mhdUriParamIterator
(
  void*              cls,
  enum MHD_ValueKind kind,
  const char*        key,
  const char*        value
)
{
  corRestUriParamAdd((char*) key, (char*) (value ? value : ""));
  return MHD_YES;
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

    corRestBodyPolicyCheck(url, MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length"));

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
  if (corRest.asyncProcessed == false)
  {
    if (corRestAsyncPoolUp() == true)
    {
      // Suspend this connection and hand it to a worker so DB/distop latency
      // doesn't block this epoll thread. The worker runs corRestProcessRequest
      // and resumes us; MHD then re-invokes this handler with asyncProcessed
      // set and we fall through to build + send the response. Suspend BEFORE
      // enqueue so a worker can never resume a not-yet-suspended connection.
      MHD_suspend_connection(connection);
      corRestAsyncEnqueue(corRestP);
      return MHD_YES;
    }

    corRestProcessRequest();    // pool down (shutdown / tests): run inline here
  }

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

  CorRestKeyValue headerV[COR_REST_RESPONSE_HEADERS_MAX];
  int             headers = corRestResponseHeaderVBuild(headerV, COR_REST_RESPONSE_HEADERS_MAX);

  for (int i = 0; i < headers; i++)
    MHD_add_response_header(response, headerV[i].key, headerV[i].value);

  enum MHD_Result ret = MHD_queue_response(connection, corRest.out.httpStatusCode, response);
  MHD_destroy_response(response);

  return ret;
}



// -----------------------------------------------------------------------------
//
// corRestBackendFinish -
//
// Runs on the MHD I/O thread that finished the request. NOT handed to a worker
// the way the built-in backend has to hand it: MHD has poolSize I/O threads, so
// a post-response phase that blocks - and it can, on an @context this broker
// hosts itself - stalls one of them and the others keep serving. That is why
// this backend is left exactly as it was.
//
void corRestBackendFinish(CorRestState* stateP)
{
  corRestP = stateP;

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

  free(stateP);

  corRestP = NULL;   // no dangling pointer to freed state on this thread
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
    CorRestState* conP = (CorRestState*) *con_cls;

    *con_cls = NULL;
    corRestBackendFinish(conP);
  }
}



// -----------------------------------------------------------------------------
//
// corRestBackendResume -
//
// The worker built the response into corRest.out; MHD sends it by re-invoking
// the connection handler, which it does once the connection is resumed (the ITC
// pipe wakes the polling thread).
//
void corRestBackendResume(CorRestState* stateP)
{
  MHD_resume_connection((struct MHD_Connection*) stateP->connection);
}



// -----------------------------------------------------------------------------
//
// corRestBackendStart -
//
int corRestBackendStart(unsigned short port, int poolSize, char* keyPem, char* certPem)
{
  // MHD_ALLOW_SUSPEND_RESUME (which bundles MHD_USE_ITC) lets the I/O threads
  // suspend a connection and hand it to the async worker pool; the worker
  // resumes it once the response is built.
  //
  // When HTTPS server credentials have been set (a TLS test receiver, not the
  // broker), MHD_USE_TLS is added together with the in-memory key/cert.
  unsigned int flags = MHD_USE_SELECT_INTERNALLY | MHD_USE_EPOLL | MHD_ALLOW_SUSPEND_RESUME;

  if ((keyPem != NULL) && (certPem != NULL))
    mhdDaemon = MHD_start_daemon(
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
      keyPem,
      MHD_OPTION_HTTPS_MEM_CERT,
      certPem,
      MHD_OPTION_END
    );
  else
    mhdDaemon = MHD_start_daemon(
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

  if (mhdDaemon == NULL)
  {
    fprintf(stderr, "corRestInit: MHD_start_daemon failed on port %d\n", port);
    return -1;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestBackendStop -
//
void corRestBackendStop(void)
{
  if (mhdDaemon != NULL)
  {
    MHD_stop_daemon(mhdDaemon);
    mhdDaemon = NULL;
  }
}
