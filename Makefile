CC ?= clang
PREFIX ?= $(HOME)/.local
BUILD_DIR := build
TARGET := $(BUILD_DIR)/9fan
SOURCES := src/main.c src/smc.c src/curve.c
HEADERS := src/smc.h src/curve.h
CFLAGS ?= -std=c11 -Os -flto -Wall -Wextra -Wpedantic -Werror
FRAMEWORKS := -framework IOKit -framework CoreFoundation

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SOURCES) $(FRAMEWORKS) -o $@
	strip -x $@

$(BUILD_DIR)/test-curve: tests/test_curve.c src/curve.c src/curve.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror tests/test_curve.c src/curve.c -o $@

test: $(BUILD_DIR)/test-curve
	$(BUILD_DIR)/test-curve

install: $(TARGET)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/9fan"

clean:
	rm -rf $(BUILD_DIR)
