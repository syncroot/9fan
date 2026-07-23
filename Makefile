override CC := /usr/bin/clang
override SHELL := /bin/sh
PREFIX ?= /usr/local
override BUILD_DIR := build
override TARGET := $(BUILD_DIR)/9fan
override ENGINE_TARGET := $(BUILD_DIR)/9fan-engine
override GUARD_TARGET := $(BUILD_DIR)/9fan-guard
TEST_TARGETS := $(BUILD_DIR)/test-curve $(BUILD_DIR)/test-controller \
	$(BUILD_DIR)/test-smc-codec $(BUILD_DIR)/test-response-monitor \
	$(BUILD_DIR)/test-thermal-guard $(BUILD_DIR)/test-signal-guard \
	$(BUILD_DIR)/test-platform-policy $(BUILD_DIR)/test-lease \
	$(BUILD_DIR)/test-protocol $(BUILD_DIR)/test-channel \
	$(BUILD_DIR)/test-guard-protocol $(BUILD_DIR)/test-hot-policy \
	$(BUILD_DIR)/test-tui
FRONTEND_SOURCES := src/main.c src/smc.c src/smc_codec.c src/curve.c \
	src/protocol.c src/channel.c src/platform_policy.c src/signal_guard.c \
	src/thermal_guard.m src/tui.c
ENGINE_SOURCES := src/engine.c src/smc.c src/smc_codec.c src/curve.c src/controller.c \
	src/response_monitor.c src/signal_guard.c src/thermal_guard.m src/hot_policy.c
ENGINE_SOURCES += src/platform_policy.c src/lease.c src/protocol.c src/channel.c
GUARD_SOURCES := src/guard.c src/smc_codec.c src/lease.c src/guard_protocol.c
HEADERS := src/smc.h src/smc_codec.h src/curve.h src/controller.h \
	src/response_monitor.h src/signal_guard.h src/thermal_guard.h \
	src/platform_policy.h src/lease.h src/protocol.h src/channel.h \
	src/guard_protocol.h src/hot_policy.h src/tui.h src/version.h
BASE_CFLAGS := -std=c11 -Os -flto -Wall -Wextra -Wpedantic -Werror \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2
ALL_CFLAGS := $(BASE_CFLAGS) $(CFLAGS)
TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror $(CFLAGS)
FRAMEWORKS := -framework IOKit -framework CoreFoundation
MAIN_FRAMEWORKS := $(FRAMEWORKS) -framework Foundation

.PHONY: all analyze check-unprivileged clean install test verify

check-unprivileged:
	test "$$(/usr/bin/id -u)" -ne 0 || { echo "Never run make as root or through sudo."; false; }

$(TEST_TARGETS): | check-unprivileged

all: $(TARGET) $(ENGINE_TARGET) $(GUARD_TARGET) | check-unprivileged

$(TARGET): $(FRONTEND_SOURCES) $(HEADERS) | check-unprivileged
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -DNINEFAN_SMC_READ_ONLY \
		$(FRONTEND_SOURCES) $(MAIN_FRAMEWORKS) -o $@
	/usr/bin/strip -x $@

$(ENGINE_TARGET): $(ENGINE_SOURCES) $(HEADERS) | check-unprivileged
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(ENGINE_SOURCES) $(MAIN_FRAMEWORKS) -o $@
	/usr/bin/strip -x $@

$(GUARD_TARGET): $(GUARD_SOURCES) $(HEADERS) | check-unprivileged
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $(GUARD_SOURCES) $(FRAMEWORKS) -o $@
	/usr/bin/strip -x $@

$(BUILD_DIR)/test-curve: tests/test_curve.c src/curve.c src/curve.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_curve.c src/curve.c -o $@

$(BUILD_DIR)/test-controller: tests/test_controller.c src/controller.c src/controller.h src/curve.c src/curve.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_controller.c src/controller.c src/curve.c -o $@

$(BUILD_DIR)/test-smc-codec: tests/test_smc_codec.c src/smc_codec.c src/smc_codec.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_smc_codec.c src/smc_codec.c -o $@

$(BUILD_DIR)/test-response-monitor: tests/test_response_monitor.c src/response_monitor.c src/response_monitor.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_response_monitor.c src/response_monitor.c -o $@

$(BUILD_DIR)/test-thermal-guard: tests/test_thermal_guard.m src/thermal_guard.m src/thermal_guard.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_thermal_guard.m src/thermal_guard.m -framework Foundation -o $@

$(BUILD_DIR)/test-signal-guard: tests/test_signal_guard.c src/signal_guard.c src/signal_guard.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_signal_guard.c src/signal_guard.c -o $@

