//
// FILE            corRestStateInit.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_STATE_INIT_H_
#define CORREST_STATE_INIT_H_



// -----------------------------------------------------------------------------
//
// corRestStateInit - initialize the thread-local corRest for a new request
//
// 'connection' is the HTTP backend's own handle for the connection, stored
// untouched in corRest.connection - see CorRestState.h. NULL for a request that
// arrived on no connection at all (an in-process self-forward).
//
extern void corRestStateInit(void* connection, const char* url, const char* method);



// -----------------------------------------------------------------------------
//
// corRestUrlPathNormalize - length, and a single trailing '/' removed
//
// Called by corRestStateInit, and AGAIN by a backend that has to percent-decode
// the path itself: decoding changes the length and can uncover the trailing
// slash that was written `%2F`. Idempotent, which is what lets it be called
// twice rather than duplicated.
//
extern void corRestUrlPathNormalize(void);



// -----------------------------------------------------------------------------
//
// corRestStateRelease - release per-request resources (bulk-free kalloc, etc.)
//
extern void corRestStateRelease(void);

#endif  // CORREST_STATE_INIT_H_
