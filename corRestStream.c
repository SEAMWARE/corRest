//
// FILE            corRestStream.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corRestStreamResponse - what both servers share. The stream itself is each server's: corRestStreamWrite
// and corRestStreamEnd are in corRestBackendBuiltin.c and corRestBackendMhd.c.
//
#include <stdbool.h>                      // bool
#include <stddef.h>                       // NULL

#include "corRest/CorRestState.h"         // corRest
#include "corRest/corRestStream.h"        // Own interface



// -----------------------------------------------------------------------------
//
// corRestStreamPossible -
//
bool corRestStreamPossible(void)
{
#if COR_WITH_STREAM
  return (corRest.connection != NULL) && (corRest.finishF == NULL) && (corRest.resumeF == NULL);
#else
  return false;
#endif
}



// -----------------------------------------------------------------------------
//
// corRestStreamResponse -
//
bool corRestStreamResponse(CorRestStreamOpen openFn, void* ctx)
{
#if COR_WITH_STREAM
  if ((openFn == NULL) || (corRestStreamPossible() == false))
    return false;

  corRest.out.streamOpenF = openFn;
  corRest.out.streamCtx   = ctx;

  return true;
#else
  (void) openFn;
  (void) ctx;

  return false;
#endif
}
