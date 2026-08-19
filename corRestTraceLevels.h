//
// FILE            corRestTraceLevels.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_TRACE_LEVELS_H_
#define CORREST_TRACE_LEVELS_H_



// Server trace levels
#define CortEvent     100   // Connection events
#define CortParse     101   // HTTP parsing
#define CortService   102   // Service lookup/matching
#define CortRequest   103   // Request processing
#define CortResponse  104   // Response building
#define CortPool      105   // Connection pool
#define CortRead      107   // Read operations
#define CortWrite     108   // Write operations
#define CortJson      109   // JSON operations

// Client trace levels
#define CortClientUrl    110   // URL parsing
#define CortClientConn   111   // Client connection management
#define CortClientPool   112   // Client connection pool
#define CortClientSend   113   // Client request sending
#define CortClientRecv   114   // Client response receiving
#define CortClientTls    115   // Client TLS operations
#define CortClientMulti  116   // Client concurrent requests

#endif  // CORREST_TRACE_LEVELS_H_
