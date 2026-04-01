//
// FILE            swRestTraceLevels.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_TRACE_LEVELS_H_
#define SWREST_SW_REST_TRACE_LEVELS_H_



// Server trace levels
#define SwtEvent     100   // Connection events
#define SwtParse     101   // HTTP parsing
#define SwtService   102   // Service lookup/matching
#define SwtRequest   103   // Request processing
#define SwtResponse  104   // Response building
#define SwtPool      105   // Connection pool
#define SwtRead      107   // Read operations
#define SwtWrite     108   // Write operations
#define SwtJson      109   // JSON operations

// Client trace levels
#define SwtClientUrl    110   // URL parsing
#define SwtClientConn   111   // Client connection management
#define SwtClientPool   112   // Client connection pool
#define SwtClientSend   113   // Client request sending
#define SwtClientRecv   114   // Client response receiving
#define SwtClientTls    115   // Client TLS operations
#define SwtClientMulti  116   // Client concurrent requests

#endif  // SWREST_SW_REST_TRACE_LEVELS_H_
