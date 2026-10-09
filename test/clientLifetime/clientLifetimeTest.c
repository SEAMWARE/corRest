//
// FILE            clientLifetimeTest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The lifetime of a client response: what corRestClientSend and the multi engine return must stay
// valid until the caller releases it (corRestClientResponseCleanup / corRestClientMultiDestroy),
// whatever happens to the connection that carried it.
//
// A local server answers every request with a body of its own ("response-<n>"). The cases:
//
//   1. send-close   corRestClientSend, the server answers "Connection: close": the connection is
//                   destroyed before Send returns
//   2. send-reuse   corRestClientSend twice on one keep-alive connection: the second request reuses
//                   the pooled connection of the first; the first response must still read
//                   "response-1"
//   3. multi-close  the multi engine, "Connection: close"
//   4. multi-reuse  the multi engine, then corRestClientSend on the pooled connection
//   5. redirect     corRestClientSend to a 302 whose Location is in a response on a connection that
//                   is closed: the redirect is followed
//
// Cases 1 and 3 are seen by AddressSanitizer (heap-use-after-free) or valgrind; cases 2 and 4 by the
// comparison alone. Exit code 0: all of them pass.
//
#include <stdio.h>                               // printf, fprintf
#include <stdlib.h>                              // exit
#include <string.h>                              // memcmp, strlen, strstr
#include <stdbool.h>                             // bool
#include <unistd.h>                              // read, write, close
#include <pthread.h>                             // pthread_create
#include <sys/socket.h>                          // socket, bind, listen, accept
#include <netinet/in.h>                          // sockaddr_in

#include "corRest/corRestClient.h"               // corRestClient*



