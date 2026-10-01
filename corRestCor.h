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
// v1: one TCP connection per peer and client thread, one request in flight on it. The frames carry
// correlation ids already; multiplexing is a later step that changes no byte of the format.
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
// corRestCorListen - accept cor:// connections on a port (a thread of its own); false on failure
//
extern bool corRestCorListen(unsigned short port);



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

#endif  // CORREST_CORRESTCOR_H_
