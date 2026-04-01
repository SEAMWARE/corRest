#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
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
LIB_SO        = libswRest.so
LIB           = libswRest.a
CC            = gcc
INCLUDE       = -I..
DFLAGS        =
CFLAGS        = -O2 -Wall -fPIC -Wno-unused-function -fstack-protector-all $(DFLAGS) $(INCLUDE)
LIB_SOURCES   = swRestInit.c           \
                swRestStop.c           \
                swRestStateInit.c      \
                swRestServiceLookup.c  \
                swRestVerbFromString.c \
                swRestVersion.c        \
                swRestProblem.c        \
                swRestHooks.c          \
                swRestParamRegistry.c  \
                swRestClient.c         \
                swRestClientPool.c     \
                swRestClientParse.c    \
                swRestClientTls.c      \
                swRestClientMulti.c

LIB_OBJS      = $(LIB_SOURCES:c=o)

TEST          = swRestTest
TEST_SOURCES  = swRestTest.c
TEST_OBJS     = $(TEST_SOURCES:c=o)

SO_LDFLAGS    = -L../kalloc -L../kjson -L../kbase -L../klog -L../ktrace -L../kprom
SO_LIBS       = -lkalloc -lkjson -lkbase -lklog -lktrace -lkprom -lmicrohttpd -lssl -lcrypto -lpthread
SO_RPATH      = -Wl,-rpath,'$$ORIGIN/../kalloc:$$ORIGIN/../kjson:$$ORIGIN/../kbase:$$ORIGIN/../klog:$$ORIGIN/../ktrace:$$ORIGIN/../kprom'

LIBS          = ../kalloc/libkalloc.a ../kjson/libkjson.a ../kbase/libkbase.a ../klog/libklog.a ../ktrace/libktrace.a ../kprom/libkprom.a -lmicrohttpd -lssl -lcrypto -lpthread

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
						$(CC) $(CFLAGS) -c $^ -o $@

%.i: %.c
						$(CC) $(CFLAGS) -c $^ -E > $@
