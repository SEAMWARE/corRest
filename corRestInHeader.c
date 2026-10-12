//
// FILE            corRestInHeader.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                      // NULL
#include <strings.h>                                     // strcasecmp

#include "corRest/CorRestState.h"                          // corRest
#include "corRest/corRestInHeader.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// corRestInHeader -
//
const char* corRestInHeader(const char* name)
{
  for (int i = 0; i < corRest.in.httpHeaderCount; i++)
  {
    if (strcasecmp(corRest.in.httpHeaderV[i].key, name) == 0)
      return corRest.in.httpHeaderV[i].value;
  }

  return NULL;
}
