//
// FILE            corRestUrlValueEncode.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool
#include <string.h>                                    // strlen

#include "kalloc/kaAlloc.h"                            // kaAlloc

#include "corRest/corRestUrlValueEncode.h"             // Own interface



// -----------------------------------------------------------------------------
//
// mustEncode - is this byte unsafe in a query-string value?
//
// Deliberately a SHORT list. Encoding everything RFC 3986 calls reserved would
// be safe on the wire but would make a forwarded query unreadable and stop it
// looking like what a client sends directly - and the NGSI-LD query language
// puts '=', '"', '<', '>', ';', '|', '(', ')' and ',' in values on purpose
// (`q=temp>20;name=="x"`). Those all survive an ordinary query-string parse:
// a parser splits on '&' and on the FIRST '=' only.
//
// What is left is genuinely ambiguous or illegal:
//
//   space, and every control char   illegal in a request line: the receiver
//                                   reads the rest as the HTTP version
//   '%'                             the escape lead - unencoded, the receiver
//                                   decodes a sequence that was never encoded
//   '&'                             ends the value and starts a new parameter
//   '#'                             a fragment delimiter to anything that
//                                   parses the URL as a whole rather than as a
//                                   request line
//   '+'                             read as a space by every form-style decoder
//                                   (MHD included) - this is the one that does
//                                   not fail loudly: `q=name=="a+b"` matches the
//                                   entity named `a b` and returns 200
//   '?'                             starts the query string
//   0x7f and every byte >= 0x80     not allowed raw in a URI; UTF-8 belongs in
//                                   percent-encoded form
//
static bool mustEncode(unsigned char c)
{
  if (c <= 0x20)  return true;     // control characters and space
  if (c >= 0x7f)  return true;     // DEL, and every non-ASCII byte

  return (c == '%') || (c == '&') || (c == '#') || (c == '+') || (c == '?');
}



// -----------------------------------------------------------------------------
//
// corRestUrlValueEncode -
//
const char* corRestUrlValueEncode(const char* value, KAlloc* kaP)
{
  static const char hex[] = "0123456789ABCDEF";

  if (value == NULL)
    return "";

  int extra = 0;

  for (const unsigned char* p = (const unsigned char*) value; *p != 0; p++)
  {
    if (mustEncode(*p))
      extra += 2;                  // one char becomes three
  }

  if (extra == 0)
    return value;                  // nothing to do - hand back what came in

  char* out = (char*) kaAlloc(kaP, strlen(value) + extra + 1);

  if (out == NULL)
    return value;                  // out of arena: raw is wrong, but silence is worse

  char* w = out;

  for (const unsigned char* p = (const unsigned char*) value; *p != 0; p++)
  {
    if (mustEncode(*p))
    {
      *w++ = '%';
      *w++ = hex[*p >> 4];
      *w++ = hex[*p & 0x0f];
    }
    else
      *w++ = (char) *p;
  }

  *w = 0;

  return out;
}



// -----------------------------------------------------------------------------
//
// hexVal - value of a hex digit, or -1 if it is not one
//
static int hexVal(char c)
{
  if ((c >= '0') && (c <= '9'))  return c - '0';
  if ((c >= 'A') && (c <= 'F'))  return c - 'A' + 10;
  if ((c >= 'a') && (c <= 'f'))  return c - 'a' + 10;

  return -1;
}



// -----------------------------------------------------------------------------
//
// corRestUrlValueDecode -
//
void corRestUrlValueDecode(char* s)
{
  if (s == NULL)
    return;

  char* out = s;

  for (char* p = s; *p != 0; )
  {
    if ((p[0] == '%') && (p[1] != 0) && (p[2] != 0))
    {
      int hi = hexVal(p[1]);
      int lo = hexVal(p[2]);

      if ((hi >= 0) && (lo >= 0))
      {
        *out++ = (char) ((hi << 4) | lo);
        p += 3;
        continue;
      }
    }

    *out++ = *p++;
  }

  *out = 0;
}
