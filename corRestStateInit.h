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

#include <microhttpd.h>



// -----------------------------------------------------------------------------
//
// corRestStateInit - initialize the thread-local corRest for a new request
//
extern void corRestStateInit(struct MHD_Connection* connection, const char* url, const char* method);



// -----------------------------------------------------------------------------
//
// corRestStateRelease - release per-request resources (bulk-free kalloc, etc.)
//
extern void corRestStateRelease(void);

#endif  // CORREST_STATE_INIT_H_
