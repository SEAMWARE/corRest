//
// FILE            swRestParamRegistry.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                          // calloc
#include <string.h>                          // strcmp

#include "swRest/swRestParamRegistry.h"      // Own interface



// -----------------------------------------------------------------------------
//
// Simple hash table for parameter name -> SwRestParam* lookup
//
#define SW_REST_PARAM_HASH_SLOTS 64

typedef struct SwRestParamSlot
{
  const SwRestParam*       paramP;
  struct SwRestParamSlot*  next;
} SwRestParamSlot;

static SwRestParamSlot*  paramHashTable[SW_REST_PARAM_HASH_SLOTS];
static bool              paramHashReady = false;



// -----------------------------------------------------------------------------
//
// paramHash - djb2 hash
//
static unsigned int paramHash(const char* name)
{
  unsigned int hash = 5381;

  while (*name != '\0')
  {
    hash = ((hash << 5) + hash) + (unsigned char) *name;
    name++;
  }

  return hash % SW_REST_PARAM_HASH_SLOTS;
}



// -----------------------------------------------------------------------------
//
// swRestParamAdd -
//
bool swRestParamAdd(const SwRestParam* paramV)
{
  if (paramV == NULL)
    return true;

  if (!paramHashReady)
  {
    memset(paramHashTable, 0, sizeof(paramHashTable));
    paramHashReady = true;
  }

  for (const SwRestParam* p = paramV; p->name != NULL; p++)
  {
    unsigned int slot = paramHash(p->name);

    SwRestParamSlot* entry = (SwRestParamSlot*) calloc(1, sizeof(SwRestParamSlot));
    if (entry == NULL)
      return false;

    entry->paramP = p;
    entry->next   = paramHashTable[slot];
    paramHashTable[slot] = entry;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// swRestParamInit -
//
bool swRestParamInit(const SwRestParam* paramV)
{
  return swRestParamAdd(paramV);
}



// -----------------------------------------------------------------------------
//
// swRestParamLookup -
//
uint64_t swRestParamLookup(const char* name)
{
  if (!paramHashReady)
    return 0;

  unsigned int slot = paramHash(name);

  for (SwRestParamSlot* s = paramHashTable[slot]; s != NULL; s = s->next)
  {
    if (strcmp(name, s->paramP->name) == 0)
      return s->paramP->bit;
  }

  return 0;
}
