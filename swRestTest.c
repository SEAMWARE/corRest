//
// FILE            swRestTest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Minimal test: starts a REST server on port 8080, serves a few endpoints.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "kalloc/kaAlloc.h"
#include "kjson/kjBuilder.h"
#include "kjson/KjNode.h"

#include "swRest/swRest.h"



// -----------------------------------------------------------------------------
//
// URL parameter bits
//
#define PARAM_TYPE    (1ULL << 0)
#define PARAM_LIMIT   (1ULL << 1)
#define PARAM_OFFSET  (1ULL << 2)



// -----------------------------------------------------------------------------
//
// URL parameter registry
//
static SwRestParam paramV[] =
{
  { "type",   PARAM_TYPE   },
  { "limit",  PARAM_LIMIT  },
  { "offset", PARAM_OFFSET },
  { NULL,     0            }
};



// -----------------------------------------------------------------------------
//
// getPing -
//
static bool getPing(void)
{
  swRest.out.responseTree = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "ping", "pong"));

  return true;
}



// -----------------------------------------------------------------------------
//
// getEntity -
//
static bool getEntity(void)
{
  char* entityId = swRest.in.wildcard[0] ? swRest.in.wildcard[0] : "?";

  swRest.out.responseTree = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "id", entityId));

  // Show URI params that were passed
  for (int i = 0; i < swRest.in.uriParamCount; i++)
    kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, swRest.in.uriParamV[i].key, swRest.in.uriParamV[i].value));

  return true;
}



// -----------------------------------------------------------------------------
//
// getEntityAttr -
//
static bool getEntityAttr(void)
{
  char* entityId = swRest.in.wildcard[0] ? swRest.in.wildcard[0] : "?";
  char* attrName = swRest.in.wildcard[1] ? swRest.in.wildcard[1] : "?";

  swRest.out.responseTree = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "entity", entityId));
  kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "attr", attrName));

  return true;
}



// -----------------------------------------------------------------------------
//
// postEntity -
//
static bool postEntity(void)
{
  swRest.out.httpStatusCode = 201;
  swRest.out.responseTree   = kjObject(swRest.kjsonP, NULL);

  kjChildAdd(swRest.out.responseTree, kjBoolean(swRest.kjsonP, "created", KTRUE));
  kjChildAdd(swRest.out.responseTree, kjInteger(swRest.kjsonP, "payloadSize", swRest.in.payloadSize));

  // Echo back parsed request tree if present
  if (swRest.in.requestTree != NULL)
    kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "payloadType", kjValueType(swRest.in.requestTree->type)));

  // Add a custom response header
  swRest.out.headerV[swRest.out.headerCount].key   = "Location";
  swRest.out.headerV[swRest.out.headerCount].value  = "/api/v1/entities/urn:new:1";
  swRest.out.headerCount++;

  return true;
}



// -----------------------------------------------------------------------------
//
// getCatchAll - greedy wildcard test
//
static bool getCatchAll(void)
{
  char* path = swRest.in.wildcard[0] ? swRest.in.wildcard[0] : "?";

  swRest.out.responseTree = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(swRest.out.responseTree, kjString(swRest.kjsonP, "caught", path));

  return true;
}



// -----------------------------------------------------------------------------
//
// Service table - single flat array, verb included in each entry
//
static SwRestServiceSimplified serviceV[] =
{
  { SwVerbGet,  "/api/v1/ping",                getPing,       0                                        },
  { SwVerbGet,  "/api/v1/entities/*/attrs/*",  getEntityAttr, 0                                        },
  { SwVerbGet,  "/api/v1/entities/*",          getEntity,     PARAM_TYPE | PARAM_LIMIT | PARAM_OFFSET  },
  { SwVerbGet,  "/catch/**",                   getCatchAll,   0                                        },
  { SwVerbPost, "/api/v1/entities",            postEntity,    0                                        }
};
static const int services = sizeof(serviceV) / sizeof(serviceV[0]);



// -----------------------------------------------------------------------------
//
// running -
//
static volatile int running = 1;
static void sigHandler(int sig) { running = 0; }



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argc, char* argv[])
{
  unsigned short port = 1026;

  for (int ix = 1; ix < argc; ix++)
  {
    if (strcmp(argv[ix], "-port") == 0 && ix + 1 < argc)
      port = atoi(argv[++ix]);
  }

  // Register URL parameters
  swRestParamInit(paramV);

  if (swRestInit(serviceV, services, port, 4) != 0)
  {
    fprintf(stderr, "swRestInit failed\n");
    return 1;
  }

  printf("swRestTest listening on port %d\n", port);
  printf("  GET  /api/v1/ping\n");
  printf("  GET  /api/v1/entities/<id>\n");
  printf("  GET  /api/v1/entities/<id>/attrs/<attr>\n");
  printf("  POST /api/v1/entities\n");
  printf("  Supported params: type, limit, offset\n");
  printf("  Use ?pretty=2 for indented JSON\n");
  printf("Ctrl-C to stop\n");

  signal(SIGINT, sigHandler);
  signal(SIGTERM, sigHandler);

  while (running)
    sleep(1);

  printf("\nStopping...\n");
  swRestStop();

  return 0;
}
