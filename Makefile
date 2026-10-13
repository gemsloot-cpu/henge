# henge top-level build. Run `make help` for the list of targets.

# ---- configuration (override on the command line) ----
CC      ?= cc
AR      ?= ar
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Iinclude

NDK_ROOT  ?= /opt/homebrew/share/android-ndk
NDK_CC    ?= $(NDK_ROOT)/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang
NDK_FLAGS := --target=aarch64-linux-android28 -fPIC -shared -O2 -Wl,-z,max-page-size=16384

# CONVERT_FLAGS: extra henge-convert flags (e.g. --libsystem)
# PLATFORM:      app-dylib target, iossim (simulator) or ios (device)
# SIM:           simulator device name for app / app-run
# APP_TIMEOUT:   app-run: seconds to wait for the m1 summary line
CONVERT_FLAGS ?=
PLATFORM      ?= iossim
SIM           ?= iPhone 17
APP_TIMEOUT   ?= 60

B := build

# ---- sources ----
# The converter library is everything in convert/ except the CLI (main.c), so it can
# be compiled into other hosts (e.g. the iOS app) on its own.
CONVERT_LIB_SRCS := $(filter-out convert/main.c,$(wildcard convert/*.c))
CONVERT_LIB_OBJS := $(patsubst convert/%.c,$(B)/convert/%.o,$(CONVERT_LIB_SRCS))
RUNTIME_SRCS     := $(wildcard runtime/*.c)
RUNTIME_OBJS     := $(patsubst runtime/%.c,$(B)/runtime/%.o,$(RUNTIME_SRCS))
PUBLIC_HEADERS   := $(wildcard include/henge/*.h)
CONVERT_HEADERS  := $(PUBLIC_HEADERS) $(wildcard convert/*.h)
RUNTIME_HEADERS  := $(PUBLIC_HEADERS) $(wildcard runtime/*.h)

APP_DD  := $(B)/app/dd
APP_OUT := $(APP_DD)/Build/Products/Debug-iphonesimulator/henge.app

.DEFAULT_GOAL := all
.PHONY: all help samples tests test test-layout test-m1 app-dylib app app-run clean

all: $(B)/henge-convert $(B)/libhenge-runtime.a samples tests ## build the converter, runtime, sample and tests

help: ## show this help
	@echo "usage: make [target] [VAR=value ...]"
	@echo
	@grep -E '^[a-zA-Z0-9_-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-12s %s\n", $$1, $$2}'
	@echo
	@echo "variables: NDK_ROOT=$(NDK_ROOT)"
	@echo "           PLATFORM=$(PLATFORM) SIM=\"$(SIM)\" CONVERT_FLAGS=$(CONVERT_FLAGS)"

# ---- converter + runtime (host) ----
$(B)/convert/%.o: convert/%.c $(CONVERT_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libhenge-convert.a: $(CONVERT_LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(B)/henge-convert: convert/main.c $(B)/libhenge-convert.a $(PUBLIC_HEADERS)
	$(CC) $(CFLAGS) $< $(B)/libhenge-convert.a -o $@

$(B)/runtime/%.o: runtime/%.c $(RUNTIME_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libhenge-runtime.a: $(RUNTIME_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# ---- samples (Android NDK) ----
samples: $(B)/samples/libadd.so ## build sample .so files with the NDK

$(B)/samples/libadd.so: samples/libadd/add.c
	@mkdir -p $(@D)
	$(NDK_CC) $(NDK_FLAGS) -o $@ $<

# ---- tests (macOS) ----
tests: $(B)/tests/test_m1 $(B)/tests/test_layout

$(B)/tests/test_m1: tests/test_m1.c $(B)/libhenge-runtime.a $(PUBLIC_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< $(B)/libhenge-runtime.a -o $@

$(B)/tests/test_layout: tests/test_layout.c $(PUBLIC_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@

# Converted + ad-hoc signed dylib for the macOS tests. Always reconverted so
# CONVERT_FLAGS changes take effect.
.PHONY: $(B)/tests/libadd.dylib
$(B)/tests/libadd.dylib: $(B)/samples/libadd.so $(B)/henge-convert
	@mkdir -p $(@D)
	$(B)/henge-convert --platform macos $(CONVERT_FLAGS) -m $(B)/tests/libadd.manifest.json -o $@ $<
	codesign -s - -f $@

test-layout: $(B)/tests/test_layout $(B)/tests/libadd.dylib ## check the converted Mach-O layout
	$(B)/tests/test_layout $(B)/samples/libadd.so $(B)/tests/libadd.dylib

test-m1: $(B)/tests/test_m1 $(B)/tests/libadd.dylib ## dlopen + henge_link + call libadd on macOS
	$(B)/tests/test_m1 $(B)/tests/libadd.dylib

test: test-layout test-m1 ## run all macOS tests
	@echo "ALL TESTS PASSED"

# ---- iOS host app (app/) ----
# Converted libadd.dylib for the app bundle. Left unsigned: Xcode embeds it into
# Frameworks/ and signs it with the app's identity (CodeSignOnCopy).
# Called from the app's pre-build phase (scripts/app-prebuild.sh).
app-dylib: $(B)/samples/libadd.so $(B)/henge-convert ## convert libadd.so for the app (PLATFORM=iossim|ios)
	@case "$(PLATFORM)" in iossim|ios) ;; *) echo "PLATFORM must be iossim or ios" >&2; exit 1;; esac
	@mkdir -p $(B)/app
	$(B)/henge-convert --platform $(PLATFORM) $(CONVERT_FLAGS) -m $(B)/app/libadd.manifest.json -o $(B)/app/libadd.dylib $<

# Simulator build, ad-hoc signed ("Sign to Run Locally"), so no Apple team or
# app/Local.xcconfig is needed. NDK_ROOT is passed through to the pre-build phase.
app: ## generate the Xcode project and build the app for the simulator
	cd app && xcodegen --quiet
	NDK_ROOT="$(NDK_ROOT)" xcodebuild -project app/henge.xcodeproj -scheme henge -configuration Debug \
	  -destination 'platform=iOS Simulator,name=$(SIM)' -derivedDataPath $(APP_DD) \
	  CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY=- DEVELOPMENT_TEAM= \
	  -quiet build
	@echo "built $(APP_OUT)"

app-run: ## boot SIM if needed, install + launch the app, require "PASS: m1 (0 failures)"
	scripts/app-run.sh "$(APP_OUT)" "$(SIM)" "$(APP_TIMEOUT)"

clean: ## remove build/
	rm -rf $(B)
