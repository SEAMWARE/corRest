//
// FILE            CorRestStats.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Prometheus metrics integration for corRest.
//
// The application creates the KpromMetric objects (it owns them),
// then passes pointers to corRest via corRestMetricsSet().
// corRest increments the counters/histograms during request handling.
//
#ifndef CORREST_STATS_H_
#define CORREST_STATS_H_

#include "kprom/kprom.h"



// -----------------------------------------------------------------------------
//
// CorRestMetrics - pointers to application-owned Prometheus metrics
//
// All pointers are optional (NULL = don't track that metric).
//
typedef struct CorRestMetrics
{
  KpromMetric*  requests;          // Counter: total requests
  KpromMetric*  requestErrors;     // Counter: requests with status >= 400
  KpromMetric*  requestDuration;   // Histogram: request duration in seconds
  KpromMetric*  responseBytes;     // Counter: total response bytes
} CorRestMetrics;



// -----------------------------------------------------------------------------
//
// corRestMetricsSet - register application-owned Prometheus metrics
//
// Must be called before corRestInit.  The metrics are not copied —
// the application must keep them alive for the lifetime of the server.
//
extern void corRestMetricsSet(CorRestMetrics* metrics);

#endif  // CORREST_STATS_H_
