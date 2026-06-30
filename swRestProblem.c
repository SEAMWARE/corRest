//
// FILE            swRestProblem.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdarg.h>                     // va_list, va_start, va_end
#include <stdio.h>                      // vsnprintf

#include "kbase/kLibLog.h"              // kLogFunction
#include "swRest/SwRestState.h"         // swRest
#include "swRest/swRestProblem.h"       // Own interface



// -----------------------------------------------------------------------------
//
// swRestProblemFunction - use the swRestProblem macro (captures the call site)
//
void swRestProblemFunction
(
  int          statusCode,
  const char*  type,
  const char*  title,
  const char*  fileName,
  int          lineNo,
  const char*  functionName,
  const char*  fmt,
  ...
)
{
  swRest.out.httpStatusCode  = statusCode;
  swRest.out.problemType     = type;
  swRest.out.problemTitle    = title;

  va_list ap;

  va_start(ap, fmt);
  vsnprintf(swRest.out.problemDetail, sizeof(swRest.out.problemDetail), fmt, ap);
  va_end(ap);

  //
  // Log the error at the caller's location (same technique as ldError): the
  // macro forwards __FILE__/__LINE__/__FUNCTION__, handed straight to
  // kLogFunction so the trace points at the detection site, not swRestProblem.c.
  //
  if (kLogFunction != NULL)
    kLogFunction(1, 0, fileName, lineNo, functionName, "%d %s: %s", statusCode, title, swRest.out.problemDetail);
}
