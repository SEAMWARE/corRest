//
// FILE            swRestOutHeader.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // memcpy

#include "kalloc/kaAlloc.h"                              // kaAlloc

#include "swRest/SwRestIn.h"                             // SW_REST_KV_GROW_SIZE
#include "swRest/SwRestState.h"                          // swRest
#include "swRest/swRestOutHeader.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// swRestOutHeaderAdd -
//
bool swRestOutHeaderAdd(const char* key, const char* value)
{
  // Grow the array if the inline (or last-grown) capacity is exhausted.
  // Mirrors the in-side resize in swRestInit.c — same allocator, same
  // growth step.
  if (swRest.out.headerCount >= swRest.out.headerSize)
  {
    int             newSize = swRest.out.headerSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV    = (SwRestKeyValue*) kaAlloc(&swRest.kalloc, newSize * sizeof(SwRestKeyValue));

    if (newV == NULL)
      return false;

    memcpy(newV, swRest.out.headerV, swRest.out.headerCount * sizeof(SwRestKeyValue));
    swRest.out.headerV    = newV;
    swRest.out.headerSize = newSize;
  }

  swRest.out.headerV[swRest.out.headerCount].key   = (char*) key;
  swRest.out.headerV[swRest.out.headerCount].value = (char*) value;
  swRest.out.headerCount++;
  return true;
}
