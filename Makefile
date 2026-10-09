# jsonc -- SPDX-License-Identifier: GPL-3.0-or-later
#
# t/run.sh compiles the test programs itself, so the sanitizer target is just a
# CFLAGS override.  -ffile-prefix-map keeps the build reproducible: without it
# -g writes $(CURDIR) into DWARF .debug_line_str.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -ffile-prefix-map=$(CURDIR)=.
ASAN     = -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
	   -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan test-valgrind clean

all: jsonc

jsonc: jsonc.c
	$(CC) $(CFLAGS) -o $@ jsonc.c -lm

test: all
	CC='$(CC)' CFLAGS='$(CFLAGS)' ./t/run.sh

# We parse untrusted bytes and sweep every truncation of every corpus file,
# which is exactly the shape of bug the sanitizers catch.  Not optional.
test-asan:
	CC='$(CC)' CFLAGS='$(ASAN)' UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		ASAN_OPTIONS=detect_leaks=1 ./t/run.sh

# `make test-asan` leaves instrumented binaries behind and valgrind refuses to
# run those, so force a clean build first.
test-valgrind:
	rm -f jsonc
	$(MAKE) jsonc
	CC='$(CC)' CFLAGS='$(CFLAGS)' JSONC_VALGRIND=1 ./t/run.sh

clean:
	rm -f jsonc t/unit
	rm -rf scratch
