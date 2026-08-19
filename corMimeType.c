//
// FILE            corMimeType.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Media-type classification for the rest layer: the single CorMimeType enum
// (CorRestIn.h) plus the two parsers that produce it —
//   - corMimeTypeParse : a single request Content-Type value (415 gate),
//   - corAcceptParse    : a q-weighted Accept header (content negotiation).
//
#include <stddef.h>                                      // NULL
#include <stdbool.h>                                     // bool
#include <stdlib.h>                                      // strtod
#include <string.h>                                      // strncasecmp, strlen

#include "corRest/CorRestIn.h"                             // CorMimeType, own interface



// -----------------------------------------------------------------------------
//
// corMimeTypeParse - classify a Content-Type header value into CorMimeType
//
// Done once on reception so the request path compares an enum rather than
// repeatedly strcasecmp-ing the header. A trailing charset/boundary parameter
// (e.g. "; charset=utf-8") is tolerated; anything outside the JSON family (or a
// NULL header) is CorMimeNone.
//
CorMimeType corMimeTypeParse(const char* contentType)
{
  if (contentType == NULL)
    return CorMimeNone;

  // longest first so "ld+json"/"geo+json" win over a "json" prefix test
  if (strncasecmp(contentType, "application/merge-patch+json", 28) == 0 &&
      (contentType[28] == 0 || contentType[28] == ';' || contentType[28] == ' '))
    return CorMimeMergePatchJson;
  if (strncasecmp(contentType, "application/geo+json", 20) == 0 &&
      (contentType[20] == 0 || contentType[20] == ';' || contentType[20] == ' '))
    return CorMimeGeoJson;
  if (strncasecmp(contentType, "application/ld+json", 19) == 0 &&
      (contentType[19] == 0 || contentType[19] == ';' || contentType[19] == ' '))
    return CorMimeLdJson;
  if (strncasecmp(contentType, "application/json", 16) == 0 &&
      (contentType[16] == 0 || contentType[16] == ';' || contentType[16] == ' '))
    return CorMimeJson;

  return CorMimeNone;
}



// -----------------------------------------------------------------------------
//
// corMimeString - the canonical media-type string for a CorMimeType
//
// The reverse of corMimeTypeParse/corAcceptParse: the broker carries the media
// type as the enum and turns it into a string only here, at the point a
// response Content-Type is emitted. CorMimeNone falls back to application/json.
//
const char* corMimeString(CorMimeType mime)
{
  switch (mime)
  {
  case CorMimeLdJson:          return "application/ld+json";
  case CorMimeGeoJson:         return "application/geo+json";
  case CorMimeMergePatchJson:  return "application/merge-patch+json";
  case CorMimeJson:
  case CorMimeNone:
  default:                    return "application/json";
  }
}



// -----------------------------------------------------------------------------
//
// trimSpaces - advance past leading whitespace
//
static const char* trimSpaces(const char* s)
{
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
}



// -----------------------------------------------------------------------------
//
// nameMatches - case-insensitive prefix-equality of mediaType against full,
//               with end-of-token guard. tokenEnd points one past the last
//               char of mediaType in the source string.
//
static bool nameMatches(const char* tokenStart, const char* tokenEnd, const char* full)
{
  int mLen = (int)(tokenEnd - tokenStart);
  int fLen = (int) strlen(full);
  if (mLen != fLen) return false;
  return strncasecmp(tokenStart, full, mLen) == 0;
}



// -----------------------------------------------------------------------------
//
// extractQ - parse the q= parameter (if any) of a single Accept entry.
//
// `params` points just past the media type, at the first ';' or end. Default
// q = 1.0 when no q= is present. Out-of-range q values are clamped to [0,1].
//
static double extractQ(const char* params, const char* entryEnd)
{
  const char* p = params;
  while (p < entryEnd)
  {
    while (p < entryEnd && (*p == ' ' || *p == '\t' || *p == ';')) p++;

    if (p + 2 < entryEnd && (p[0] == 'q' || p[0] == 'Q') && p[1] == '=')
    {
      char buf[16];
      int  bn = 0;
      const char* v = p + 2;
      while (v < entryEnd && bn < (int)(sizeof(buf) - 1) && *v != ';' && *v != ' ' && *v != '\t')
        buf[bn++] = *v++;
      buf[bn] = 0;
      double q = strtod(buf, NULL);
      if (q < 0) q = 0;
      if (q > 1) q = 1;
      return q;
    }

    // Skip until next ';' or end
    while (p < entryEnd && *p != ';') p++;
  }

  return 1.0;
}



