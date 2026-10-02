//
// FILE            corRestWait.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <errno.h>                                  // errno, EINTR
#include <stdbool.h>                                // true
#include <poll.h>                                   // poll
#include <stddef.h>                                 // NULL
#include <time.h>                                   // clock_gettime

#include "corBase/corCo.h"                          // corCoCurrent
#include "corBase/corCoLoop.h"                      // corCoLoopInit, corCoLoopWait, corCoLoopResumeHookSet
#include "corRest/corRest.h"                        // corRestP

#include "corRest/corRestWait.h"                    // Own interface



// -----------------------------------------------------------------------------
//
// The scheduler's wait - set once at start-up, before any coroutine runs
//
static CorRestCoWaitFunction coWaitF = NULL;

void corRestCoWaitSet(CorRestCoWaitFunction fn)
{
  coWaitF = fn;
}



// -----------------------------------------------------------------------------
//
// msNow - CLOCK_MONOTONIC in milliseconds
//
static long long msNow(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// corRestWaitFd -
//
int corRestWaitFd(int fd, short events, int timeoutMs, short* reventsP)
{
  if ((coWaitF != NULL) && (corCoCurrent() != NULL))
    return coWaitF(fd, events, timeoutMs, reventsP);

  long long     deadline = (timeoutMs >= 0) ? msNow() + timeoutMs : 0;
  struct pollfd p        = { fd, events, 0 };

  while (true)
  {
    int r = poll(&p, 1, timeoutMs);

    if ((r >= 0) || (errno != EINTR))
    {
      if ((r > 0) && (reventsP != NULL))
        *reventsP = p.revents;
      return r;
    }

    if (timeoutMs >= 0)
    {
      long long left = deadline - msNow();

      if (left <= 0)
        return 0;
      timeoutMs = (int) left;
    }
  }
}



// -----------------------------------------------------------------------------
//
// coWaitBound - corCoLoopWait, with the request bound to the thread kept across the yield
//
// The loop and every other coroutine of the thread rebind corRestP meanwhile - and a request inside an
// in-process forward may have it pointing at the inner request.
//
static int coWaitBound(int fd, short events, int timeoutMs, short* reventsP)
{
  CorRestState* savedP = corRestP;
  int           r      = corCoLoopWait(fd, events, timeoutMs, reventsP);

  corRestP = savedP;
  return r;
}

static void coUnbind(void)
{
  corRestP = NULL;
}



// -----------------------------------------------------------------------------
//
// corRestCoLoopInit -
//
void corRestCoLoopInit(int epollFd)
{
  corCoLoopInit(epollFd);
  corRestCoWaitSet(coWaitBound);
  corCoLoopResumeHookSet(coUnbind);
}
