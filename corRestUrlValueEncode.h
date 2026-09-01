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



// -----------------------------------------------------------------------------
//
// corRestUrlValueDecode - decode %XX sequences, IN PLACE
//
// The other direction, and the one corRest already does on every incoming
// in-process request. Kept beside the encoder so the two spellings of the same
// rule cannot drift apart. Decoding only ever shortens, hence in place; a '%'
// that does not start two hex digits is left exactly as it is.
//
extern void corRestUrlValueDecode(char* s);

#endif  // CORREST_URL_VALUE_ENCODE_H_
