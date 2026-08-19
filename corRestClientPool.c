//
// FILE            corRestClientPool.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Connection pool for the HTTP client.
//

#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, malloc, free
#include <string.h>                              // memset, strcmp
#include <stdbool.h>                             // bool, true, false
#include <unistd.h>                              // close
#include <time.h>                                // clock_gettime
#include <errno.h>                               // errno, EAGAIN, EWOULDBLOCK
#include <sys/socket.h>                          // recv, MSG_PEEK, MSG_DONTWAIT

#include "corRest/corRestClient.h"                 // CorRestClientConn, CorRestClientPool



// -----------------------------------------------------------------------------
//
// Global pool
//
static CorRestClientPool pool;
static bool             poolInited = false;



// -----------------------------------------------------------------------------
//
// nowMs - Current time in milliseconds
//
static uint64_t nowMs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// bucketHash - Hash host:port:tls to a bucket index
//
static int bucketHash(const char* host, unsigned short port, bool tls)
{
  unsigned int h = 5381;
  const char*  p = host;

  while (*p)
    h = h * 33 + (unsigned char)*p++;

  h = h * 33 + port;
  h = h * 33 + tls;

  return (int)(h & (CORR_POOL_BUCKETS - 1));
}



// -----------------------------------------------------------------------------
//
// connMatch - Check if a pooled connection matches host:port:tls
//
static bool connMatch(CorRestClientConn* conn, const char* host, unsigned short port, bool tls)
{
  return (conn->port == port &&
          conn->tls  == tls  &&
          strcmp(conn->host, host) == 0) ? true : false;
}



// -----------------------------------------------------------------------------
//
// evictExpired - Evict one expired connection from a bucket
//
static void evictExpired(int bucket, uint64_t now)
{
  CorRestClientConn** pp   = &pool.buckets[bucket].head;
  int                secs = pool.idleTimeoutSec;

  while (*pp != NULL)
  {
    CorRestClientConn* c = *pp;

    if ((now - c->lastUsed) > (uint64_t)secs * 1000)
    {
      *pp = c->next;
      pool.buckets[bucket].count--;
      pool.totalIdle--;

      if (c->ssl != NULL)
        corRestClientTlsClose(c);
      if (c->fd >= 0)
        close(c->fd);
      free(c->buf);
      free(c);
      return;
    }

    pp = &c->next;
  }
}



// -----------------------------------------------------------------------------
//
// corRestClientPoolInit - Initialize the connection pool
//
int corRestClientPoolInit(int maxIdlePerHost, int idleTimeoutSec)
{
  memset(&pool, 0, sizeof(pool));

  pool.maxIdlePerHost = maxIdlePerHost;
  pool.idleTimeoutSec = idleTimeoutSec;

  if (pthread_mutex_init(&pool.mutex, NULL) != 0)
    return -1;

  poolInited = true;

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientPoolDestroy - Destroy the pool, close all idle connections
//
void corRestClientPoolDestroy(void)
{
  if (!poolInited)
    return;

  pthread_mutex_lock(&pool.mutex);

  for (int i = 0; i < CORR_POOL_BUCKETS; i++)
  {
    CorRestClientConn* c = pool.buckets[i].head;

    while (c != NULL)
    {
      CorRestClientConn* next = c->next;

      if (c->ssl != NULL)
        corRestClientTlsClose(c);
      if (c->fd >= 0)
        close(c->fd);
      free(c->buf);
      free(c);
      c = next;
    }

    pool.buckets[i].head  = NULL;
    pool.buckets[i].count = 0;
  }

  pool.totalIdle = 0;
  pthread_mutex_unlock(&pool.mutex);
  pthread_mutex_destroy(&pool.mutex);

  poolInited = false;
}



// -----------------------------------------------------------------------------
//
// connIsAlive - peek at the socket; returns false on EOF (peer FIN'd) or
// any error other than "would-block". A pooled connection that has been
// half-closed by the remote end will return EOF here, letting us evict it
// before the caller writes a request and waits the full requestTimeout
// (typically 10 s) for a recv that never comes.
//
static bool connIsAlive(int fd)
{
  if (fd < 0)
    return false;

  char    peek;
  ssize_t n = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);

  if (n == 0)                             // FIN received
    return false;
  if (n > 0)                              // data already buffered (stale response)
    return false;
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return true;                          // alive, no data yet — normal

  return false;                           // any other error: socket is bad
}



// -----------------------------------------------------------------------------
//
// corRestClientPoolGet - Get a pooled connection matching host:port:tls
//
CorRestClientConn* corRestClientPoolGet(const char* host, unsigned short port, bool tls)
{
  if (!poolInited)
    return NULL;

  int      bucket = bucketHash(host, port, tls);
  uint64_t now    = nowMs();

  pthread_mutex_lock(&pool.mutex);

  evictExpired(bucket, now);

  CorRestClientConn** pp = &pool.buckets[bucket].head;

  while (*pp != NULL)
  {
    CorRestClientConn* c = *pp;

    if (connMatch(c, host, port, tls))
    {
      *pp = c->next;
      c->next = NULL;
      pool.buckets[bucket].count--;
      pool.totalIdle--;

      pthread_mutex_unlock(&pool.mutex);

      // Drop the connection on the floor if the peer already closed it —
      // returning it would make the caller wait the full requestTimeout
      // for a response that's never coming.
      if (!connIsAlive(c->fd))
      {
        if (c->fd >= 0) close(c->fd);
        if (c->buf) free(c->buf);
        free(c);
        return NULL;
      }

      return c;
    }

    pp = &c->next;
  }

  pthread_mutex_unlock(&pool.mutex);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// corRestClientPoolPut - Return a keep-alive connection to the pool
//
void corRestClientPoolPut(CorRestClientConn* conn)
{
  if (!poolInited || conn == NULL || conn->fd < 0)
  {
    if (conn != NULL)
    {
      if (conn->ssl != NULL)
        corRestClientTlsClose(conn);
      if (conn->fd >= 0)
        close(conn->fd);
      free(conn->buf);
      free(conn);
    }
    return;
  }

  int bucket = bucketHash(conn->host, conn->port, conn->tls);

  pthread_mutex_lock(&pool.mutex);

  if (pool.buckets[bucket].count >= pool.maxIdlePerHost)
  {
    pthread_mutex_unlock(&pool.mutex);

    if (conn->ssl != NULL)
      corRestClientTlsClose(conn);
    close(conn->fd);
    free(conn->buf);
    free(conn);
    return;
  }

  conn->bufLen  = 0;
  conn->lastUsed = nowMs();
  conn->next     = pool.buckets[bucket].head;

  pool.buckets[bucket].head = conn;
  pool.buckets[bucket].count++;
  pool.totalIdle++;

  pthread_mutex_unlock(&pool.mutex);
}
