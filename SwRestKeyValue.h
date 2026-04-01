//
// FILE            SwRestKeyValue.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_KEY_VALUE_H_
#define SWREST_SW_REST_KEY_VALUE_H_



// -----------------------------------------------------------------------------
//
// SwRestKeyValue - generic key-value pair for headers and URI params
//
typedef struct SwRestKeyValue
{
  char*  key;
  char*  value;
} SwRestKeyValue;

#endif  // SWREST_SW_REST_KEY_VALUE_H_
