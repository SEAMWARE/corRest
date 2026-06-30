//
// FILE            swRestProblem.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// RFC 9457 Problem Details for NGSI-LD error responses.
//
#ifndef SWREST_SW_REST_PROBLEM_H_
#define SWREST_SW_REST_PROBLEM_H_



// -----------------------------------------------------------------------------
//
// NGSI-LD error type URIs
//
#define SW_REST_ERROR_BAD_REQUEST    "https://uri.etsi.org/ngsi-ld/errors/BadRequestData"
#define SW_REST_ERROR_NOT_FOUND      "https://uri.etsi.org/ngsi-ld/errors/ResourceNotFound"
#define SW_REST_ERROR_INTERNAL       "https://uri.etsi.org/ngsi-ld/errors/InternalError"
#define SW_REST_ERROR_INVALID_REQ    "https://uri.etsi.org/ngsi-ld/errors/InvalidRequest"
#define SW_REST_ERROR_TOO_COMPLEX    "https://uri.etsi.org/ngsi-ld/errors/TooComplexQuery"
#define SW_REST_ERROR_METHOD         "https://uri.etsi.org/ngsi-ld/errors/MethodNotAllowed"
#define SW_REST_ERROR_ALREADY_EXISTS "https://uri.etsi.org/ngsi-ld/errors/AlreadyExists"
#define SW_REST_ERROR_UNSUPPORTED    "https://uri.etsi.org/ngsi-ld/errors/UnsupportedMediaType"
#define SW_REST_ERROR_REQUEST_LENGTH "https://uri.etsi.org/ngsi-ld/errors/RequestEntityTooLarge"



// -----------------------------------------------------------------------------
//
// swRestProblemFunction - set problem details in swRest.out and log the error
//
// Use the swRestProblem macro instead — it captures the caller's __FILE__,
// __LINE__ and __FUNCTION__ so the logged error line points at the place the
// problem was detected, not swRestProblem.c (same technique as ldError).
//
extern void swRestProblemFunction
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

#define swRestProblem(statusCode, type, title, ...)  \
  swRestProblemFunction(statusCode, type, title, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#endif  // SWREST_SW_REST_PROBLEM_H_
