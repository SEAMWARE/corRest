//
// FILE            corRestOutHeader.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // memcpy

#include "kalloc/kaAlloc.h"                              // kaAlloc

#include "corRest/CorRestIn.h"                             // COR_REST_KV_GROW_SIZE
#include "corRest/CorRestState.h"                          // corRest
#include "corRest/corRestOutHeader.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// corRestOutHeaderAdd -
//
bool corRestOutHeaderAdd(const char* key, const char* value)
{
  // Grow the array if the inline (or last-grown) capacity is exhausted.
  // Mirrors the in-side resize in corRestInit.c — same allocator, same
  // growth step.
  if (corRest.out.headerCount >= corRest.out.headerSize)
  {
    int             newSize = corRest.out.headerSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV    = (CorRestKeyValue*) kaAlloc(&corRest.kalloc, newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return false;

    memcpy(newV, corRest.out.headerV, corRest.out.headerCount * sizeof(CorRestKeyValue));
    corRest.out.headerV    = newV;
    corRest.out.headerSize = newSize;
  }

  corRest.out.headerV[corRest.out.headerCount].key   = (char*) key;
  corRest.out.headerV[corRest.out.headerCount].value = (char*) value;
  corRest.out.headerCount++;
  return true;
}