static int           serverPort;
static volatile bool serverClose;                // answer "Connection: close" and close
static int           responseNo;
static pthread_mutex_t responseMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// connServe - answer every request on one connection
//
static void* connServe(void* arg)
{
  int  fd = (int) (long) arg;
  char buf[4096];
  int  len = 0;

  while (1)
  {
    int n = read(fd, &buf[len], sizeof(buf) - 1 - len);
    if (n <= 0)
      break;

    len += n;
    buf[len] = 0;

    char* end = strstr(buf, "\r\n\r\n");
    if (end == NULL)
      continue;

    if (strncmp(buf, "GET /redirect ", 14) == 0)
    {
      char response[256];
      int  rLen = snprintf(response, sizeof(response),
                           "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d/x\r\nConnection: %s\r\nContent-Length: 0\r\n\r\n",
                           serverPort, serverClose ? "close" : "keep-alive");

      if (write(fd, response, rLen) != rLen)
        break;

      len = 0;

      if (serverClose)
        break;

      continue;
    }

    pthread_mutex_lock(&responseMutex);
    int no = ++responseNo;
    pthread_mutex_unlock(&responseMutex);

    char body[64];
    char response[256];
    int  bodyLen = snprintf(body, sizeof(body), "response-%d", no);
    int  rLen    = snprintf(response, sizeof(response),
                            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: %s\r\nContent-Length: %d\r\n\r\n%s",
                            serverClose ? "close" : "keep-alive", bodyLen, body);

    if (write(fd, response, rLen) != rLen)
      break;

    len = 0;

    if (serverClose)
      break;
  }

  close(fd);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// serverRun - accept loop
//
static void* serverRun(void* arg)
{
  int listenFd = (int) (long) arg;

  while (1)
  {
    int fd = accept(listenFd, NULL, NULL);
    if (fd < 0)
      continue;

    pthread_t tid;
    pthread_create(&tid, NULL, connServe, (void*) (long) fd);
    pthread_detach(tid);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// serverStart - a listener on an ephemeral port of 127.0.0.1
//
static void serverStart(void)
{
  int                fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sa;
  socklen_t          saLen = sizeof(sa);

  memset(&sa, 0, sizeof(sa));
  sa.sin_family      = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port        = 0;

  if ((bind(fd, (struct sockaddr*) &sa, sizeof(sa)) != 0) || (listen(fd, 16) != 0))
  {
    perror("bind/listen");
    exit(2);
  }

  getsockname(fd, (struct sockaddr*) &sa, &saLen);
  serverPort = ntohs(sa.sin_port);

  pthread_t tid;
  pthread_create(&tid, NULL, serverRun, (void*) (long) fd);
  pthread_detach(tid);
}



// -----------------------------------------------------------------------------
//
// bodyIs - does the response carry this body?
//
static bool bodyIs(CorRestClientResponse* respP, const char* expected)
{
  int len = strlen(expected);

  return (respP->body != NULL) && (respP->bodyLen == len) && (memcmp(respP->body, expected, len) == 0);
}



// -----------------------------------------------------------------------------
//
// check - print the verdict of one case
//
static int check(const char* name, CorRestClientResponse* respP, const char* expected)
{
  bool ok = bodyIs(respP, expected);

  printf("%-12s %s (expected '%s', got '%.*s')\n", name, ok ? "OK" : "FAILED", expected,
         (respP->body != NULL) ? respP->bodyLen : 0, (respP->body != NULL) ? respP->body : "");

  return ok ? 0 : 1;
}



// -----------------------------------------------------------------------------
//
// sendGet - one GET with corRestClientSend
//
static void sendGet(CorRestClientResponse* respP, const char* path)
{
  char                 url[64];
  CorRestClientRequest req;

  snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", serverPort, path);
  corRestClientRequestInit(&req, CorVerbGet, url, NULL);
  corRestClientSend(&req, respP);
}



// -----------------------------------------------------------------------------
//
// multiGet - one GET with the multi engine
//
static CorRestClientMulti* multiGet(void)
{
  char                url[64];
  CorRestClientMulti* multiP = corRestClientMultiCreate(1);

  snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", serverPort);
  corRestClientMultiAdd(multiP, CorVerbGet, url, NULL, 0, NULL, 0, NULL, NULL);
  corRestClientMultiPerform(multiP, 5000);

  return multiP;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(void)
{
  int                   failures = 0;
  char                  expected[32];
  CorRestClientResponse resp1;
  CorRestClientResponse resp2;

  serverStart();
  corRestClientInit(8, 30, "clientLifetimeTest");

  // 1. send-close
  serverClose = true;
  sendGet(&resp1, "/x");
  snprintf(expected, sizeof(expected), "response-%d", responseNo);
  failures += check("send-close", &resp1, expected);
  corRestClientResponseCleanup(&resp1);

  // 2. send-reuse
  serverClose = false;
  sendGet(&resp1, "/x");
  snprintf(expected, sizeof(expected), "response-%d", responseNo);
  sendGet(&resp2, "/x");
  failures += check("send-reuse", &resp1, expected);
  corRestClientResponseCleanup(&resp1);
  corRestClientResponseCleanup(&resp2);

  // 3. multi-close
  serverClose = true;
  CorRestClientMulti* multiP = multiGet();
  snprintf(expected, sizeof(expected), "response-%d", responseNo);
  failures += check("multi-close", corRestClientMultiResponse(multiP, 0), expected);
  corRestClientMultiDestroy(multiP);

  // 4. multi-reuse
  serverClose = false;
  multiP = multiGet();
  snprintf(expected, sizeof(expected), "response-%d", responseNo);
  sendGet(&resp2, "/x");
  failures += check("multi-reuse", corRestClientMultiResponse(multiP, 0), expected);
  corRestClientMultiDestroy(multiP);
  corRestClientResponseCleanup(&resp2);

  // 5. redirect-close
  serverClose = true;
  sendGet(&resp1, "/redirect");
  snprintf(expected, sizeof(expected), "response-%d", responseNo);
  failures += check("redirect", &resp1, expected);
  corRestClientResponseCleanup(&resp1);

  printf("%s\n", (failures == 0) ? "PASS" : "FAIL");
  return (failures == 0) ? 0 : 1;
}
