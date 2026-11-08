# SPDX-License-Identifier: MIT
# Copyright 2026 the Desktop Commander C contributors
#
# C11, Linux first, one static binary. pthreads and the C library only.
#
#   make            builds ./dcm, size before speed
#   make check      unit tests, the pool test and the conformance run
#   make debug      the same with debug information
#
# Extra flags for a special build: make EXTRA="-m32" (32-bit clients),
# make EXTRA="-fsanitize=address,undefined" EXTRA_LDFLAGS="-fsanitize=address,undefined"
#   make config     writes src/config.h from src/config.def.h if it is absent
#
# The target is small: -Oz (or -Os), one section per function, and a linker
# that drops what nothing calls.

CC       ?= cc
CLANG    := $(shell $(CC) --version 2>/dev/null | grep -ci clang)
ifeq ($(CLANG),1)
OPT      ?= -Oz
else
OPT      ?= -Os
endif
CFLAGS   ?= $(OPT)
CFLAGS   += $(EXTRA) -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
            -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wno-unused-parameter \
            -Wstrict-aliasing=2 -fstrict-aliasing \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -ffunction-sections -fdata-sections -fno-common -MMD -MP
LDLIBS   += -pthread
LDFLAGS  += $(EXTRA_LDFLAGS)
PREFIX   ?= /usr/local

UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDFLAGS  += -Wl,-dead_strip
else
LDFLAGS  += -Wl,--gc-sections -Wl,--as-needed -Wl,--build-id=none
endif

SRC := src/json.c src/util.c src/config.c src/fs.c src/edit.c src/search.c \
       src/proc.c src/misc.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := dcm

TEST_BIN := tests/test_json tests/test_util

.PHONY: all check clean config debug install uninstall size

all: $(BIN)

# The user's own configuration. `make config` writes it once and never again;
# a clean checkout therefore builds with the defaults of src/config.def.h.
config:
	@test -f src/config.h || cp src/config.def.h src/config.h

src/config.h:
	cp src/config.def.h $@

$(OBJ): src/config.h

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)
	@size $(BIN) 2>/dev/null || true

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

debug:
	$(MAKE) OPT="$(OPT) -g"

tests/test_json: tests/test_json.c src/json.c src/util.c src/config.c
	$(CC) $(CFLAGS) -o $@ tests/test_json.c src/json.c src/util.c src/config.c \
	  $(LDFLAGS) $(LDLIBS)

tests/test_util: tests/test_util.c src/util.c src/json.c src/config.c
	$(CC) $(CFLAGS) -o $@ tests/test_util.c src/util.c src/json.c src/config.c \
	  $(LDFLAGS) $(LDLIBS)

check: $(BIN) tests/test_json tests/test_util
	./tests/test_json
	./tests/test_util
	@if command -v python3 >/dev/null 2>&1; then \
		python3 tests/test_pool.py ./$(BIN); \
		python3 tests/test_process.py ./$(BIN); \
		python3 tests/test_protocol.py ./$(BIN); \
		python3 tests/conformance.py --binary ./$(BIN); \
	else \
		echo "skip: python3 not found, the pool, process and conformance tests did not run"; \
	fi

size: $(BIN)
	@size $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN) $(TEST_BIN)

-include $(OBJ:.o=.d)
