//
// FILE            swRestStop.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                     // free
#include <microhttpd.h>

#include "swRest/SwRestVerb.h"          // SwVerbs
#include "swRest/SwRestService.h"       // SwRestServiceVector



// -----------------------------------------------------------------------------
//
// Globals (defined in swRestInit.c)
//
extern SwRestServiceVector   swRestServiceV[];
extern struct MHD_Daemon*    swRestDaemon;
extern void                  swRestWorkerPoolStop(void);



// -----------------------------------------------------------------------------
//
// swRestStop -
//
void swRestStop(void)
{
  // Drain + join the async worker pool BEFORE stopping the daemon: this clears
  // any suspended connections (resuming across MHD_stop_daemon is an API
  // violation) and stops new suspensions.
  swRestWorkerPoolStop();

  if (swRestDaemon != NULL)
  {
    MHD_stop_daemon(swRestDaemon);
    swRestDaemon = NULL;
  }

  for (int verb = 0; verb < SwVerbs; verb++)
  {
    free(swRestServiceV[verb].serviceV);
    swRestServiceV[verb].serviceV = NULL;
    swRestServiceV[verb].services = 0;
  }
}
