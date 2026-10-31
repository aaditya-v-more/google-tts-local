# Build with MSYS2's UCRT64 toolchain:  make -C windows
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
VERSION := $(shell cat VERSION 2>/dev/null || cat ../native/VERSION 2>/dev/null || echo 0.4.0)
SOURCES := gtts.c speech.c elf.c libc.c format.c json.c
HEADERS := engine.h speech.h json.h
LIBS := -lws2_32 -lbcrypt -lsynchronization -lpsapi -lshell32
LDFLAGS := -static -s

all: gtts.exe gttsw.exe

gtts.exe: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -DGTTS_VERSION='"$(VERSION)"' $(SOURCES) -o $@ $(LDFLAGS) $(LIBS)

gttsw.exe: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -DGTTS_VERSION='"$(VERSION)"' -DGTTS_WINDOWLESS -mwindows $(SOURCES) -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f gtts.exe gttsw.exe

.PHONY: all clean