$(BUILD_DIR)/test-platform-policy: tests/test_platform_policy.c src/platform_policy.c src/platform_policy.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_platform_policy.c src/platform_policy.c -o $@

$(BUILD_DIR)/test-lease: tests/test_lease.c src/lease.c src/lease.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_lease.c src/lease.c -o $@

$(BUILD_DIR)/test-protocol: tests/test_protocol.c src/protocol.c src/protocol.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_protocol.c src/protocol.c -o $@

$(BUILD_DIR)/test-channel: tests/test_channel.c src/channel.c src/channel.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_channel.c src/channel.c -o $@

$(BUILD_DIR)/test-guard-protocol: tests/test_guard_protocol.c src/guard_protocol.c src/guard_protocol.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_guard_protocol.c src/guard_protocol.c -o $@

$(BUILD_DIR)/test-hot-policy: tests/test_hot_policy.c src/hot_policy.c src/hot_policy.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_hot_policy.c src/hot_policy.c -o $@

$(BUILD_DIR)/test-tui: tests/test_tui.c src/tui.c src/tui.h src/curve.c src/curve.h src/protocol.c src/protocol.h
	@/bin/mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) \
		tests/test_tui.c src/tui.c src/curve.c src/protocol.c -o $@

test: $(TEST_TARGETS) | check-unprivileged
	$(BUILD_DIR)/test-curve
	$(BUILD_DIR)/test-controller
	$(BUILD_DIR)/test-smc-codec
	$(BUILD_DIR)/test-response-monitor
	$(BUILD_DIR)/test-thermal-guard
	$(BUILD_DIR)/test-signal-guard
	$(BUILD_DIR)/test-platform-policy
	$(BUILD_DIR)/test-lease
	$(BUILD_DIR)/test-protocol
	$(BUILD_DIR)/test-channel
	$(BUILD_DIR)/test-guard-protocol
	$(BUILD_DIR)/test-hot-policy
	$(BUILD_DIR)/test-tui

analyze: | check-unprivileged
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/curve.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/controller.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/response_monitor.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/signal_guard.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/smc_codec.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/smc.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) \
		-DNINEFAN_SMC_READ_ONLY src/smc.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/main.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/engine.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/thermal_guard.m
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/guard.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/platform_policy.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/lease.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/protocol.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/channel.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/guard_protocol.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/hot_policy.c
	$(CC) --analyze -Xanalyzer -analyzer-output=text $(BASE_CFLAGS) src/tui.c

