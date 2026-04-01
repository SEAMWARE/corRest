//
// FILE            swRestClientParse.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// HTTP response parsing for the client.
//

#define _GNU_SOURCE                              // strcasestr
#include <stdlib.h>                              // strtol, malloc, free, atoi
#include <string.h>                              // memchr, memcpy, memmove, strlen, strncasecmp, strcasestr
#include <strings.h>                             // strcasestr
#include <stdbool.h>                             // bool, true, false

#include "swRest/swRestClient.h"                 // SwRestClientConn, SwRestClientResponse
#include "kalloc/KAlloc.h"                       // KAlloc



// -----------------------------------------------------------------------------
//
// findCRLF - Find CRLF or LF in buffer, return pointer past it
//
static inline char* findCRLF(char* buf, int len, int* lineLen)
{
  char* p   = buf;
  char* end = buf + len;

  while (p < end)
  {
    if (*p == '\n')
    {
      if (p > buf && p[-1] == '\r')
        *lineLen = (int)(p - buf - 1);
      else
        *lineLen = (int)(p - buf);
      return p + 1;
    }
    p++;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// scanHeaderValue - extract Content-Length / Transfer-Encoding from a header
//                   line WITHOUT modifying the buffer
//
static void scanHeaderValue(char* line, int lineLen,
                            int* contentLengthP, bool* chunkedP, bool* keepAliveP)
{
  char* colon = (char*) memchr(line, ':', lineLen);
  if (colon == NULL)
    return;

  int nameLen = (int)(colon - line);

  char* value = colon + 1;
  while (value < line + lineLen && (*value == ' ' || *value == '\t'))
    value++;

  int valueLen = lineLen - (int)(value - line);

  if (nameLen == 14 && strncasecmp(line, "Content-Length", 14) == 0)
  {
    *contentLengthP = 0;
    for (int i = 0; i < valueLen && value[i] >= '0' && value[i] <= '9'; i++)
      *contentLengthP = *contentLengthP * 10 + (value[i] - '0');
  }
  else if (nameLen == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
  {
    if (valueLen >= 7)
    {
      for (int i = 0; i <= valueLen - 7; i++)
      {
        if (strncasecmp(value + i, "chunked", 7) == 0)
        {
          *chunkedP = true;
          break;
        }
      }
    }
  }
  else if (nameLen == 10 && strncasecmp(line, "Connection", 10) == 0)
  {
    if (valueLen >= 5 && strncasecmp(value, "close", 5) == 0)
      *keepAliveP = false;
    else if (valueLen >= 10 && strncasecmp(value, "keep-alive", 10) == 0)
      *keepAliveP = true;
  }
}



// -----------------------------------------------------------------------------
//
// chunkedComplete - Read-only check: is the chunked body complete?
//
static int chunkedComplete(char* buf, int len, int* totalBodyLen)
{
  char* src    = buf;
  char* srcEnd = buf + len;
  int   bodyLen = 0;
  int   lineLen;

  while (src < srcEnd)
  {
    char* next = findCRLF(src, (int)(srcEnd - src), &lineLen);
    if (next == NULL)
      return 1;

    long chunkSize = 0;
    for (int i = 0; i < lineLen; i++)
    {
      char c = src[i];
      if (c >= '0' && c <= '9')
        chunkSize = chunkSize * 16 + (c - '0');
      else if (c >= 'a' && c <= 'f')
        chunkSize = chunkSize * 16 + (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        chunkSize = chunkSize * 16 + (c - 'A' + 10);
      else if (c == ';')
        break;
      else
        return -1;
    }

    src = next;

    if (chunkSize == 0)
    {
      *totalBodyLen = bodyLen;
      return 0;
    }

    if (srcEnd - src < chunkSize + 2)
      return 1;

    bodyLen += (int) chunkSize;
    src += chunkSize;

    if (src + 1 < srcEnd && src[0] == '\r' && src[1] == '\n')
      src += 2;
    else if (src < srcEnd && src[0] == '\n')
      src += 1;
    else
      return 1;
  }

  return 1;
}



// -----------------------------------------------------------------------------
//
// swRestClientResponseComplete - Read-only check: is the full response in the buffer?
//
int swRestClientResponseComplete(SwRestClientConn* conn)
{
  char* buf = conn->buf;
  int   len = conn->bufLen;
  char* p   = buf;
  char* end = buf + len;
  int   lineLen;
  char* nextLine;

  nextLine = findCRLF(p, len, &lineLen);
  if (nextLine == NULL)
    return 1;
  p = nextLine;

  int   contentLength = -1;
  bool  chunked       = false;
  bool  keepAlive     = true;

  while (p < end)
  {
    nextLine = findCRLF(p, (int)(end - p), &lineLen);
    if (nextLine == NULL)
      return 1;

    if (lineLen == 0)
    {
      p = nextLine;
      break;
    }

    scanHeaderValue(p, lineLen, &contentLength, &chunked, &keepAlive);
    p = nextLine;
  }

  int available = (int)(end - p);

  if (chunked)
  {
    int dechunkedLen = 0;
    return chunkedComplete(p, available, &dechunkedLen);
  }
  else if (contentLength >= 0)
  {
    return (available >= contentLength) ? 0 : 1;
  }
  else
  {
    char* statusP = buf;
    while (statusP < p && *statusP != ' ')
      statusP++;
    if (statusP < p)
    {
      statusP++;
      int code = 0;
      if (end - statusP >= 3)
        code = (statusP[0] - '0') * 100 + (statusP[1] - '0') * 10 + (statusP[2] - '0');

      if (code == 204 || code == 304 || (code >= 100 && code < 200))
        return 0;
    }

    if (!keepAlive)
      return 1;

    return 0;
  }
}



// -----------------------------------------------------------------------------
//
// parseStatusLine - Parse "HTTP/1.x STATUS TEXT\r\n"
//
static int parseStatusLine(char* line, int lineLen, SwRestClientResponse* resp)
{
  char* p   = line;
  char* end = line + lineLen;

  while (p < end && *p != ' ')
    p++;
  if (p >= end)
    return -1;
  p++;

  if (end - p < 3)
    return -1;

  resp->statusCode = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
  p += 3;

  if (p < end && *p == ' ')
    p++;

  if (p < end)
  {
    resp->statusText = p;
    line[lineLen] = '\0';
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// addResponseHeader - Add a header to response (dynamic array)
//
static void addResponseHeader(SwRestClientResponse* resp, char* name, char* value)
{
  if (resp->headerCount >= resp->headerSize)
  {
    int newSize = resp->headerSize + SW_REST_KV_GROW_SIZE;
    SwRestKeyValue* newV = (SwRestKeyValue*)malloc(newSize * sizeof(SwRestKeyValue));
    if (newV == NULL)
      return;

    memcpy(newV, resp->headerV, resp->headerCount * sizeof(SwRestKeyValue));
    if (resp->headerV != resp->headers)
      free(resp->headerV);

    resp->headerV    = newV;
    resp->headerSize = newSize;
  }

  resp->headerV[resp->headerCount].key  = name;
  resp->headerV[resp->headerCount].value = value;
  resp->headerCount++;
}



// -----------------------------------------------------------------------------
//
// parseResponseHeader - Parse a single response header line
//
static int parseResponseHeader(char* line, int lineLen, SwRestClientResponse* resp,
                               int* contentLengthP, bool* chunkedP, bool* keepAliveP)
{
  char* colon = (char*)memchr(line, ':', lineLen);
  if (colon == NULL)
    return -1;

  *colon = '\0';
  char* name  = line;
  int nameLen = (int)(colon - line);

  char* value = colon + 1;
  while (*value == ' ' || *value == '\t')
    value++;

  int valueLen = lineLen - (int)(value - line);
  value[valueLen] = '\0';

  addResponseHeader(resp, name, value);

  if (nameLen == 14 && strncasecmp(name, "Content-Length", 14) == 0)
  {
    *contentLengthP = atoi(value);
  }
  else if (nameLen == 17 && strncasecmp(name, "Transfer-Encoding", 17) == 0)
  {
    if (strcasestr(value, "chunked") != NULL)
      *chunkedP = true;
  }
  else if (nameLen == 10 && strncasecmp(name, "Connection", 10) == 0)
  {
    if (strncasecmp(value, "close", 5) == 0)
      *keepAliveP = false;
    else if (strncasecmp(value, "keep-alive", 10) == 0)
      *keepAliveP = true;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// dechunk - Dechunk a chunked transfer-encoded body in place
//
static int dechunk(char* buf, int len, int* outLen)
{
  char* src    = buf;
  char* srcEnd = buf + len;
  char* dst    = buf;
  int   lineLen;

  while (src < srcEnd)
  {
    char* next = findCRLF(src, (int)(srcEnd - src), &lineLen);
    if (next == NULL)
      return 1;

    char* sizeStr = src;
    sizeStr[lineLen] = '\0';
    long chunkSize = strtol(sizeStr, NULL, 16);

    src = next;

    if (chunkSize == 0)
    {
      *outLen = (int)(dst - buf);
      return 0;
    }

    if (srcEnd - src < chunkSize + 2)
      return 1;

    memmove(dst, src, chunkSize);
    dst += chunkSize;
    src += chunkSize;

    if (src[0] == '\r' && src[1] == '\n')
      src += 2;
    else if (src[0] == '\n')
      src += 1;
    else
      return -1;
  }

  return 1;
}



// -----------------------------------------------------------------------------
//
// swRestClientParseResponse - Parse HTTP response from conn->buf (destructive)
//
int swRestClientParseResponse(SwRestClientConn* conn, SwRestClientResponse* resp, KAlloc* allocP)
{
  (void) allocP;

  char* buf = conn->buf;
  int   len = conn->bufLen;
  char* p   = buf;
  char* end = buf + len;
  int   lineLen;
  char* nextLine;

  resp->headerCount = 0;
  resp->headerV     = resp->headers;
  resp->headerSize  = SW_REST_INITIAL_KV_SLOTS;
  resp->statusCode  = 0;
  resp->statusText  = NULL;
  resp->body        = NULL;
  resp->bodyLen     = 0;

  nextLine = findCRLF(p, len, &lineLen);
  if (nextLine == NULL)
    return 1;

  if (parseStatusLine(p, lineLen, resp) != 0)
    return -1;

  p = nextLine;

  int   contentLength = -1;
  bool  chunked       = false;
  bool  keepAlive     = true;

  while (p < end)
  {
    nextLine = findCRLF(p, (int)(end - p), &lineLen);
    if (nextLine == NULL)
      return 1;

    if (lineLen == 0)
    {
      p = nextLine;
      break;
    }

    parseResponseHeader(p, lineLen, resp, &contentLength, &chunked, &keepAlive);
    p = nextLine;
  }

  conn->keepAlive = keepAlive;

  int available  = len - (int)(p - buf);

  if (chunked)
  {
    int dechunkedLen = 0;
    int dr = dechunk(p, available, &dechunkedLen);

    if (dr == 1)
      return 1;
    if (dr < 0)
      return -1;

    resp->body    = p;
    resp->bodyLen = dechunkedLen;
  }
  else if (contentLength >= 0)
  {
    if (available < contentLength)
      return 1;

    resp->body    = p;
    resp->bodyLen = contentLength;
  }
  else if (contentLength == -1)
  {
    if (resp->statusCode == 204 || resp->statusCode == 304 ||
        (resp->statusCode >= 100 && resp->statusCode < 200))
    {
      resp->body    = NULL;
      resp->bodyLen = 0;
    }
    else if (!keepAlive)
    {
      resp->body    = p;
      resp->bodyLen = available;
    }
    else
    {
      resp->body    = NULL;
      resp->bodyLen = 0;
    }
  }

  return 0;
}
