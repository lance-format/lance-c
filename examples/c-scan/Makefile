# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LANCE_C_PREFIX ?= /usr/local

CPPFLAGS += -I$(LANCE_C_PREFIX)/include
# Embed the prefix's library directory so the example runs from a local install
# without LD_LIBRARY_PATH (Linux) or DYLD_LIBRARY_PATH (macOS).
LDFLAGS += -L$(LANCE_C_PREFIX)/lib -Wl,-rpath,$(LANCE_C_PREFIX)/lib
LDLIBS += -llance_c

BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/lance-c-scan

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)
