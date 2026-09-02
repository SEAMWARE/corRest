//
// FILE            corRestProblem.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdarg.h>                     // va_list, va_start, va_end
#include <stdio.h>                      // vsnprintf

#include "ktrace/ktOut.h"               // ktOut
#include "corRest/CorRestState.h"         // corRest
#include "corRest/corRestProblem.h"       // Own interface



// -----------------------------------------------------------------------------
//
// corRestProblemFunction - use the corRestProblem macro (captures the call site)
//
void corRestProblemFunction
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
  corRest.out.httpStatusCode  = statusCode;
  corRest.out.problemType     = type;
  corRest.out.problemTitle    = title;

  va_list ap;

  va_start(ap, fmt);
  vsnprintf(corRest.out.problemDetail, sizeof(corRest.out.problemDetail), fmt, ap);
  va_end(ap);

  //
  // Log the error at the caller's location (same technique as ldError): the
  // macro forwards __FILE__/__LINE__/__FUNCTION__, handed straight to the trace
  // so it points at the detection site, not corRestProblem.c.
  //
  // ktOut() rather than KT_E(): KT_E captures __FILE__ and __LINE__ at ITS OWN
  // call site, which would point every problem the library reports at this line.
  //
  ktOut(fileName, lineNo, functionName, 'E', -1, "%d %s: %s", statusCode, title, corRest.out.problemDetail);
}
