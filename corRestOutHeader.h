#ifndef CORREST_OUT_HEADER_H_
#define CORREST_OUT_HEADER_H_

//
// FILE            corRestOutHeader.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Append a (key, value) pair to corRest.out.headerV with one call,
// hiding the inline-array vs. heap-grown distinction service routines
// previously hand-rolled. Grows the array on demand; both strings are
// borrowed and must outlive the response render.
//
#include <stdbool.h>                                     // bool



// -----------------------------------------------------------------------------
//
// corRestOutHeaderAdd - append response header
//
// Returns true on success, false if the array could not be grown
// (out of memory). Failure is rare; callers may ignore it.
//
extern bool corRestOutHeaderAdd(const char* key, const char* value);

#endif  // CORREST_OUT_HEADER_H_
