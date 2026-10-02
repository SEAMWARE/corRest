#ifndef CORREST_CORRESTRESOLVE_H_
#define CORREST_CORRESTRESOLVE_H_

//
// FILE            corRestResolve.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corRestResolve - getaddrinfo, without stopping an event loop (coraine doc/coroutines.md § 4)
//
// getaddrinfo blocks, and has no socket to wait on. Outside a coroutine - and for an address or
// "localhost", which need no name server - it is plain getaddrinfo. Inside a coroutine, a name is
// resolved on a thread of its own while the coroutine waits for it (corRestWaitFd on an eventfd): the
// loop goes on serving everything else. Free the result with freeaddrinfo, as getaddrinfo's.
//
#include <netdb.h>                                  // struct addrinfo

extern int corRestResolve(const char* host, const char* port, const struct addrinfo* hintsP, struct addrinfo** resPP);

#endif  // CORREST_CORRESTRESOLVE_H_