// -----------------------------------------------------------------------------
//
// corAcceptParse - q-weighted Accept-header parsing for the three NGSI-LD media
// types. Highest q wins; equal q → first-listed wins; q=0 excludes a type.
// "*/*" and "application/*" count as q for json (the default representation).
//
//   - NULL or empty Accept           → CorMimeJson (§ 6.3.4: default)
//   - one of the three is acceptable → corresponding type, highest q wins
//   - none of the three is acceptable → CorMimeNone  (§ 6.3.4: 406)
//
// geo+json by itself counts as "acceptable" here; the route layer enforces the
// "geo+json-only is 406 outside Retrieve/Query Entity" rule.
//
CorMimeType corAcceptParse(const char* acceptHeader)
{
  if (acceptHeader == NULL || *acceptHeader == 0)
    return CorMimeJson;

  // Track best-q-so-far per type. -1.0 means "not seen / excluded".
  double qJson    = -1.0;
  double qLdJson  = -1.0;
  double qGeoJson = -1.0;

  // Order-of-first-appearance per type, for tie-breaking on equal q.
  int orderJson    = -1;
  int orderLdJson  = -1;
  int orderGeoJson = -1;

  const char* p     = acceptHeader;
  int         entry = 0;

  while (*p != 0)
  {
    p = trimSpaces(p);
    if (*p == 0) break;

    // Locate end of this entry (next comma or end-of-string).
    const char* entryEnd = p;
    while (*entryEnd != 0 && *entryEnd != ',') entryEnd++;

    // Locate end of media-type name (within the entry: up to ';' or entryEnd).
    const char* nameEnd = p;
    while (nameEnd < entryEnd && *nameEnd != ';' && *nameEnd != ' ' && *nameEnd != '\t') nameEnd++;

    double q = extractQ(nameEnd, entryEnd);

    CorMimeType t  = CorMimeNone;
    bool       wildcardJson = false;

    if      (nameMatches(p, nameEnd, "application/geo+json")) t = CorMimeGeoJson;
    else if (nameMatches(p, nameEnd, "application/ld+json"))  t = CorMimeLdJson;
    else if (nameMatches(p, nameEnd, "application/json"))     t = CorMimeJson;
    else if (nameMatches(p, nameEnd, "application/*") ||
             nameMatches(p, nameEnd, "*/*"))                  wildcardJson = true;

    if (t == CorMimeGeoJson)
    {
      if (q > qGeoJson) { qGeoJson = q; orderGeoJson = entry; }
    }
    else if (t == CorMimeLdJson)
    {
      if (q > qLdJson)  { qLdJson  = q; orderLdJson  = entry; }
    }
    else if (t == CorMimeJson || wildcardJson)
    {
      if (q > qJson)    { qJson    = q; orderJson    = entry; }
    }

    p = entryEnd;
    if (*p == ',') p++;
    entry++;
  }

  // q == 0 is "not acceptable" — exclude from candidates.
  if (qJson    == 0) qJson    = -1.0;
  if (qLdJson  == 0) qLdJson  = -1.0;
  if (qGeoJson == 0) qGeoJson = -1.0;

  // No acceptable type — § 6.3.4 mandates HTTP 406. The route layer
  // makes that decision; we just signal "none acceptable".
  if (qJson < 0 && qLdJson < 0 && qGeoJson < 0)
    return CorMimeNone;

  // Pick the highest q. Equal q → smallest first-appearance order wins.
  // Initialise with json as the spec-default to bias ties towards it
  // when nothing was sent (which extractQ would have given q=1.0).
  CorMimeType best  = CorMimeJson;
  double     bestQ = qJson;
  int        bestO = orderJson;

  if (qLdJson > bestQ || (qLdJson == bestQ && qLdJson >= 0 && (bestO < 0 || orderLdJson < bestO)))
  {
    best = CorMimeLdJson; bestQ = qLdJson; bestO = orderLdJson;
  }
  if (qGeoJson > bestQ || (qGeoJson == bestQ && qGeoJson >= 0 && (bestO < 0 || orderGeoJson < bestO)))
  {
    best = CorMimeGeoJson;
  }

  return best;
}
