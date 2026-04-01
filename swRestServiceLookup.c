//
// FILE            swRestServiceLookup.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                          // strcmp, strncmp, strstr, strchr

#include "swRest/SwRestState.h"              // swRest
#include "swRest/SwRestService.h"            // SwRestService, SwRestServiceVector
#include "swRest/swRestServiceLookup.h"      // Own interface



// -----------------------------------------------------------------------------
//
// requestPrepare - compute character sums for fast URL matching
//
#define SW_REST_MAX_CHARS_BEFORE_WILDCARD 64

static void requestPrepare(const char* url, int* cSumV, int* cSumsP, int* sLenP)
{
  *cSumsP = 0;

  if (url[0] == 0)
  {
    *sLenP = 0;
    return;
  }

  cSumV[0] = url[0];

  int ix = 1;

  while ((url[ix] != 0) && (ix < SW_REST_MAX_CHARS_BEFORE_WILDCARD))
  {
    cSumV[ix] = cSumV[ix - 1] + url[ix];
    ++ix;
  }
  *cSumsP = ix;

  while (url[ix] != 0)
    ++ix;

  *sLenP = ix;
}



// -----------------------------------------------------------------------------
//
// swRestServiceLookup -
//
SwRestService* swRestServiceLookup(SwRestServiceVector* serviceV)
{
  int cSumV[SW_REST_MAX_CHARS_BEFORE_WILDCARD];
  int cSums;
  int sLen;

  requestPrepare(swRest.in.urlPath, cSumV, &cSums, &sLen);

  for (int ix = 0; ix < serviceV->services; ix++)
  {
    SwRestService* serviceP = &serviceV->serviceV[ix];

    if (serviceP->wildcards == 0)
    {
      // Exact match: compare length, char-sum, then strcmp
      if (serviceP->charsBeforeFirstWildcard == sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[sLen - 1])
        {
          if (strcmp(serviceP->url, swRest.in.urlPath) == 0)
            return serviceP;
        }
      }
    }
    else if (serviceP->wildcards == 1)
    {
      if (serviceP->charsBeforeFirstWildcard < sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[serviceP->charsBeforeFirstWildcard - 1])
        {
          if (strncmp(serviceP->url, swRest.in.urlPath, serviceP->charsBeforeFirstWildcard) == 0)
          {
            if (serviceP->matchForSecondWildcardLen != 0)
            {
              // Wildcard in the middle: /prefix/*/suffix
              int endIndex = sLen - serviceP->matchForSecondWildcardLen;

              if (endIndex > serviceP->charsBeforeFirstWildcard &&
                  strncmp(&swRest.in.urlPath[endIndex], serviceP->matchForSecondWildcard, serviceP->matchForSecondWildcardLen) == 0)
              {
                char* wildcardValue = &swRest.in.urlPath[serviceP->charsBeforeFirstWildcard];

                // For non-greedy *, wildcard must not contain '/' (single component only)
                if (!serviceP->greedy)
                {
                  // Check that the wildcard portion has no '/' in it
                  int wildcardLen = endIndex - serviceP->charsBeforeFirstWildcard;
                  bool hasSlash = false;
                  for (int j = 0; j < wildcardLen; j++)
                  {
                    if (wildcardValue[j] == '/')
                    {
                      hasSlash = true;
                      break;
                    }
                  }
                  if (hasSlash)
                    continue;  // Not a match for single-component wildcard
                }

                swRest.in.wildcard[0] = wildcardValue;
                swRest.in.urlPath[endIndex] = 0;
                return serviceP;
              }
            }
            else
            {
              // Wildcard at the end: /prefix/*
              char* wildcardValue = &swRest.in.urlPath[serviceP->charsBeforeFirstWildcard];

              // For non-greedy *, wildcard must not contain '/'
              if (!serviceP->greedy && strchr(wildcardValue, '/') != NULL)
                continue;

              swRest.in.wildcard[0] = wildcardValue;
              return serviceP;
            }
          }
        }
      }
    }
    else  // 2+ wildcards
    {
      if (serviceP->charsBeforeFirstWildcard < sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[serviceP->charsBeforeFirstWildcard - 1])
        {
          char* matchP;

          if ((matchP = strstr(&swRest.in.urlPath[serviceP->charsBeforeFirstWildcard], serviceP->matchForSecondWildcard)) != NULL)
          {
            char* wc0 = &swRest.in.urlPath[serviceP->charsBeforeFirstWildcard];
            char* wc1 = &matchP[serviceP->matchForSecondWildcardLen];

            // For non-greedy, first wildcard must be single component (no '/')
            if (!serviceP->greedy)
            {
              bool hasSlash = false;
              for (char* p = wc0; p < matchP; p++)
              {
                if (*p == '/')
                {
                  hasSlash = true;
                  break;
                }
              }
              if (hasSlash)
                continue;
            }

            swRest.in.wildcard[0] = wc0;
            swRest.in.wildcard[1] = wc1;
            *matchP = 0;

            return serviceP;
          }
        }
      }
    }
  }

  return NULL;
}