verify: check-unprivileged
	/usr/bin/make clean
	/usr/bin/make all
	/usr/bin/make test
	/usr/bin/make analyze
	/usr/bin/git diff --check
	@forbidden="$$(/usr/bin/printf '\342\200\224')"; \
		matches="$$(/usr/bin/git grep -n -e "$$forbidden" -- .)"; \
		status=$$?; \
		test "$$status" -eq 1 || { \
			/usr/bin/printf '%s\n' "$$matches"; \
			echo "Forbidden U+2014 character found in tracked content."; \
			false; \
		}
	@forbidden="$$(/usr/bin/printf '\342\200\224')"; \
		matches="$$(/usr/bin/git ls-files | /usr/bin/grep -F "$$forbidden")"; \
		status=$$?; \
		test "$$status" -eq 1 || { \
			/usr/bin/printf '%s\n' "$$matches"; \
			echo "Forbidden U+2014 character found in a tracked filename."; \
			false; \
		}
	/usr/bin/git ls-files --error-unmatch -- \
		$(FRONTEND_SOURCES) $(ENGINE_SOURCES) $(GUARD_SOURCES) $(HEADERS) \
		tests/*.c tests/*.m Makefile > /dev/null
	/usr/bin/git ls-files -z | /usr/bin/xargs -0 /usr/bin/shasum -a 256 -- > $(BUILD_DIR)/SOURCE_SHA256SUMS
	/usr/bin/shasum -a 256 -c $(BUILD_DIR)/SOURCE_SHA256SUMS
	/usr/bin/shasum -a 256 $(TARGET) $(ENGINE_TARGET) $(GUARD_TARGET) > $(BUILD_DIR)/SHA256SUMS
	/usr/bin/shasum -a 256 -c $(BUILD_DIR)/SHA256SUMS
	{ \
		/usr/bin/shasum -a 256 $(TARGET) | /usr/bin/awk '{print $$1 "  /usr/local/bin/.9fan.new"}'; \
		/usr/bin/shasum -a 256 $(ENGINE_TARGET) | /usr/bin/awk '{print $$1 "  /usr/local/libexec/.9fan-engine.new"}'; \
		/usr/bin/shasum -a 256 $(GUARD_TARGET) | /usr/bin/awk '{print $$1 "  /usr/local/libexec/.9fan-guard.new"}'; \
	} > $(BUILD_DIR)/INSTALL_SHA256SUMS
	{ \
		/usr/bin/shasum -a 256 $(TARGET) | /usr/bin/awk '{print $$1 "  /usr/local/bin/9fan"}'; \
		/usr/bin/shasum -a 256 $(ENGINE_TARGET) | /usr/bin/awk '{print $$1 "  /usr/local/libexec/9fan-engine"}'; \
		/usr/bin/shasum -a 256 $(GUARD_TARGET) | /usr/bin/awk '{print $$1 "  /usr/local/libexec/9fan-guard"}'; \
	} > $(BUILD_DIR)/INSTALLED_SHA256SUMS
	@echo "Verified release hashes:"
	@/bin/cat $(BUILD_DIR)/SHA256SUMS

# Deliberately has no build prerequisites. Never invoke make itself with sudo;
# the supported unprivileged target elevates only fixed installation operations.
install:
	test "$$(/usr/bin/id -u)" -ne 0 || { echo "Do not run make with sudo. Run 'make verify', then 'make install'."; false; }
	test -t 0 && test -t 1 || { echo "make install requires an interactive terminal for guarded hardware validation."; false; }
	case ":$$PATH:" in *:/usr/local/bin:*) ;; *) echo "/usr/local/bin must be in PATH before installing."; false;; esac
	{ test ! -e "$$HOME/.local/bin/9fan" && test ! -L "$$HOME/.local/bin/9fan"; } || { echo "Remove the duplicate $$HOME/.local/bin/9fan before installing."; false; }
	{ resolved="$$(command -v 9fan 2>/dev/null || true)"; test -z "$$resolved" || test "$$resolved" = "/usr/local/bin/9fan"; } || { echo "Another 9fan shadows /usr/local/bin/9fan in PATH."; false; }
	test "$(BUILD_DIR)" = "build"
	test "$(TARGET)" = "build/9fan"
	test "$(ENGINE_TARGET)" = "build/9fan-engine"
	test "$(GUARD_TARGET)" = "build/9fan-guard"
	test "$(CC)" = "/usr/bin/clang"
	test "$(PREFIX)" = "/usr/local"
	test -z "$(DESTDIR)"
	test ! -L build
	test "$$(cd -P build && pwd -P)" = "$$(pwd -P)/build"
	test "$$(/usr/bin/stat -f %u build)" = "$$(/usr/bin/id -u)"
	test "$$(/usr/bin/stat -f %Lp build)" = 755
	test -f build/SHA256SUMS
	test -f build/SOURCE_SHA256SUMS
	test -f build/INSTALL_SHA256SUMS
	test -f build/INSTALLED_SHA256SUMS
	test ! -L build/9fan
	test ! -L build/9fan-engine
	test ! -L build/9fan-guard
	test ! -L build/SHA256SUMS
	test ! -L build/SOURCE_SHA256SUMS
	test ! -L build/INSTALL_SHA256SUMS
	test ! -L build/INSTALLED_SHA256SUMS
	test "$$(/usr/bin/stat -f %u build/9fan)" = "$$(/usr/bin/id -u)"
	test "$$(/usr/bin/stat -f %u build/9fan-engine)" = "$$(/usr/bin/id -u)"
	test "$$(/usr/bin/stat -f %u build/9fan-guard)" = "$$(/usr/bin/id -u)"
	test "$$(/usr/bin/stat -f %u build/SOURCE_SHA256SUMS)" = "$$(/usr/bin/id -u)"
	test "$$(/usr/bin/stat -f %l build/9fan)" = 1
	test "$$(/usr/bin/stat -f %l build/9fan-engine)" = 1
	test "$$(/usr/bin/stat -f %l build/9fan-guard)" = 1
	/usr/bin/shasum -a 256 -c build/SHA256SUMS
	/usr/bin/shasum -a 256 -c build/SOURCE_SHA256SUMS
	test ! -L "/usr/local"
	test ! -L "/usr/local/bin"
	test "$$(cd -P /usr/local && pwd -P)" = "/usr/local"
	test "$$(cd -P /usr/local/bin && pwd -P)" = "/usr/local/bin"
	test "$$(/usr/bin/stat -f %u /usr/local)" = 0
	test "$$(/usr/bin/stat -f %Lp /usr/local)" = 755
	test "$$(/usr/bin/stat -f %u /usr/local/bin)" = 0
	test "$$(/usr/bin/stat -f %Lp /usr/local/bin)" = 755
	! /usr/bin/pgrep -x 9fan
	! /usr/bin/pgrep -x 9fan-engine
	! /usr/bin/pgrep -x 9fan-guard
	/usr/bin/sudo -v
	/usr/bin/sudo /bin/rm -f /var/db/9fan.validation
	test ! -e /usr/local/libexec || test ! -L /usr/local/libexec
	test -e /usr/local/libexec || /usr/bin/sudo /usr/bin/install -d -o root -g wheel -m 755 /usr/local/libexec
	test "$$(cd -P /usr/local/libexec && pwd -P)" = "/usr/local/libexec"
	test "$$(/usr/bin/stat -f %u /usr/local/libexec)" = 0
	test "$$(/usr/bin/stat -f %Lp /usr/local/libexec)" = 755
	test ! -L /usr/local/bin/9fan
	test ! -L /usr/local/libexec/9fan-engine
	test ! -L /usr/local/libexec/9fan-guard
	test ! -L /usr/local/libexec/9fan.SHA256SUMS
	test ! -e /usr/local/libexec/9fan.SHA256SUMS || test "$$(/usr/bin/stat -f '%Su %Sg %Lp' /usr/local/libexec/9fan.SHA256SUMS)" = "root wheel 644"
	test ! -e /usr/local/libexec/9fan.SHA256SUMS || /usr/bin/shasum -a 256 -c /usr/local/libexec/9fan.SHA256SUMS || { echo "Existing installation is inconsistent; replacing it from the verified build."; true; }
	/usr/bin/shasum -a 256 -c build/SHA256SUMS
	/usr/bin/shasum -a 256 -c build/SOURCE_SHA256SUMS
	/usr/bin/sudo /bin/rm -f /var/run/9fan.install.sha256 /var/run/9fan.installed.sha256
	/usr/bin/sudo /bin/rm -f /usr/local/bin/.9fan.new /usr/local/libexec/.9fan-engine.new /usr/local/libexec/.9fan-guard.new
	/usr/bin/sudo /usr/bin/install -o root -g wheel -m 755 $(GUARD_TARGET) /usr/local/libexec/.9fan-guard.new
	/usr/bin/sudo /usr/bin/install -o root -g wheel -m 755 $(ENGINE_TARGET) /usr/local/libexec/.9fan-engine.new
	/usr/bin/sudo /usr/bin/install -o root -g wheel -m 755 $(TARGET) /usr/local/bin/.9fan.new
	/usr/bin/shasum -a 256 -c build/INSTALL_SHA256SUMS
	/usr/bin/sudo /bin/mv -f /usr/local/libexec/.9fan-guard.new /usr/local/libexec/9fan-guard
	/usr/bin/sudo /bin/mv -f /usr/local/libexec/.9fan-engine.new /usr/local/libexec/9fan-engine
	/usr/bin/sudo /bin/mv -f /usr/local/bin/.9fan.new /usr/local/bin/9fan
	/usr/bin/shasum -a 256 -c build/INSTALLED_SHA256SUMS
	/usr/bin/sudo /usr/bin/install -o root -g wheel -m 644 build/INSTALLED_SHA256SUMS /usr/local/libexec/9fan.SHA256SUMS
	test "$$(/usr/bin/stat -f '%Su %Sg %Lp' /usr/local/libexec/9fan-guard)" = "root wheel 755"
	test "$$(/usr/bin/stat -f '%Su %Sg %Lp' /usr/local/libexec/9fan-engine)" = "root wheel 755"
	test "$$(/usr/bin/stat -f '%Su %Sg %Lp' /usr/local/bin/9fan)" = "root wheel 755"
	test "$$(/usr/bin/stat -f '%Su %Sg %Lp' /usr/local/libexec/9fan.SHA256SUMS)" = "root wheel 644"
	test "$$(/bin/cat /usr/local/libexec/9fan.SHA256SUMS)" = "$$(/usr/bin/shasum -a 256 /usr/local/bin/9fan /usr/local/libexec/9fan-engine /usr/local/libexec/9fan-guard)"
	test "$$(command -v 9fan)" = "/usr/local/bin/9fan"
	@echo "Installed files verified. Guarded hardware validation is required to finish."
	/usr/local/bin/9fan self-test
	/usr/local/bin/9fan status
	@echo "9fan installation and guarded hardware validation passed."

clean:
	/bin/rm -rf $(BUILD_DIR) *.plist
