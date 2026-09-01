//
// FILE            corRestUrlValueEncode.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Percent-encode a string for use as a query-string VALUE.
//
#ifndef CORREST_URL_VALUE_ENCODE_H_
#define CORREST_URL_VALUE_ENCODE_H_

#include "kalloc/kaAlloc.h"                            // KAlloc



// -----------------------------------------------------------------------------
//
// corRestUrlValueEncode - percent-encode a string for a query-string value
//
// The counterpart of the decoding corRest already does on the way in. A value
// that arrived percent-decoded and is then re-emitted into a URL - a forwarded
// query, a callback URL - has to be encoded again, or the receiver reads
// something else than was meant.
//
// Returns `value` itself when nothing needs encoding (the common case, no
// allocation); otherwise a fresh string from kaP. NULL in gives "" out.
//
extern const char* corRestUrlValueEncode(const char* value, KAlloc* kaP);

#endif  // CORREST_URL_VALUE_ENCODE_H_
