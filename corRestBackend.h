//
// FILE            corRestBackend.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The seam between corRest and the HTTP server underneath it.
//
// There are two servers - libmicrohttpd and the built-in epoll one in corHttp -
// and exactly one of them is in a build (COR_HTTP_SERVER). Everything on the
// corRest side of this header is the same either way: the service table, the
// dispatch, the worker pool, the response policy. Everything on the other side
// is a socket and a callback shape.
//
// Not a public header: it is the INTERNAL contract between corRestInit.c and
// corRestBackend<Name>.c, and nothing above corRest includes it.
//
#ifndef CORREST_BACKEND_H_
#define CORREST_BACKEND_H_

#include <stdbool.h>                      // bool

#include "corRest/CorRestKeyValue.h"      // CorRestKeyValue
#include "corRest/CorRestState.h"         // CorRestState



// -----------------------------------------------------------------------------
//
// What the backend implements
//
// corRestBackendStart - listen on 'port' and serve, and RETURN.
//
//   Both backends run their own threads; neither blocks the caller, because
//   corRestInit's caller goes on to do other things and then parks. keyPem /
//   certPem are the HTTPS server credentials, NULL for plain HTTP - a backend
//   without TLS refuses rather than silently serving the port unencrypted.
//
//   Returns 0, or -1 with a reason on stderr.
//
extern int   corRestBackendStart(unsigned short port, int poolSize, char* keyPem, char* certPem);


//
// corRestBackendStop - stop serving; called after the worker pool has drained.
//
extern void  corRestBackendStop(void);


//
// corRestBackendFinish - the request is over; run its post-response work and
// release it
//
// The backend's, because only it knows what the request state BORROWED and what
// therefore has to be given back. Binds corRestP to stateP, runs the
// post-response hook, releases the arena, frees the state, and unbinds.
//
// Runs on a worker whenever there is a pool (corRestAsyncFinish), on the
// calling thread otherwise.
//
extern void  corRestBackendFinish(CorRestState* stateP);


//
// corRestBackendResume - a worker has built the response; send it
//
// Called ON THE WORKER THREAD with corRestP still bound to 'stateP', which is
// what lets a backend read corRest.out here. It must not write to the socket
// itself - that belongs to whichever thread owns the connection - and both
// backends therefore only hand the connection back to their event loop.
//
extern void  corRestBackendResume(CorRestState* stateP);



// -----------------------------------------------------------------------------
//
// What corRestInit.c provides to the backend
//
// corRestHttpHeaderAdd  - one request header; key/value are BORROWED, so they
//                         must live as long as the request does.
// corRestUriParamsParse - split + percent-decode corRest.in.urlParams into
//                         corRest.in.uriParamV. Destroys the buffer it parses.
// corRestUriParamAdd    - one already-split, already-decoded parameter.
//
extern void  corRestHttpHeaderAdd(const char* key, const char* value);
extern void  corRestUriParamsParse(void);
extern void  corRestUriParamAdd(char* name, char* value);


//
// corRestBodyPolicyCheck - the two answers that are decided before the body
//
// § 6.3.4: POST/PATCH/PUT to an NGSI-LD route must carry Content-Length (411,
// no payload). § 6.3.2: an announced body over the broker's cap is a 413.
// Both are known from the request line and the headers alone, which is why
// they are here and not in the dispatch: a backend that streams the body wants
// to stop reading it, and one that has it already wants to not parse it.
//
// 'clHeader' is the Content-Length header value, NULL when absent.
//
extern void  corRestBodyPolicyCheck(const char* url, const char* clHeader);


//
// corRestResponseHeaderVBuild - the response headers, in the order they go out
//
// ONE implementation of the header policy - content type, Preference-Applied,
// the service routine's own headers, CORS - for both backends, because the set
// and the ORDER of these headers is compared line by line by several hundred
// functional tests. Two copies of it would be two chances to drift.
//
// Fills 'hv' and returns how many. Keys and values are borrowed from the
// request arena and from static configuration, so they last as long as the
// request state does.
//
extern int   corRestResponseHeaderVBuild(CorRestKeyValue* hv, int max);

#define COR_REST_RESPONSE_HEADERS_MAX  64


//
// corRestAsyncPoolUp / corRestAsyncEnqueue - hand the request to a worker
//
// Two calls and not one, because the connection has to be SUSPENDED BETWEEN
// THEM: a worker can pick the request up and finish it before the enqueue has
// returned, and resuming a connection that was never suspended is an API
// violation in one backend and a lost response in the other.
//
//   if (corRestAsyncPoolUp())  { suspend(); corRestAsyncEnqueue(stateP); return; }
//   corRestProcessRequest();   // pool down (shutdown, or a server without one)
//
extern bool  corRestAsyncPoolUp(void);
extern void  corRestAsyncEnqueue(CorRestState* stateP);


//
// corRestAsyncFinish - hand the POST-RESPONSE phase to a worker
//
// Returns true when a worker has taken it - the caller must then not touch
// stateP again. False means there is no pool and the caller runs
// corRestBackendFinish itself.
//
// The response has already gone out, so this is not about latency: it is about
// the thread. The phase can issue an HTTP request the broker itself has to
// answer (a notification compacted with an @context the broker hosts), and a
// single-threaded event loop running it would be waiting on itself.
//
extern bool  corRestAsyncFinish(CorRestState* stateP);


//
// corRestWorkerPoolStart / Stop - one worker per I/O thread
//
extern int   corRestWorkerPoolStart(int workers);
extern void  corRestWorkerPoolStop(void);


//
// corRestProcessRequest - the dispatch itself (public; corRestInit.h)
//
extern void  corRestProcessRequest(void);

#endif  // CORREST_BACKEND_H_
