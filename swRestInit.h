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

#include "swRest/SwRestService.h"



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

#endif  // SWREST_SW_REST_INIT_H_
