//
// FILE            corRestParamRegistry.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                          // calloc
#include <string.h>                          // strcmp

#include "corRest/corRestParamRegistry.h"      // Own interface



// -----------------------------------------------------------------------------
//
// Simple hash table for parameter name -> CorRestParam* lookup
//
#define COR_REST_PARAM_HASH_SLOTS 64

typedef struct CorRestParamSlot
{
  const CorRestParam*       paramP;
  struct CorRestParamSlot*  next;
} CorRestParamSlot;

static CorRestParamSlot*  paramHashTable[COR_REST_PARAM_HASH_SLOTS];
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

  return hash % COR_REST_PARAM_HASH_SLOTS;
}



// -----------------------------------------------------------------------------
//
// corRestParamAdd -
//
bool corRestParamAdd(const CorRestParam* paramV)
{
  if (paramV == NULL)
    return true;

  if (!paramHashReady)
  {
    memset(paramHashTable, 0, sizeof(paramHashTable));
    paramHashReady = true;
  }

  for (const CorRestParam* p = paramV; p->name != NULL; p++)
  {
    unsigned int slot = paramHash(p->name);

    CorRestParamSlot* entry = (CorRestParamSlot*) calloc(1, sizeof(CorRestParamSlot));
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
// corRestParamInit -
//
bool corRestParamInit(const CorRestParam* paramV)
{
  return corRestParamAdd(paramV);
}



// -----------------------------------------------------------------------------
//
// corRestParamLookup -
//
uint64_t corRestParamLookup(const char* name)
{
  if (!paramHashReady)
    return 0;

  unsigned int slot = paramHash(name);

  for (CorRestParamSlot* s = paramHashTable[slot]; s != NULL; s = s->next)
  {
    if (strcmp(name, s->paramP->name) == 0)
      return s->paramP->bit;
  }

  return 0;
}
