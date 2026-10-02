#ifndef CORREST_CORRESTCOR_H_
#define CORREST_CORRESTCOR_H_

//
// FILE            corRestCor.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// cor:// - the broker's API over the cor binary format (coraine doc/cor-protocol.md § 5).
//
// The same requests as HTTP, the same service routines - but what travels is the TREE: a request's
// body arrives as the tree the service routine works on, and a response leaves as the tree it built,
// with no JSON parse or render on either side.
//
// Multiplexed (doc § 5.3): a peer's connections are the whole process's, and any number of requests
// are in flight on each - a response is matched to its request by the frame's correlation id, and may
// come back in any order. No byte of the format changed for it.
//
#include <stdbool.h>                                  // bool

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBin.h"                       // CorBinCodec
#include "corRest/CorRestVerb.h"                      // CorRestVerb
#include "corRest/CorRestKeyValue.h"                  // CorRestKeyValue



// -----------------------------------------------------------------------------
//
// corRestCorInit - the codec, its fixed namespaces and its term count - both ends must agree
//
// termCount: the size of the codec's core-term table. HELLO compares it, and v1 refuses a peer
// whose table differs (a later version writes the newer terms as strings instead).
//
extern void corRestCorInit(const CorBinCodec* codecP, const char** namespaceV, int namespaces, int termCount);



// -----------------------------------------------------------------------------
//
// corRestCorListen - accept cor:// connections on a port, served by loopCount event loops; false on failure
//
extern bool corRestCorListen(unsigned short port, int loopCount);



// -----------------------------------------------------------------------------
//
// corRestCorClientConns - how many connections the client keeps to each peer (default 1, max 16)
//
// The calls to a peer take its connections in turn. A peer's server reads each connection on one of
// its event loops, so this many connections spread the requests over as many of its loops.
//
extern void corRestCorClientConns(int n);



// -----------------------------------------------------------------------------
//
// CorRestCorResponse - what a cor:// request answered
//
typedef struct CorRestCorResponse
{
  int               status;           // the HTTP status code
  CorRestKeyValue*  headerV;
  int               headerCount;
  CorNode*          bodyTree;         // the response body, as the peer built it - NULL if none
  char*             bodyText;         // or, for a body that was not a tree, its text
} CorRestCorResponse;



// -----------------------------------------------------------------------------
//
// corRestCorSend - one request over cor://
//
// url          cor://host:port - anything after the authority is ignored: the path is pathAndQuery
// pathAndQuery /ngsi-ld/v1/entities/urn:E1?options=keyValues
// bodyTree     the request body as a tree, or NULL - bodyText is then parsed into one if given
// respAllocP   where the response lives: its tree points into a buffer allocated here
//
// Blocking. Returns false on a transport failure (with *errorP); an HTTP error status is a response.
//
extern bool corRestCorSend(const char*         url,
                           CorRestVerb         verb,
                           const char*         pathAndQuery,
                           CorRestKeyValue*    headerV,
                           int                 headerCount,
                           CorNode*            bodyTree,
                           const char*         bodyText,
                           int                 timeoutMs,
                           CorAlloc*           respAllocP,
                           CorRestCorResponse* respP,
                           const char**        errorP);




// -----------------------------------------------------------------------------
//
// corRestCorStart / corRestCorWait - a request sent now, its response collected later
//
// For a fan-out: start every request, then wait for each. The requests are in flight at the same
// time, on as many connections as corRestCorClientConns allows, and multiplexed beyond.
//
// corRestCorStart: the request is encoded and sent before it returns - bodyTree (or bodyText, parsed
// into kaP) is the caller's again then. NULL only for a bad URL, a body that is not JSON, or no memory
// (with *errorP); any other failure is reported by corRestCorWait.
//
// corRestCorWait: blocks until the response is in, the call fails, or its time is up; the response
// lives in respAllocP, as corRestCorSend's does. Frees the call - every call started must be waited
// for, once.
//
// Between the two, respAllocP may be used freely: the response is decoded into memory of the call's
// own, by whichever thread reads it, and handed to respAllocP only in corRestCorWait.
//
typedef struct CorRestCorCall CorRestCorCall;

extern CorRestCorCall* corRestCorStart(const char*       url,
                                       CorRestVerb       verb,
                                       const char*       pathAndQuery,
                                       CorRestKeyValue*  headerV,
                                       int               headerCount,
                                       CorNode*          bodyTree,
                                       const char*       bodyText,
                                       int               timeoutMs,
                                       CorAlloc*         kaP,
                                       const char**      errorP);

extern bool corRestCorWait(CorRestCorCall* callP, CorAlloc* respAllocP, CorRestCorResponse* respP, const char** errorP);

#endif  // CORREST_CORRESTCOR_H_
