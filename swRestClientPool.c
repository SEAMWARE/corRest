//
// FILE            swRestClientPool.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Connection pool for the HTTP client.
//

#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, malloc, free
#include <string.h>                              // memset, strcmp
#include <stdbool.h>                             // bool, true, false
#include <unistd.h>                              // close
#include <time.h>                                // clock_gettime

#include "swRest/swRestClient.h"                 // SwRestClientConn, SwRestClientPool



// -----------------------------------------------------------------------------
//
// Global pool
//
static SwRestClientPool pool;
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

  return (int)(h & (SWC_POOL_BUCKETS - 1));
}



// -----------------------------------------------------------------------------
//
// connMatch - Check if a pooled connection matches host:port:tls
//
static bool connMatch(SwRestClientConn* conn, const char* host, unsigned short port, bool tls)
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
  SwRestClientConn** pp   = &pool.buckets[bucket].head;
  int                secs = pool.idleTimeoutSec;

  while (*pp != NULL)
  {
    SwRestClientConn* c = *pp;

    if ((now - c->lastUsed) > (uint64_t)secs * 1000)
    {
      *pp = c->next;
      pool.buckets[bucket].count--;
      pool.totalIdle--;

      if (c->ssl != NULL)
        swRestClientTlsClose(c);
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
// swRestClientPoolInit - Initialize the connection pool
//
int swRestClientPoolInit(int maxIdlePerHost, int idleTimeoutSec)
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
// swRestClientPoolDestroy - Destroy the pool, close all idle connections
//
void swRestClientPoolDestroy(void)
{
  if (!poolInited)
    return;

  pthread_mutex_lock(&pool.mutex);

  for (int i = 0; i < SWC_POOL_BUCKETS; i++)
  {
    SwRestClientConn* c = pool.buckets[i].head;

    while (c != NULL)
    {
      SwRestClientConn* next = c->next;

      if (c->ssl != NULL)
        swRestClientTlsClose(c);
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
// swRestClientPoolGet - Get a pooled connection matching host:port:tls
//
SwRestClientConn* swRestClientPoolGet(const char* host, unsigned short port, bool tls)
{
  if (!poolInited)
    return NULL;

  int      bucket = bucketHash(host, port, tls);
  uint64_t now    = nowMs();

  pthread_mutex_lock(&pool.mutex);

  evictExpired(bucket, now);

  SwRestClientConn** pp = &pool.buckets[bucket].head;

  while (*pp != NULL)
  {
    SwRestClientConn* c = *pp;

    if (connMatch(c, host, port, tls))
    {
      *pp = c->next;
      c->next = NULL;
      pool.buckets[bucket].count--;
      pool.totalIdle--;

      pthread_mutex_unlock(&pool.mutex);

      return c;
    }

    pp = &c->next;
  }

  pthread_mutex_unlock(&pool.mutex);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// swRestClientPoolPut - Return a keep-alive connection to the pool
//
void swRestClientPoolPut(SwRestClientConn* conn)
{
  if (!poolInited || conn == NULL || conn->fd < 0)
  {
    if (conn != NULL)
    {
      if (conn->ssl != NULL)
        swRestClientTlsClose(conn);
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
      swRestClientTlsClose(conn);
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
