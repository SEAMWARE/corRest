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



// -----------------------------------------------------------------------------
//
// swRestProblem - set problem details in swRest.out
//
extern void swRestProblem(int statusCode, const char* type, const char* title, const char* fmt, ...);

#endif  // SWREST_SW_REST_PROBLEM_H_
