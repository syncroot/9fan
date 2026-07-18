CC ?= clang
PREFIX ?= /usr/local
BUILD_DIR := build
TARGET := $(BUILD_DIR)/9fan
GUARD_TARGET := $(BUILD_DIR)/9fan-guard
SOURCES := src/main.c src/smc.c src/smc_codec.c src/curve.c src/controller.c \
	src/response_monitor.c src/signal_guard.c src/thermal_guard.m
GUARD_SOURCES := src/guard.c src/smc_codec.c
HEADERS := src/smc.h src/smc_codec.h src/curve.h src/controller.h \
	src/response_monitor.h src/signal_guard.h src/thermal_guard.h
BASE_CFLAGS := -std=c11 -Os -flto -Wall -Wextra -Wpedantic -Werror \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2
ALL_CFLAGS := $(BASE_CFLAGS) $(CFLAGS)
TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror $(CFLAGS)
FRAMEWORKS := -framework IOKit -framework CoreFoundation
MAIN_FRAMEWORKS := $(FRAMEWORKS) -framework Foundation

.PHONY: all analyze clean install test

all: $(TARGET) $(GUARD_TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(SOURCES) $(MAIN_FRAMEWORKS) -o $@
	strip -x $@

$(GUARD_TARGET): $(GUARD_SOURCES) src/smc_codec.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(GUARD_SOURCES) $(FRAMEWORKS) -o $@
	strip -x $@

$(BUILD_DIR)/test-curve: tests/test_curve.c src/curve.c src/curve.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_curve.c src/curve.c -o $@

$(BUILD_DIR)/test-controller: tests/test_controller.c src/controller.c src/controller.h src/curve.c src/curve.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_controller.c src/controller.c src/curve.c -o $@

$(BUILD_DIR)/test-smc-codec: tests/test_smc_codec.c src/smc_codec.c src/smc_codec.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_smc_codec.c src/smc_codec.c -o $@

$(BUILD_DIR)/test-response-monitor: tests/test_response_monitor.c src/response_monitor.c src/response_monitor.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_response_monitor.c src/response_monitor.c -o $@

$(BUILD_DIR)/test-thermal-guard: tests/test_thermal_guard.m src/thermal_guard.m src/thermal_guard.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_thermal_guard.m src/thermal_guard.m -framework Foundation -o $@

$(BUILD_DIR)/test-signal-guard: tests/test_signal_guard.c src/signal_guard.c src/signal_guard.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_signal_guard.c src/signal_guard.c -o $@

test: $(BUILD_DIR)/test-curve $(BUILD_DIR)/test-controller $(BUILD_DIR)/test-smc-codec \
	$(BUILD_DIR)/test-response-monitor $(BUILD_DIR)/test-thermal-guard \
	$(BUILD_DIR)/test-signal-guard
	$(BUILD_DIR)/test-curve
	$(BUILD_DIR)/test-controller
	$(BUILD_DIR)/test-smc-codec
	$(BUILD_DIR)/test-response-monitor
	$(BUILD_DIR)/test-thermal-guard
	$(BUILD_DIR)/test-signal-guard

analyze:
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/curve.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/controller.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/response_monitor.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/signal_guard.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/smc_codec.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/smc.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/main.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/thermal_guard.m
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/guard.c

install: $(TARGET) $(GUARD_TARGET)
	test "$$(realpath "$$(dirname "$(DESTDIR)$(PREFIX)")")/$$(basename "$(DESTDIR)$(PREFIX)")" = "$(DESTDIR)$(PREFIX)"
	test ! -L "$(DESTDIR)$(PREFIX)"
	test ! -L "$(DESTDIR)$(PREFIX)/bin"
	test -d "$(DESTDIR)$(PREFIX)" || install -d -o root -g wheel -m 755 "$(DESTDIR)$(PREFIX)"
	test "$$(realpath "$(DESTDIR)$(PREFIX)")" = "$(DESTDIR)$(PREFIX)"
	test -d "$(DESTDIR)$(PREFIX)/bin" || install -d -o root -g wheel -m 755 "$(DESTDIR)$(PREFIX)/bin"
	test "$$(realpath "$(DESTDIR)$(PREFIX)/bin")" = "$(DESTDIR)$(PREFIX)/bin"
	test "$$(stat -f %u "$(DESTDIR)$(PREFIX)")" = 0
	test "$$(stat -f %Lp "$(DESTDIR)$(PREFIX)")" = 755
	test "$$(stat -f %u "$(DESTDIR)$(PREFIX)/bin")" = 0
	test "$$(stat -f %Lp "$(DESTDIR)$(PREFIX)/bin")" = 755
	test ! -L "$(DESTDIR)$(PREFIX)/bin/9fan"
	test ! -L "$(DESTDIR)$(PREFIX)/bin/9fan-guard"
	install -o root -g wheel -m 755 $(GUARD_TARGET) "$(DESTDIR)$(PREFIX)/bin/9fan-guard"
	install -o root -g wheel -m 755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/9fan"

clean:
	rm -rf $(BUILD_DIR) *.plist
