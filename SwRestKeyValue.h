//
// FILE            SwRestKeyValue.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_KEY_VALUE_H_
#define SWREST_SW_REST_KEY_VALUE_H_

#include <stdint.h>                                       // uint64_t



// -----------------------------------------------------------------------------
//
// SwRestKeyValue - generic key-value pair for headers and URI params
//
// `bit` is meaningful only for URI params: the registry bit for the param name
// (see swRestParamLookup), resolved once at ingestion so downstream code can
// test params by bit rather than by name. It is 0 for headers and unknown params.
//
typedef struct SwRestKeyValue
{
  char*     key;
  char*     value;
  uint64_t  bit;
} SwRestKeyValue;

#endif  // SWREST_SW_REST_KEY_VALUE_H_
