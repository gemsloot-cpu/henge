# henge top-level build. `make test` runs the M1 end-to-end and layout tests on macOS.

CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Iinclude
AR      ?= ar

NDK_ROOT  ?= /opt/homebrew/share/android-ndk
NDK_CC    ?= $(NDK_ROOT)/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang
NDK_FLAGS := --target=aarch64-linux-android28 -fPIC -shared -O2 -Wl,-z,max-page-size=16384

B := build

CONVERT_SRCS := $(wildcard convert/*.c)
RUNTIME_SRCS := $(wildcard runtime/*.c)
RUNTIME_OBJS := $(patsubst runtime/%.c,$(B)/runtime/%.o,$(RUNTIME_SRCS))
HEADERS      := $(wildcard include/henge/*.h) $(wildcard runtime/*.h)

# Extra flags for the converter during tests (e.g. CONVERT_FLAGS=--libsystem).
CONVERT_FLAGS ?=

.PHONY: all samples tests test test-layout test-m1 app-dylib clean

all: $(B)/henge-convert $(B)/libhenge-runtime.a samples tests

$(B)/henge-convert: $(CONVERT_SRCS) $(HEADERS) | $(B)
	$(CC) $(CFLAGS) $(CONVERT_SRCS) -o $@

$(B)/runtime/%.o: runtime/%.c $(HEADERS) | $(B)
	@mkdir -p $(B)/runtime
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libhenge-runtime.a: $(RUNTIME_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# ---- samples (Android NDK) ----
samples: $(B)/samples/libadd.so

$(B)/samples/libadd.so: samples/libadd/add.c
	@mkdir -p $(B)/samples
	$(NDK_CC) $(NDK_FLAGS) -o $@ $<

# ---- tests ----
tests: $(B)/tests/test_m1 $(B)/tests/test_layout

$(B)/tests/test_m1: tests/test_m1.c $(B)/libhenge-runtime.a $(HEADERS)
	@mkdir -p $(B)/tests
	$(CC) $(CFLAGS) $< $(B)/libhenge-runtime.a -o $@

$(B)/tests/test_layout: tests/test_layout.c $(HEADERS)
	@mkdir -p $(B)/tests
	$(CC) $(CFLAGS) $< -o $@

# Converted + ad-hoc signed dylib for the macOS tests.
$(B)/tests/libadd.dylib: $(B)/samples/libadd.so $(B)/henge-convert
	@mkdir -p $(B)/tests
	$(B)/henge-convert --platform macos $(CONVERT_FLAGS) -m $(B)/tests/libadd.manifest.json -o $@ $<
	codesign -s - -f $@

# Always reconvert so CONVERT_FLAGS changes take effect.
.PHONY: $(B)/tests/libadd.dylib

test-layout: $(B)/tests/test_layout $(B)/tests/libadd.dylib
	$(B)/tests/test_layout $(B)/samples/libadd.so $(B)/tests/libadd.dylib

test-m1: $(B)/tests/test_m1 $(B)/tests/libadd.dylib
	$(B)/tests/test_m1 $(B)/tests/libadd.dylib

test: test-layout test-m1
	@echo "ALL TESTS PASSED"

# ---- iOS host app (app/) ----
# Converted libadd.dylib for the app bundle. Left unsigned: Xcode embeds it into
# Frameworks/ and signs it with the app's identity (CodeSignOnCopy).
# PLATFORM=iossim (simulator) or ios (device). Called from the app's pre-build phase.
PLATFORM ?= iossim

app-dylib: $(B)/samples/libadd.so $(B)/henge-convert
	@case "$(PLATFORM)" in iossim|ios) ;; *) echo "PLATFORM must be iossim or ios" >&2; exit 1;; esac
	@mkdir -p $(B)/app
	$(B)/henge-convert --platform $(PLATFORM) $(CONVERT_FLAGS) -m $(B)/app/libadd.manifest.json -o $(B)/app/libadd.dylib $<

$(B):
	@mkdir -p $(B)

clean:
	rm -rf $(B)
