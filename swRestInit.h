//
// FILE            swRestInit.h
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
#ifndef SWREST_SW_REST_INIT_H_
#define SWREST_SW_REST_INIT_H_

#include "kalloc/KAlloc.h"            // KAlloc
#include "swRest/SwRestService.h"
#include "swRest/SwRestVerb.h"        // SwRestVerb
#include "swRest/SwRestKeyValue.h"    // SwRestKeyValue



// -----------------------------------------------------------------------------
//
// swRestInit -
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
extern int swRestInit(SwRestServiceSimplified serviceV[], int services, unsigned short port, int poolSize);


// -----------------------------------------------------------------------------
//
// swRestHttpsServerCredentialsSet - serve HTTPS instead of HTTP
//
// Call before swRestInit with the PEM contents (not file paths) of the private
// key and certificate. The MHD daemon is then started with TLS enabled. NULL
// (the default) keeps the server on plain HTTP. Used by TLS test receivers
// (e.g. ftClient), not by the broker.
//
extern void swRestHttpsServerCredentialsSet(char* keyPem, char* certPem);



// -----------------------------------------------------------------------------
//
// swRestProcessRequest - run the request-dispatch core on the bound swRest state
//
// Consumes swRest.in, produces swRest.out (incl. the rendered body in
// swRest.out.payload / payloadSize). Connection-free — the MHD send lives in the
// connection handler.
//
extern void swRestProcessRequest(void);



// -----------------------------------------------------------------------------
//
// swRestProcessInProcess - run a distributed-op forward in-process (self-forward)
//
// When a forward targets this broker's own endpoint, run it directly on a fresh
// inner SwRestState instead of over a socket. Outputs are borrowed into
// respAllocP (kept alive by the caller). Returns the HTTP status code, or -1 if
// the inner request could not be set up. The caller must save/restore any
// thread-local application state the inner pipeline resets (swNgsild + deferred
// caches).
//
extern int swRestProcessInProcess(SwRestVerb       verb,
                                  const char*      path,
                                  SwRestKeyValue*  headerV,
                                  int              headerCount,
                                  const char*      body,
                                  int              bodyLen,
                                  KAlloc*          respAllocP,
                                  char**           respBodyP,
                                  int*             respBodyLenP,
                                  SwRestKeyValue** respHeaderVP,
                                  int*             respHeaderCountP);

#endif  // SWREST_SW_REST_INIT_H_
