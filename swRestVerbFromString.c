//
// FILE            swRestVerbFromString.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>

#include "swRest/SwRestVerb.h"



// -----------------------------------------------------------------------------
//
// swRestVerbFromString -
//
SwRestVerb swRestVerbFromString(const char* method)
{
  if      (strcmp(method, "GET")     == 0)  return SwVerbGet;
  else if (strcmp(method, "PUT")     == 0)  return SwVerbPut;
  else if (strcmp(method, "POST")    == 0)  return SwVerbPost;
  else if (strcmp(method, "DELETE")  == 0)  return SwVerbDelete;
  else if (strcmp(method, "PATCH")   == 0)  return SwVerbPatch;
  else if (strcmp(method, "HEAD")    == 0)  return SwVerbHead;
  else if (strcmp(method, "OPTIONS") == 0)  return SwVerbOptions;

  return SwVerbs;  // invalid
}



// -----------------------------------------------------------------------------
//
// swRestVerbToString -
//
const char* swRestVerbToString(SwRestVerb verb)
{
  switch (verb)
  {
  case SwVerbGet:      return "GET";
  case SwVerbPut:      return "PUT";
  case SwVerbPost:     return "POST";
  case SwVerbDelete:   return "DELETE";
  case SwVerbPatch:    return "PATCH";
  case SwVerbHead:     return "HEAD";
  case SwVerbOptions:  return "OPTIONS";
  default:             return "UNKNOWN";
  }
}
