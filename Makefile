# SPDX-License-Identifier: MIT
# Copyright 2026 the Desktop Commander C contributors
#
# C11, Linux first. No third-party libraries: pthreads and the C library only.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter
LDLIBS  += -pthread
PREFIX  ?= /usr/local

SRC := src/json.c src/util.c src/config.c src/fs.c src/edit.c src/search.c \
       src/proc.c src/misc.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := dcm

TEST_SRC := tests/test_json.c tests/test_util.c

.PHONY: all check clean install uninstall

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_json: tests/test_json.c src/json.c
	$(CC) $(CFLAGS) -o $@ tests/test_json.c src/json.c $(LDLIBS)

tests/test_util: tests/test_util.c src/util.c src/json.c src/config.c
	$(CC) $(CFLAGS) -o $@ tests/test_util.c src/util.c src/json.c src/config.c $(LDLIBS)

check: $(BIN) tests/test_json tests/test_util
	./tests/test_json
	./tests/test_util
	@if command -v python3 >/dev/null 2>&1; then \
		python3 tests/conformance.py --binary ./$(BIN); \
	else \
		echo "skip: python3 not found, conformance test not run"; \
	fi

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(BIN) tests/test_json tests/test_util
