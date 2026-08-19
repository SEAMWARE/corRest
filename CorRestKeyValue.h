//
// FILE            CorRestKeyValue.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef CORREST_KEY_VALUE_H_
#define CORREST_KEY_VALUE_H_

#include <stdint.h>                                       // uint64_t



// -----------------------------------------------------------------------------
//
// CorRestKeyValue - generic key-value pair for headers and URI params
//
// `bit` is meaningful only for URI params: the registry bit for the param name
// (see corRestParamLookup), resolved once at ingestion so downstream code can
// test params by bit rather than by name. It is 0 for headers and unknown params.
//
typedef struct CorRestKeyValue
{
  char*     key;
  char*     value;
  uint64_t  bit;
} CorRestKeyValue;

#endif  // CORREST_KEY_VALUE_H_
