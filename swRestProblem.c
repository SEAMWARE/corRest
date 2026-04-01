//
// FILE            swRestProblem.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdarg.h>                     // va_list, va_start, va_end
#include <stdio.h>                      // vsnprintf

#include "swRest/SwRestState.h"         // swRest
#include "swRest/swRestProblem.h"       // Own interface



// -----------------------------------------------------------------------------
//
// swRestProblem -
//
void swRestProblem(int statusCode, const char* type, const char* title, const char* fmt, ...)
{
  swRest.out.httpStatusCode  = statusCode;
  swRest.out.problemType     = type;
  swRest.out.problemTitle    = title;

  va_list ap;

  va_start(ap, fmt);
  vsnprintf(swRest.out.problemDetail, sizeof(swRest.out.problemDetail), fmt, ap);
  va_end(ap);
}
