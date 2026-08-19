//
// FILE            corRestProblem.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// RFC 9457 Problem Details for NGSI-LD error responses.
//
#ifndef CORREST_PROBLEM_H_
#define CORREST_PROBLEM_H_



// -----------------------------------------------------------------------------
//
// NGSI-LD error type URIs
//
#define COR_REST_ERROR_BAD_REQUEST    "https://uri.etsi.org/ngsi-ld/errors/BadRequestData"
#define COR_REST_ERROR_NOT_FOUND      "https://uri.etsi.org/ngsi-ld/errors/ResourceNotFound"
#define COR_REST_ERROR_INTERNAL       "https://uri.etsi.org/ngsi-ld/errors/InternalError"
#define COR_REST_ERROR_INVALID_REQ    "https://uri.etsi.org/ngsi-ld/errors/InvalidRequest"
#define COR_REST_ERROR_TOO_COMPLEX    "https://uri.etsi.org/ngsi-ld/errors/TooComplexQuery"
#define COR_REST_ERROR_METHOD         "https://uri.etsi.org/ngsi-ld/errors/MethodNotAllowed"
#define COR_REST_ERROR_ALREADY_EXISTS "https://uri.etsi.org/ngsi-ld/errors/AlreadyExists"
#define COR_REST_ERROR_UNSUPPORTED    "https://uri.etsi.org/ngsi-ld/errors/UnsupportedMediaType"
#define COR_REST_ERROR_REQUEST_LENGTH "https://uri.etsi.org/ngsi-ld/errors/RequestEntityTooLarge"



// -----------------------------------------------------------------------------
//
// corRestProblemFunction - set problem details in corRest.out and log the error
//
// Use the corRestProblem macro instead — it captures the caller's __FILE__,
// __LINE__ and __FUNCTION__ so the logged error line points at the place the
// problem was detected, not corRestProblem.c (same technique as ldError).
//
extern void corRestProblemFunction
(
  int          statusCode,
  const char*  type,
  const char*  title,
  const char*  fileName,
  int          lineNo,
  const char*  functionName,
  const char*  fmt,
  ...
);

#define corRestProblem(statusCode, type, title, ...)  \
  corRestProblemFunction(statusCode, type, title, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#endif  // CORREST_PROBLEM_H_
