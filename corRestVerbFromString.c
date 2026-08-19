//
// FILE            corRestVerbFromString.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>

#include "corRest/CorRestVerb.h"



// -----------------------------------------------------------------------------
//
// corRestVerbFromString -
//
CorRestVerb corRestVerbFromString(const char* method)
{
  if      (strcmp(method, "GET")     == 0)  return CorVerbGet;
  else if (strcmp(method, "PUT")     == 0)  return CorVerbPut;
  else if (strcmp(method, "POST")    == 0)  return CorVerbPost;
  else if (strcmp(method, "DELETE")  == 0)  return CorVerbDelete;
  else if (strcmp(method, "PATCH")   == 0)  return CorVerbPatch;
  else if (strcmp(method, "HEAD")    == 0)  return CorVerbHead;
  else if (strcmp(method, "OPTIONS") == 0)  return CorVerbOptions;

  return CorVerbs;  // invalid
}



// -----------------------------------------------------------------------------
//
// corRestVerbToString -
//
const char* corRestVerbToString(CorRestVerb verb)
{
  switch (verb)
  {
  case CorVerbGet:      return "GET";
  case CorVerbPut:      return "PUT";
  case CorVerbPost:     return "POST";
  case CorVerbDelete:   return "DELETE";
  case CorVerbPatch:    return "PATCH";
  case CorVerbHead:     return "HEAD";
  case CorVerbOptions:  return "OPTIONS";
  default:             return "UNKNOWN";
  }
}
