#ifndef CORREST_CORRESTWAIT_H_
#define CORREST_CORRESTWAIT_H_

//
// FILE            corRestWait.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corRestWaitFd - the one place a client waits for a socket (coraine doc/coroutines.md § 2)
//
// Every wait of corRest's clients - the HTTP client, the multi client, the cor:// client - is this
// call. Outside a coroutine it is poll() on one fd, as the clients did themselves. Inside one, it is
// the scheduler's: the fd goes to the event loop's epoll and the coroutine yields, so the loop serves
// everything else meanwhile - the hook the scheduler installs with corRestCoWaitSet.
//
// Never call it with a lock held: inside a coroutine it yields, and another coroutine of the same
// thread that wants that lock would wait for ever.
//



// -----------------------------------------------------------------------------
//
// corRestWaitFd - until fd is ready for events (POLLIN, POLLOUT) or timeoutMs passes (< 0: no limit)
//
// Returns as poll() on one fd does: 1 ready (*reventsP, if not NULL, gets what it is ready for), 0 the
// time ran out, -1 an error (errno). Interrupted by a signal it waits again, for what is left.
//
extern int corRestWaitFd(int fd, short events, int timeoutMs, short* reventsP);



// -----------------------------------------------------------------------------
//
// CorRestCoWaitFunction / corRestCoWaitSet - the scheduler's wait, used inside a coroutine
//
// Same contract as corRestWaitFd. NULL (the default): no scheduler - a coroutine's wait is poll() too.
//
typedef int (*CorRestCoWaitFunction)(int fd, short events, int timeoutMs, short* reventsP);

extern void corRestCoWaitSet(CorRestCoWaitFunction fn);



// -----------------------------------------------------------------------------
//
// corRestCoLoopInit - this thread's epoll loop runs coroutines (corBase corCoLoop), bound to corRest
//
// For a server loop of corRest's (cor://, the builtin HTTP server): the loop's epoll watches what its
// coroutines wait for, corRestWaitFd inside them is corCoLoopWait, and corRestP - the request bound to
// the thread - is the coroutine's own across a yield and nobody's on the loop between them.
//
extern void corRestCoLoopInit(int epollFd);

#endif  // CORREST_CORRESTWAIT_H_
