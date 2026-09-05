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
#
# COR_HTTP_SERVER - which HTTP server this build carries. Two, and exactly one.
#
#   mhd      libmicrohttpd, an external shared library
#   builtin  the epoll server in this repo, no external dependency
#
# Emitted as a 0/1 PAIR rather than one flag, so the source reads
# `#if COR_HTTP_SERVER_MHD` and -Wundef turns a misspelling into a compile error
# instead of silently selecting the other implementation. Same discipline as the
# COR_FEATURE_* defines the broker compiles with.
#
# In CFLAGS and not DFLAGS on purpose - see the note above: a caller who passes
# DFLAGS on the command line would drop these along with every other default.
#
# The value picks the backend SOURCE FILE as well as the defines: the two
# corRestBackend*.c files are alternatives, and compiling the unused one would
# need the library it exists to avoid.
#
COR_HTTP_SERVER ?= mhd
ifeq ($(COR_HTTP_SERVER),mhd)
  HTTP_SERVER_FLAGS   = -DCOR_HTTP_SERVER_MHD=1 -DCOR_HTTP_SERVER_BUILTIN=0
  HTTP_SERVER_SOURCE  = corRestBackendMhd.c
  HTTP_SERVER_LIBS    = -lmicrohttpd
  HTTP_SERVER_ARCHIVE =
else ifeq ($(COR_HTTP_SERVER),builtin)
  HTTP_SERVER_FLAGS   = -DCOR_HTTP_SERVER_MHD=0 -DCOR_HTTP_SERVER_BUILTIN=1
  HTTP_SERVER_SOURCE  = corRestBackendBuiltin.c
  HTTP_SERVER_LIBS    =
  #
  # corHttp is a sibling repo and a static archive, like every other lib here.
  # It is NOT folded into libcorRest.a - an archive cannot contain another one -
  # so a consumer linking the built-in flavour links both, which is what
  # coraine's CMakeLists does.
  #
  HTTP_SERVER_ARCHIVE = ../corHttp/libcorHttp.a
else
  $(error COR_HTTP_SERVER must be 'mhd' or 'builtin', not '$(COR_HTTP_SERVER)')
endif

CFLAGS        = -O2 -Wall -Werror -Wundef -fPIC -Wno-unused-function -fstack-protector-all $(DFLAGS) $(HTTP_SERVER_FLAGS) $(INCLUDE) -MMD -MP $(EXTRA_CFLAGS)
LIB_SOURCES   = corRestInit.c           \
                $(HTTP_SERVER_SOURCE)   \
                corMimeType.c           \
                corRestStop.c           \
                corRestStateInit.c      \
                corRestServiceLookup.c  \
                corRestVerbFromString.c \
                corRestUrlValueEncode.c \
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

#
# BUILD - which flavour of build this is, and where its objects live.
#
# Objects used to sit next to their sources, one set for every flavour, and that
# is a silent-wrong-answer machine: `make coverage` leaves instrumented objects
# behind, a later ordinary build finds them NEWER than the sources and relinks
# them into a binary that calls itself ordinary. This lib had the sharper end of
# it - its own test binary links without -lgcov and every instrumented object
# then wants __gcov_init, so the build failed outright rather than lying.
#
# A plain variable and not a target-specific one on purpose: target-specific
# variables (`debug: CFLAGS += -g`) are not visible when the makefile is parsed,
# so a directory derived from them would be the same directory for every target.
#
BUILD        ?= debug
OBJDIR       := obj/$(BUILD)

ifeq ($(BUILD),debug)
CFLAGS       += -g -DDEBUG
endif

LIB_OBJS      = $(addprefix $(OBJDIR)/,$(LIB_SOURCES:.c=.o))
LIB_DEPS      = $(addprefix $(OBJDIR)/,$(LIB_SOURCES:.c=.d))

#
# $(OBJDIR)/.flags - the flags these objects were built with.
#
# The directory separates the flavours; this catches a change WITHIN one. A
# caller adding EXTRA_CFLAGS changes the compile line and nothing else: sources
# are untouched, objects stay newer than them, and make rebuilds nothing. The
# stamp is rewritten only when the flags actually differ, so its timestamp moves
# exactly when a rebuild is due, and every object depends on it.
#
FLAGSTAMP    := $(OBJDIR)/.flags

