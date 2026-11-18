# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LANCE_C_PREFIX ?= /usr/local

CPPFLAGS += -I$(LANCE_C_PREFIX)/include
LDFLAGS += -L$(LANCE_C_PREFIX)/lib
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
