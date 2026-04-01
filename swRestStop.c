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



// -----------------------------------------------------------------------------
//
// swRestStop -
//
void swRestStop(void)
{
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
