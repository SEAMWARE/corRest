//
// FILE            SwRestService.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWREST_SW_REST_SERVICE_H_
#define SWREST_SW_REST_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "swRest/SwRestVerb.h"



// -----------------------------------------------------------------------------
//
// SwRestServiceRoutine - function pointer for service routines
//
// No parameter - the service routine reads/writes the __thread SwRestState swRest.
//
typedef bool (*SwRestServiceRoutine)(void);



// -----------------------------------------------------------------------------
//
// SwRestParam - URL parameter definition for hash-table registration
//
// Each entry maps a parameter name to a bit in a uint64_t bitmask.
// The registry array is terminated by { NULL, 0 }.
//
typedef struct SwRestParam
{
  const char*  name;
  uint64_t     bit;
} SwRestParam;



// -----------------------------------------------------------------------------
//
// SwRestServiceOptions - per-service option bits, in two named groups.
//
//   wildcards - URL wildcard-slot validation, COMPUTED at init time from the
//               URL pattern by the service-init hook (ldUrlWildcardOptionsInit).
//               Not set by the service author.
//   features  - service features DECLARED by the service author in the service
//               table (SwRestServiceSimplified.options). Copied verbatim into
//               the runtime service at init; the wildcard bits are added on top.
//
// A bit-struct (rather than a raw mask + #defines) so each flag reads by name.
//
typedef struct SwRestServiceOptions
{
  struct
  {
    uint8_t  uriAt0   : 1;   // wildcard[0] must be a valid URI (entity/sub/reg/map id)
    uint8_t  nameAt1  : 1;   // wildcard[1] must be a valid attribute name (§ 4.6.2)
    uint8_t  uriAt2   : 1;   // wildcard[2] must be a valid instance URI
  } wildcards;

  struct
  {
    uint8_t  entityArrayBody : 1;  // body is an array of Entity objects → § 6.2.4
                                   // @context-member rule is checked per element
  } features;
} SwRestServiceOptions;



// -----------------------------------------------------------------------------
//
// SwRestServiceSimplified -
//
// This is what the caller provides: a flat array of services, each with its verb.
// The init function splits them by verb and expands into full SwRestService structs.
//
// GET services automatically get HEAD support (same handler, body suppressed).
// OPTIONS is auto-generated for all registered URL paths.
//
typedef struct SwRestServiceSimplified
{
  SwRestVerb               verb;
  const char*              url;
  SwRestServiceRoutine     serviceRoutine;
  uint64_t                 supportedParams;
  uint64_t                 ldOp;            // LdOp bit for the service's atomic op — 0 for non-NGSI-LD routes
  SwRestServiceRoutine     payloadCheck;    // optional body validator, run before serviceRoutine — NULL for none
  SwRestServiceOptions     options;         // author-declared service features (wildcard bits added at init)
} SwRestServiceSimplified;



// -----------------------------------------------------------------------------
//
// SwRestService -
//
// Expanded service descriptor, prepared at init time for fast lookup.
// URL pattern matching uses character-sum hashing to avoid strcmp in most cases.
//
typedef struct SwRestService
{
  char*                  url;                          // URL Path (points into SwRestServiceSimplified)
  SwRestServiceRoutine   serviceRoutine;               // Function pointer
  SwRestServiceRoutine   payloadCheck;                 // optional body validator, run before serviceRoutine (NULL = none)
  uint64_t               supportedParams;              // bitmask of accepted URL parameters
  uint64_t               ldOp;                         // LdOp bit (copied from SwRestServiceSimplified at init)
  SwRestServiceOptions   options;                      // author features + init-hook wildcard bits (see SwRestServiceOptions)
  int                    wildcards;                    // 0, 1, or 2+
  bool                   greedy;                       // true if last wildcard is ** (matches multiple components)
  int                    charsBeforeFirstWildcard;     // length of fixed prefix before first '*'
  int                    charsBeforeFirstWildcardSum;  // sum of chars in that prefix (for fast reject)
  int                    charsBeforeSecondWildcard;    // length of text between first and second '*'
  int                    charsBeforeSecondWildcardSum; // sum of those chars
  char                   matchForSecondWildcard[16];   // pattern between first and second '*'
  int                    matchForSecondWildcardLen;    // length of that pattern
  char                   matchForThirdWildcard[16];    // pattern between second and third '*'
  int                    matchForThirdWildcardLen;     // length of that pattern (0 if no third wildcard)
  char                   matchAfterLastWildcard[16];   // literal suffix after the LAST '*' (e.g. "/value"); "" if none
  int                    matchAfterLastWildcardLen;    // length of that suffix (0 if the last wildcard runs to end)
} SwRestService;



// -----------------------------------------------------------------------------
//
// SwRestServiceVector - expanded, one per HTTP method
//
typedef struct SwRestServiceVector
{
  SwRestService*  serviceV;
  int             services;
} SwRestServiceVector;

#endif  // SWREST_SW_REST_SERVICE_H_
