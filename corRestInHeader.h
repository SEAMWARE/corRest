#ifndef CORREST_IN_HEADER_H_
#define CORREST_IN_HEADER_H_

//
// FILE            corRestInHeader.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// corRestInHeader - the value of a request header of the bound request, its name compared without
// case (RFC 9110 § 5.1); NULL: not in the request. The first one, when the request has several.
//
extern const char* corRestInHeader(const char* name);

#endif  // CORREST_IN_HEADER_H_
