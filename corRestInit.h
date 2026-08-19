//
// FILE            corRestInit.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#ifndef CORREST_INIT_H_
#define CORREST_INIT_H_

#include "kalloc/KAlloc.h"            // KAlloc
#include "corRest/CorRestService.h"
#include "corRest/CorRestVerb.h"        // CorRestVerb
#include "corRest/CorRestKeyValue.h"    // CorRestKeyValue



// -----------------------------------------------------------------------------
//
// corRestInit -
//
// Initialize the REST library: prepare service lookup tables and start MHD.
//
// Parameters:
//   serviceV      - flat array of service definitions (each includes its verb)
//   services      - number of entries in serviceV
//   port          - TCP port to listen on
//   poolSize      - MHD thread pool size
//
// GET services automatically get HEAD support (same handler, body suppressed).
// OPTIONS is auto-generated for all registered URL paths.
//
// Returns 0 on success, -1 on error.
//
extern int corRestInit(CorRestServiceSimplified serviceV[], int services, unsigned short port, int poolSize);


// -----------------------------------------------------------------------------
//
// corRestHttpsServerCredentialsSet - serve HTTPS instead of HTTP
//
// Call before corRestInit with the PEM contents (not file paths) of the private
// key and certificate. The MHD daemon is then started with TLS enabled. NULL
// (the default) keeps the server on plain HTTP. Used by TLS test receivers
// (e.g. ftClient), not by the broker.
//
extern void corRestHttpsServerCredentialsSet(char* keyPem, char* certPem);



// -----------------------------------------------------------------------------
//
// corRestProcessRequest - run the request-dispatch core on the bound corRest state
//
// Consumes corRest.in, produces corRest.out (incl. the rendered body in
// corRest.out.payload / payloadSize). Connection-free — the MHD send lives in the
// connection handler.
//
extern void corRestProcessRequest(void);



// -----------------------------------------------------------------------------
//
// corRestProcessInProcess - run a distributed-op forward in-process (self-forward)
//
// When a forward targets this broker's own endpoint, run it directly on a fresh
// inner CorRestState instead of over a socket. Outputs are borrowed into
// respAllocP (kept alive by the caller). Returns the HTTP status code, or -1 if
// the inner request could not be set up. The caller must save/restore any
// thread-local application state the inner pipeline resets (corNgsild + deferred
// caches).
//
extern int corRestProcessInProcess(CorRestVerb       verb,
                                  const char*      path,
                                  CorRestKeyValue*  headerV,
                                  int              headerCount,
                                  const char*      body,
                                  int              bodyLen,
                                  KAlloc*          respAllocP,
                                  char**           respBodyP,
                                  int*             respBodyLenP,
                                  CorRestKeyValue** respHeaderVP,
                                  int*             respHeaderCountP);

#endif  // CORREST_INIT_H_
