#
# tinymcp v0.6
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# Makefile - the host build: the POSIX example, the tests, the conformance
# run against the official mcp SDK, and the code size on a Cortex-M33.
#
# SPDX-License-Identifier: Apache-2.0

CC      ?= cc
PYTHON  ?= python3
VENV    ?= .venv
ARMCC   ?= arm-none-eabi-gcc

CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wstrict-prototypes -Wmissing-prototypes -O2 -g
CFLAGS  += -I.
SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

LIB_SRCS = tinymcp.c tinymcp_json.c
HDRS     = tinymcp.h tinymcp_json.h

all: build/tinymcp_posix

build:
	mkdir -p build

build/%.o: %.c $(HDRS) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/tinymcp_posix: examples/posix/tinymcp_posix.c build/tinymcp.o build/tinymcp_json.o $(HDRS)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -o $@ $< build/tinymcp.o build/tinymcp_json.o

build/test_tinymcp: tests/test_tinymcp.c $(LIB_SRCS) $(HDRS) | build
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ $< $(LIB_SRCS)

# The protocol against a fake host: both eras, every method, every code.
test: build/test_tinymcp
	./build/test_tinymcp

# Each check must fail on a build that breaks what it guards.
mutants: tests/mutants.sh tests/test_tinymcp.c $(LIB_SRCS)
	sh tests/mutants.sh

$(VENV)/bin/python:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install -q mcp pyserial

# The official SDK client against the POSIX example, both eras.
conformance: build/tinymcp_posix $(VENV)/bin/python
	$(VENV)/bin/python tests/conformance.py ./build/tinymcp_posix

# The bridge against a fake board on a pty.
test-bridge: $(VENV)/bin/python
	$(VENV)/bin/python tests/test_bridge.py

# attend.py against a stub of the Messages API and a fake board.
test-attend: $(VENV)/bin/python
	$(VENV)/bin/python tests/test_attend.py

check: test mutants conformance test-bridge test-attend

# Code and data of the library alone, for the port table.
size: | build
	$(ARMCC) -std=c99 -Os -mcpu=cortex-m33 -mthumb -ffunction-sections \
	    -fdata-sections -I. -c tinymcp.c -o build/tinymcp.m33.o
	$(ARMCC) -std=c99 -Os -mcpu=cortex-m33 -mthumb -ffunction-sections \
	    -fdata-sections -I. -c tinymcp_json.c -o build/tinymcp_json.m33.o
	$(ARMCC:gcc=size) build/tinymcp.m33.o build/tinymcp_json.m33.o

# Comment and prose style, over the files git tracks: stage new ones first.
lint:
	$(PYTHON) hygiene/check_comment_style.py --strict
	$(PYTHON) hygiene/check_prose_style.py

clean:
	rm -rf build

.PHONY: all test mutants conformance test-bridge test-attend check size lint clean
