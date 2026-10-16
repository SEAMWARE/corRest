#ifndef COR_REST_CLIENT_PROXY_H_
#define COR_REST_CLIENT_PROXY_H_

//
// FILE            corRestClientProxy.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The client's HTTP proxy (COR_WITH_CLIENT_PROXY): https through a CONNECT tunnel, http with the absolute URI.
// Both engines - corRestClientSend and the multi engine - take the route a request's host has.
//
#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// CorRestClientProxyRoute - the proxy a request goes through
//
typedef struct CorRestClientProxyRoute
{
  char            host[256];
  unsigned short  port;
  char            auth[512];                           // a Proxy-Authorization value, or ""
} CorRestClientProxyRoute;



// -----------------------------------------------------------------------------
//
// corRestClientProxyFromEnv - the proxies of the environment: https_proxy, http_proxy, no_proxy (or their
// upper-case names). An unset variable: no proxy for that scheme. false: a URL that is not an http:// proxy
// URL - errBuf says why, and nothing changed
//
extern bool corRestClientProxyFromEnv(char* errBuf, int errBufLen);



// -----------------------------------------------------------------------------
//
// corRestClientProxySet - the proxies, given: an http:// URL each ([user:password@]host[:port], port 1080 by
// default) or NULL (none), and the no-proxy list - comma-separated: "*", host names (a domain covers its
// hosts: ".example.org" or "example.org"), addresses. Set before the first request; not thread-safe against
// requests in flight
//
extern bool corRestClientProxySet(const char* httpUrl, const char* httpsUrl, const char* noProxy, char* errBuf, int errBufLen);



// -----------------------------------------------------------------------------
//
// corRestClientProxyFor - the route of a request to 'host' (tls: https); false: straight to the host
//
extern bool corRestClientProxyFor(bool tls, const char* host, CorRestClientProxyRoute* routeP);



// -----------------------------------------------------------------------------
//
// corRestClientProxyTunnel - on 'fd', connected to the proxy: CONNECT host:port, the proxy's 2xx awaited
// (within timeoutMs). 0: the tunnel is open - TLS to the host goes next; -1: errBuf says why
//
extern int corRestClientProxyTunnel(int fd, const char* host, unsigned short port, const CorRestClientProxyRoute* routeP, int timeoutMs, char* errBuf, int errBufLen);

#endif  // COR_REST_CLIENT_PROXY_H_
