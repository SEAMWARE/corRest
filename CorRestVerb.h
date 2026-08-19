//
// FILE            CorRestVerb.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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
#ifndef CORREST_VERB_H_
#define CORREST_VERB_H_



// -----------------------------------------------------------------------------
//
// CorRestVerb - HTTP methods, used as index into service vector array
//
typedef enum CorRestVerb
{
  CorVerbGet     = 0,
  CorVerbPut     = 1,
  CorVerbPost    = 2,
  CorVerbDelete  = 3,
  CorVerbPatch   = 4,
  CorVerbHead    = 5,
  CorVerbOptions = 6,
  CorVerbs       = 7    // number of verbs / size of service vector array
} CorRestVerb;



// -----------------------------------------------------------------------------
//
// corRestVerbFromString -
//
extern CorRestVerb corRestVerbFromString(const char* method);



// -----------------------------------------------------------------------------
//
// corRestVerbToString -
//
extern const char* corRestVerbToString(CorRestVerb verb);

#endif  // CORREST_VERB_H_
