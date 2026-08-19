//
// FILE            corRestStop.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                     // free
#include <microhttpd.h>

#include "corRest/CorRestVerb.h"          // CorVerbs
#include "corRest/CorRestService.h"       // CorRestServiceVector



// -----------------------------------------------------------------------------
//
// Globals (defined in corRestInit.c)
//
extern CorRestServiceVector   corRestServiceV[];
extern struct MHD_Daemon*    corRestDaemon;
extern void                  corRestWorkerPoolStop(void);



// -----------------------------------------------------------------------------
//
// corRestStop -
//
void corRestStop(void)
{
  // Drain + join the async worker pool BEFORE stopping the daemon: this clears
  // any suspended connections (resuming across MHD_stop_daemon is an API
  // violation) and stops new suspensions.
  corRestWorkerPoolStop();

  if (corRestDaemon != NULL)
  {
    MHD_stop_daemon(corRestDaemon);
    corRestDaemon = NULL;
  }

  for (int verb = 0; verb < CorVerbs; verb++)
  {
    free(corRestServiceV[verb].serviceV);
    corRestServiceV[verb].serviceV = NULL;
    corRestServiceV[verb].services = 0;
  }
}
