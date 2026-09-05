//
// FILE            corRestStop.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                     // free

#include "corRest/CorRestVerb.h"          // CorVerbs
#include "corRest/CorRestService.h"       // CorRestServiceVector
#include "corRest/corRestBackend.h"       // corRestBackendStop, corRestWorkerPoolStop



// -----------------------------------------------------------------------------
//
// Globals (defined in corRestInit.c)
//
extern CorRestServiceVector   corRestServiceV[];



// -----------------------------------------------------------------------------
//
// corRestStop -
//
void corRestStop(void)
{
  // Drain + join the async worker pool BEFORE stopping the server: this clears
  // any suspended connections (resuming across MHD_stop_daemon is an API
  // violation, and the built-in server would be tearing down a connection a
  // worker still holds) and stops new suspensions.
  corRestWorkerPoolStop();

  corRestBackendStop();

  for (int verb = 0; verb < CorVerbs; verb++)
  {
    free(corRestServiceV[verb].serviceV);
    corRestServiceV[verb].serviceV = NULL;
    corRestServiceV[verb].services = 0;
  }
}
