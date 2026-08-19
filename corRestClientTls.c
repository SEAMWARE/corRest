//
// FILE            corRestClientTls.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// TLS support for the HTTP client.
//

#include <stdio.h>                               // snprintf
#include <stdbool.h>                             // bool
#include <string.h>                              // strlen
#include <unistd.h>                              // close

#include <openssl/ssl.h>                         // SSL_*, SSL_CTX_*
#include <openssl/err.h>                         // ERR_*

#include "corRest/corRestClient.h"                 // CorRestClientConn



// -----------------------------------------------------------------------------
//
// Global SSL context (one per process)
//
static SSL_CTX* sslCtx = NULL;



// -----------------------------------------------------------------------------
//
// corRestClientTlsInsecure - skip peer/host verification on outbound TLS
//
// Off by default (certificates are verified against the system CA store). When
// the broker is started with --insecureNotif this is turned on, so notifications
// and forwards to TLS endpoints accept self-signed certificates — the common
// case for an endpoint inside a trusted/firewalled network.
//
static bool insecure = false;

void corRestClientTlsInsecureSet(bool onoff)
{
  insecure = onoff;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsInit - Create global SSL_CTX, load system CA certs
//
int corRestClientTlsInit(void)
{
  if (sslCtx != NULL)
    return 0;

  const SSL_METHOD* method = TLS_client_method();
  sslCtx = SSL_CTX_new(method);
  if (sslCtx == NULL)
    return -1;

  if (SSL_CTX_set_default_verify_paths(sslCtx) != 1)
  {
    // Failed to load system CA certificates - continue anyway
  }

  SSL_CTX_set_verify(sslCtx, insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);

  SSL_CTX_set_min_proto_version(sslCtx, TLS1_2_VERSION);

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsCleanup - Free global SSL_CTX
//
void corRestClientTlsCleanup(void)
{
  if (sslCtx != NULL)
  {
    SSL_CTX_free(sslCtx);
    sslCtx = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsConnect - Perform TLS handshake on an existing connection
//
int corRestClientTlsConnect(CorRestClientConn* conn)
{
  if (sslCtx == NULL)
    return -1;

  SSL* ssl = SSL_new(sslCtx);
  if (ssl == NULL)
    return -1;

  SSL_set_tlsext_host_name(ssl, conn->host);
  if (!insecure)
    SSL_set1_host(ssl, conn->host);

  if (SSL_set_fd(ssl, conn->fd) != 1)
  {
    SSL_free(ssl);
    return -1;
  }

  int r = SSL_connect(ssl);
  if (r != 1)
  {
    SSL_free(ssl);
    return -1;
  }

  conn->ssl = ssl;

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsRead - Read from TLS connection
//
int corRestClientTlsRead(CorRestClientConn* conn, char* buf, int len)
{
  SSL* ssl = (SSL*)conn->ssl;
  if (ssl == NULL)
    return -1;

  int r = SSL_read(ssl, buf, len);
  if (r <= 0)
  {
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      return 0;
    if (err == SSL_ERROR_ZERO_RETURN)
      return 0;
    return -1;
  }

  return r;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsWrite - Write to TLS connection
//
int corRestClientTlsWrite(CorRestClientConn* conn, const char* buf, int len)
{
  SSL* ssl = (SSL*)conn->ssl;
  if (ssl == NULL)
    return -1;

  int r = SSL_write(ssl, buf, len);
  if (r <= 0)
  {
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      return 0;
    return -1;
  }

  return r;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsClose - Shut down TLS and free SSL handle
//
void corRestClientTlsClose(CorRestClientConn* conn)
{
  SSL* ssl = (SSL*)conn->ssl;
  if (ssl == NULL)
    return;

  SSL_shutdown(ssl);
  SSL_free(ssl);
  conn->ssl = NULL;
}
