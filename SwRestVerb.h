//
// FILE            SwRestVerb.h
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
#ifndef SWREST_SW_REST_VERB_H_
#define SWREST_SW_REST_VERB_H_



// -----------------------------------------------------------------------------
//
// SwRestVerb - HTTP methods, used as index into service vector array
//
typedef enum SwRestVerb
{
  SwVerbGet     = 0,
  SwVerbPut     = 1,
  SwVerbPost    = 2,
  SwVerbDelete  = 3,
  SwVerbPatch   = 4,
  SwVerbHead    = 5,
  SwVerbOptions = 6,
  SwVerbs       = 7    // number of verbs / size of service vector array
} SwRestVerb;



// -----------------------------------------------------------------------------
//
// swRestVerbFromString -
//
extern SwRestVerb swRestVerbFromString(const char* method);



// -----------------------------------------------------------------------------
//
// swRestVerbToString -
//
extern const char* swRestVerbToString(SwRestVerb verb);

#endif  // SWREST_SW_REST_VERB_H_
