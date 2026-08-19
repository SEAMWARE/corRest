//
// FILE            corRestTest.c
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

#include "corRest/corRest.h"



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
static CorRestParam paramV[] =
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
  corRest.out.responseTree = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, "ping", "pong"));

  return true;
}



// -----------------------------------------------------------------------------
//
// getEntity -
//
static bool getEntity(void)
{
  char* entityId = corRest.in.wildcard[0] ? corRest.in.wildcard[0] : "?";

  corRest.out.responseTree = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, "id", entityId));

  // Show URI params that were passed
  for (int i = 0; i < corRest.in.uriParamCount; i++)
    kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, corRest.in.uriParamV[i].key, corRest.in.uriParamV[i].value));

  return true;
}



// -----------------------------------------------------------------------------
//
// getEntityAttr -
//
static bool getEntityAttr(void)
{
  char* entityId = corRest.in.wildcard[0] ? corRest.in.wildcard[0] : "?";
  char* attrName = corRest.in.wildcard[1] ? corRest.in.wildcard[1] : "?";

  corRest.out.responseTree = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, "entity", entityId));
  kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, "attr", attrName));

  return true;
}



// -----------------------------------------------------------------------------
//
// postEntity -
//
static bool postEntity(void)
{
  corRest.out.httpStatusCode = 201;
  corRest.out.responseTree   = kjObject(corRest.kjsonP, NULL);

  kjChildAdd(corRest.out.responseTree, kjBoolean(corRest.kjsonP, "created", KTRUE));
  kjChildAdd(corRest.out.responseTree, kjInteger(corRest.kjsonP, "payloadSize", corRest.in.payloadSize));

  // Echo back parsed request tree if present
  if (corRest.in.requestTree != NULL)
    kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, "payloadType", kjValueType(corRest.in.requestTree->type)));

  // Add a custom response header
  corRest.out.headerV[corRest.out.headerCount].key   = "Location";
  corRest.out.headerV[corRest.out.headerCount].value  = "/api/v1/entities/urn:new:1";
  corRest.out.headerCount++;

  return true;
}



// -----------------------------------------------------------------------------
//
// getCatchAll - greedy wildcard test
//
static bool getCatchAll(void)
{
  char* path = corRest.in.wildcard[0] ? corRest.in.wildcard[0] : "?";

  corRest.out.responseTree = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(corRest.out.responseTree, kjString(corRest.kjsonP, "caught", path));

  return true;
}



// -----------------------------------------------------------------------------
//
// Service table - single flat array, verb included in each entry
//
static CorRestServiceSimplified serviceV[] =
{
  { CorVerbGet,  "/api/v1/ping",                getPing,       0                                        },
  { CorVerbGet,  "/api/v1/entities/*/attrs/*",  getEntityAttr, 0                                        },
  { CorVerbGet,  "/api/v1/entities/*",          getEntity,     PARAM_TYPE | PARAM_LIMIT | PARAM_OFFSET  },
  { CorVerbGet,  "/catch/**",                   getCatchAll,   0                                        },
  { CorVerbPost, "/api/v1/entities",            postEntity,    0                                        }
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
  corRestParamInit(paramV);

  if (corRestInit(serviceV, services, port, 4) != 0)
  {
    fprintf(stderr, "corRestInit failed\n");
    return 1;
  }

  printf("corRestTest listening on port %d\n", port);
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
  corRestStop();

  return 0;
}
