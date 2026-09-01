#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
LIB_SO        = libcorRest.so
LIB           = libcorRest.a
CC            = gcc
INCLUDE       = -I..
DFLAGS        =
#
# EXTRA_CFLAGS - the hook for a caller that needs to ADD flags to this build.
#
# Not DFLAGS. DFLAGS is a plain variable, so `make DFLAGS=...` REPLACES it -
# the command line beats the makefile - and a `DFLAGS +=` inside the makefile is
# ignored along with it, because += never appends to a command-line variable. A
# caller reaching for DFLAGS to add one flag therefore drops every default this
# lib sets for itself. DFLAGS is empty here today, so nothing is lost yet; the
# first -D added to it would be, silently. corNgsild lost -DANSI and
# -DCOR_WITH_ICU that way and compiled the wrong collation path under coverage.
#
# EXTRA_CFLAGS is appended LAST, so a caller's -O0 / -Wno-error also win over the
# -O2 / -Werror here, which is what an instrumented build needs.
#
CFLAGS        = -O2 -Wall -Werror -fPIC -Wno-unused-function -fstack-protector-all $(DFLAGS) $(INCLUDE) -MMD -MP $(EXTRA_CFLAGS)
LIB_SOURCES   = corRestInit.c           \
                corMimeType.c           \
                corRestStop.c           \
                corRestStateInit.c      \
                corRestServiceLookup.c  \
                corRestVerbFromString.c \
                corRestVersion.c        \
                corRestProblem.c        \
                corRestOutHeader.c      \
                corRestHooks.c          \
                corRestParamRegistry.c  \
                corRestClient.c         \
                corRestClientPool.c     \
                corRestClientParse.c    \
                corRestClientTls.c      \
                corRestClientMulti.c

LIB_OBJS      = $(LIB_SOURCES:c=o)
LIB_DEPS      = $(LIB_SOURCES:c=d)

TEST          = corRestTest
TEST_SOURCES  = corRestTest.c
TEST_OBJS     = $(TEST_SOURCES:c=o)

SO_LDFLAGS    = -L../kalloc -L../kjson -L../kbase -L../klog -L../ktrace -L../kprom
SO_LIBS       = -lkalloc -lkjson -lklog -lktrace -lkprom -lkbase -lmicrohttpd -lssl -lcrypto -lpthread
SO_RPATH      = -Wl,-rpath,'$$ORIGIN/../kalloc:$$ORIGIN/../kjson:$$ORIGIN/../kbase:$$ORIGIN/../klog:$$ORIGIN/../ktrace:$$ORIGIN/../kprom'

LIBS          = ../kalloc/libkalloc.a ../kjson/libkjson.a ../klog/libklog.a ../ktrace/libktrace.a ../kprom/libkprom.a ../kbase/libkbase.a -lmicrohttpd -lssl -lcrypto -lpthread -lm

all: $(LIB_SO) $(LIB) $(TEST)

clean:
						rm -f *.o
						rm -f *.a
						rm -f *~
						rm -f *.so
						rm -f $(TEST)

install:    all
						@if [ ! -d bin ]; then mkdir bin; fi
						cp $(TEST) bin/

di:         install

ci:         clean install

$(LIB):			$(LIB_OBJS) $(LIB_SOURCES)
						ar r $(LIB) $(LIB_OBJS)
						ranlib $(LIB)

$(LIB_SO):	$(LIB_OBJS) $(LIB_SOURCES)
						$(CC) -shared $(LIB_OBJS) -o $(LIB_SO) $(SO_LDFLAGS) $(SO_LIBS) $(SO_RPATH)

$(TEST):		$(TEST_OBJS) $(LIB)
						$(CC) -o $(TEST) $(TEST_OBJS) $(LIB) $(LIBS)


%.o: %.c
						$(CC) $(CFLAGS) -c $< -o $@

%.i: %.c
						$(CC) $(CFLAGS) -c $^ -E > $@

-include $(LIB_DEPS)