TEST          = corRestTest
TEST_SOURCES  = corRestTest.c
TEST_OBJS     = $(addprefix $(OBJDIR)/,$(TEST_SOURCES:.c=.o))

SO_LDFLAGS    = -L../kalloc -L../kjson -L../kbase -L../klog -L../ktrace
SO_LIBS       = -lkalloc -lkjson -lklog -lktrace -lkbase $(HTTP_SERVER_ARCHIVE) $(HTTP_SERVER_LIBS) -lssl -lcrypto -lpthread
SO_RPATH      = -Wl,-rpath,'$$ORIGIN/../kalloc:$$ORIGIN/../kjson:$$ORIGIN/../kbase:$$ORIGIN/../klog:$$ORIGIN/../ktrace'

LIBS          = ../kalloc/libkalloc.a ../kjson/libkjson.a ../klog/libklog.a ../ktrace/libktrace.a ../kbase/libkbase.a $(HTTP_SERVER_ARCHIVE) $(HTTP_SERVER_LIBS) -lssl -lcrypto -lpthread -lm

#
# Built per flavour, then STAGED to the repo root where every consumer expects
# them. Unconditionally: comparing timestamps here would reintroduce the bug the
# object directories fix, since obj/debug/libX.a is easily older than a libX.a
# left behind by a coverage build.
#
all: $(OBJDIR)/$(LIB_SO) $(OBJDIR)/$(LIB) $(OBJDIR)/$(TEST)
						@cp -f $(OBJDIR)/$(LIB) $(LIB)
						@cp -f $(OBJDIR)/$(LIB_SO) $(LIB_SO)
						@cp -f $(OBJDIR)/$(TEST) $(TEST)

$(FLAGSTAMP): FORCE
						@mkdir -p $(OBJDIR)
						@echo '$(CFLAGS)' | cmp -s - $@ 2>/dev/null || echo '$(CFLAGS)' > $@

FORCE:

clean:
						rm -rf obj
						#
						# ...and the legacy in-tree artefacts. Objects live under obj/ now, but a tree
						# built before that still has .o/.d beside its sources - and, worse, .gcno:
						# gcovr reads those and reports a file nobody compiled as entirely unexecuted,
						# which once moved the published figure by three points.
						#
						rm -f *.o *.d *.gcno *.gcda
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

#
# The staged artefacts are targets in their own right, so a caller can ask for
# `make libcorX.a` and get the current flavour's archive copied into place. The
# coverage target does exactly that, by name.
#
$(LIB): $(OBJDIR)/$(LIB)
						@cp -f $< $@

$(LIB_SO): $(OBJDIR)/$(LIB_SO)
						@cp -f $< $@

#
# Removed and rebuilt, never updated in place. `ar r` REPLACES and ADDS but
# never removes, so an archive built once with corRestBackendMhd.o keeps it
# after a switch to the built-in backend - and the link then has two definitions
# of every corRestBackend* function, one of which wants libmicrohttpd.
#
$(OBJDIR)/$(LIB):	$(LIB_OBJS)
						@rm -f $@
						ar r $@ $(LIB_OBJS)
						ranlib $@

$(OBJDIR)/$(LIB_SO):	$(LIB_OBJS)
						$(CC) -shared $(LIB_OBJS) -o $@ $(SO_LDFLAGS) $(SO_LIBS) $(SO_RPATH)

$(OBJDIR)/$(TEST):	$(TEST_OBJS) $(OBJDIR)/$(LIB)
						$(CC) -o $@ $(TEST_OBJS) $(OBJDIR)/$(LIB) $(LIBS)


$(OBJDIR)/%.o: %.c $(FLAGSTAMP)
						@mkdir -p $(OBJDIR)
						$(CC) $(CFLAGS) -c $< -o $@

%.i: %.c
						$(CC) $(CFLAGS) -c $^ -E > $@

-include $(LIB_DEPS)
