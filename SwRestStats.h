//
// FILE            SwRestStats.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Prometheus metrics integration for swRest.
//
// The application creates the KpromMetric objects (it owns them),
// then passes pointers to swRest via swRestMetricsSet().
// swRest increments the counters/histograms during request handling.
//
#ifndef SWREST_SW_REST_STATS_H_
#define SWREST_SW_REST_STATS_H_

#include "kprom/kprom.h"



// -----------------------------------------------------------------------------
//
// SwRestMetrics - pointers to application-owned Prometheus metrics
//
// All pointers are optional (NULL = don't track that metric).
//
typedef struct SwRestMetrics
{
  KpromMetric*  requests;          // Counter: total requests
  KpromMetric*  requestErrors;     // Counter: requests with status >= 400
  KpromMetric*  requestDuration;   // Histogram: request duration in seconds
  KpromMetric*  responseBytes;     // Counter: total response bytes
} SwRestMetrics;



// -----------------------------------------------------------------------------
//
// swRestMetricsSet - register application-owned Prometheus metrics
//
// Must be called before swRestInit.  The metrics are not copied —
// the application must keep them alive for the lifetime of the server.
//
extern void swRestMetricsSet(SwRestMetrics* metrics);

#endif  // SWREST_SW_REST_STATS_H_
