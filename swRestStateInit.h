//
// FILE            swRestStateInit.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_STATE_INIT_H_
#define SWREST_SW_REST_STATE_INIT_H_

#include <microhttpd.h>



// -----------------------------------------------------------------------------
//
// swRestStateInit - initialize the thread-local swRest for a new request
//
extern void swRestStateInit(struct MHD_Connection* connection, const char* url, const char* method);



// -----------------------------------------------------------------------------
//
// swRestStateRelease - release per-request resources (bulk-free kalloc, etc.)
//
extern void swRestStateRelease(void);

#endif  // SWREST_SW_REST_STATE_INIT_H_
