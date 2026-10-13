# henge top-level build. Run `make help` for the list of targets.

# ---- configuration (override on the command line) ----
CC      ?= cc
AR      ?= ar
CFLAGS  := -std=c11 -Wall -Wextra -O2 -Iinclude

NDK_ROOT  ?= /opt/homebrew/share/android-ndk
NDK_CC    ?= $(NDK_ROOT)/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang
NDK_FLAGS := --target=aarch64-linux-android28 -fPIC -shared -O2 -Wl,-z,max-page-size=16384

# Android SDK, for aapt2 + android.jar (sample APK). Newest installed versions by default.
ANDROID_HOME ?= $(HOME)/android-sdk
AAPT2        ?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/build-tools/*/aapt2)))
ANDROID_JAR  ?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/platforms/android-*/android.jar)))

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
APK_LIB_SRCS     := $(filter-out apk/main.c,$(wildcard apk/*.c))
APK_LIB_OBJS     := $(patsubst apk/%.c,$(B)/apk/%.o,$(APK_LIB_SRCS))
RUNTIME_SRCS     := $(wildcard runtime/*.c runtime/*.S)
RUNTIME_OBJS     := $(patsubst runtime/%,$(B)/runtime/%.o,$(basename $(RUNTIME_SRCS)))
PUBLIC_HEADERS   := $(wildcard include/henge/*.h)
CONVERT_HEADERS  := $(PUBLIC_HEADERS) $(wildcard convert/*.h)
RUNTIME_HEADERS  := $(PUBLIC_HEADERS) $(wildcard runtime/*.h)
APK_HEADERS      := $(PUBLIC_HEADERS) $(wildcard apk/*.h)

APP_DD  := $(B)/app/dd
APP_OUT := $(APP_DD)/Build/Products/Debug-iphonesimulator/henge.app

.DEFAULT_GOAL := all
.PHONY: all help samples samples-apk tests test test-layout test-m1 test-patch test-dl test-apk app-dylib app app-run clean

all: $(B)/henge-convert $(B)/henge-apk $(B)/libhenge-runtime.a samples tests ## build the tools, runtime, sample and tests

help: ## show this help
	@echo "usage: make [target] [VAR=value ...]"
	@echo
	@grep -E '^[a-zA-Z0-9_-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-12s %s\n", $$1, $$2}'
	@echo
	@echo "variables: NDK_ROOT=$(NDK_ROOT) ANDROID_HOME=$(ANDROID_HOME)"
	@echo "           PLATFORM=$(PLATFORM) SIM=\"$(SIM)\" CONVERT_FLAGS=$(CONVERT_FLAGS)"

# ---- converter + runtime (host) ----
$(B)/convert/%.o: convert/%.c $(CONVERT_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libhenge-convert.a: $(CONVERT_LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# The CLI links the runtime only for henge_shim_provides (--check reports).
$(B)/henge-convert: convert/main.c $(B)/libhenge-convert.a $(B)/libhenge-runtime.a $(PUBLIC_HEADERS)
	$(CC) $(CFLAGS) $< $(B)/libhenge-convert.a $(B)/libhenge-runtime.a -o $@

$(B)/runtime/%.o: runtime/%.c $(RUNTIME_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/runtime/%.o: runtime/%.S
	@mkdir -p $(@D)
	$(CC) -c $< -o $@

$(B)/libhenge-runtime.a: $(RUNTIME_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# ---- APK reader (apk/, zlib) ----
$(B)/apk/%.o: apk/%.c $(APK_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libhenge-apk.a: $(APK_LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(B)/henge-apk: apk/main.c $(B)/libhenge-apk.a $(PUBLIC_HEADERS)
	$(CC) $(CFLAGS) $< $(B)/libhenge-apk.a -lz -o $@

# ---- samples (Android NDK) ----
samples: $(B)/samples/libadd.so $(B)/samples/libpatch.so $(B)/samples/libdep.so ## build sample .so files with the NDK

$(B)/samples/libadd.so: samples/libadd/add.c
	@mkdir -p $(@D)
	$(NDK_CC) $(NDK_FLAGS) -o $@ $<

# libdep.so needs libbase.so; both use Android packed relocations (APS2).
PACKED := -Wl,--pack-dyn-relocs=android
$(B)/samples/libbase.so: samples/libdep/base.c
	@mkdir -p $(@D)
	$(NDK_CC) $(NDK_FLAGS) $(PACKED) -Wl,-soname,libbase.so -o $@ $<

$(B)/samples/libdep.so: samples/libdep/dep.c $(B)/samples/libbase.so
	@mkdir -p $(@D)
	$(NDK_CC) $(NDK_FLAGS) $(PACKED) -o $@ $< -L$(B)/samples -lbase

# API 29+ so __thread uses ELF TLS (TLSDESC); every function gets a stack protector.
$(B)/samples/libpatch.so: samples/libpatch/patch.c samples/libpatch/regcheck.S
	@mkdir -p $(@D)
	$(NDK_CC) $(NDK_FLAGS) --target=aarch64-linux-android29 -fstack-protector-all -o $@ $^

# Sample APK: aapt2-compiled manifest + lib/arm64-v8a/libadd.so (deflate) +
# assets/hello.txt (stored), so both extraction paths are covered.
SAMPLE_APK_STAGE := $(B)/samples/apk
samples-apk: $(B)/samples/sample.apk ## build the sample APK (needs aapt2 + android.jar)

$(B)/samples/sample.apk: samples/apk/AndroidManifest.xml samples/apk/hello.txt $(B)/samples/libadd.so
	@test -x "$(AAPT2)" || { echo "error: aapt2 not found under $(ANDROID_HOME)/build-tools (set ANDROID_HOME or AAPT2)" >&2; exit 1; }
	@test -f "$(ANDROID_JAR)" || { echo "error: android.jar not found under $(ANDROID_HOME)/platforms (set ANDROID_HOME or ANDROID_JAR)" >&2; exit 1; }
	rm -rf $(SAMPLE_APK_STAGE) $@.tmp
	mkdir -p $(SAMPLE_APK_STAGE)/lib/arm64-v8a $(SAMPLE_APK_STAGE)/assets
	"$(AAPT2)" link -I "$(ANDROID_JAR)" --manifest samples/apk/AndroidManifest.xml -o $@.tmp
	cp $(B)/samples/libadd.so $(SAMPLE_APK_STAGE)/lib/arm64-v8a/
	cp samples/apk/hello.txt $(SAMPLE_APK_STAGE)/assets/
	cd $(SAMPLE_APK_STAGE) && zip -q -X -9 $(abspath $@.tmp) lib/arm64-v8a/libadd.so && zip -q -X -0 $(abspath $@.tmp) assets/hello.txt
	mv $@.tmp $@

# ---- tests (macOS) ----
tests: $(B)/tests/test_m1 $(B)/tests/test_layout $(B)/tests/test_patch $(B)/tests/test_dl $(B)/tests/test_apk

$(B)/tests/test_m1: tests/test_m1.c $(B)/libhenge-runtime.a $(PUBLIC_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< $(B)/libhenge-runtime.a -o $@

$(B)/tests/test_patch: tests/test_patch.c $(B)/libhenge-runtime.a $(PUBLIC_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< $(B)/libhenge-runtime.a -o $@

$(B)/tests/test_dl: tests/test_dl.c $(B)/libhenge-runtime.a $(PUBLIC_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< $(B)/libhenge-runtime.a -o $@

$(B)/tests/test_layout: tests/test_layout.c $(PUBLIC_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@

$(B)/tests/test_apk: tests/test_apk.c $(B)/libhenge-apk.a $(APK_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Iapk $< $(B)/libhenge-apk.a -lz -o $@

# Converted dylib for the macOS tests, ad-hoc signed by henge-convert's built-in
# signer and checked with Apple's codesign. Always reconverted so CONVERT_FLAGS
# changes take effect.
.PHONY: $(B)/tests/libadd.dylib
$(B)/tests/libadd.dylib: $(B)/samples/libadd.so $(B)/henge-convert
	@mkdir -p $(@D)
	$(B)/henge-convert --platform macos --sign $(CONVERT_FLAGS) -m $(B)/tests/libadd.manifest.json -o $@ $<
	codesign -v $@

.PHONY: $(B)/tests/libpatch.dylib
$(B)/tests/libpatch.dylib: $(B)/samples/libpatch.so $(B)/henge-convert
	@mkdir -p $(@D)
	$(B)/henge-convert --platform macos --sign $(CONVERT_FLAGS) -m $(B)/tests/libpatch.manifest.json -o $@ $<
	codesign -v $@

test-patch: $(B)/tests/test_patch $(B)/tests/libpatch.dylib ## run patched svc / tpidr_el0 / TLS / IFUNC code on macOS
	$(B)/tests/test_patch $(B)/tests/libpatch.dylib

# Converted libdep + libbase in their own directory (the test's library path).
.PHONY: $(B)/tests/dl
$(B)/tests/dl: $(B)/samples/libdep.so $(B)/samples/libbase.so $(B)/henge-convert
	@mkdir -p $@
	for l in libdep libbase; do \
	  $(B)/henge-convert --platform macos --sign $(CONVERT_FLAGS) -o $@/$$l.dylib $(B)/samples/$$l.so && \
	  codesign -v $@/$$l.dylib || exit 1; \
	done

test-dl: $(B)/tests/test_dl $(B)/tests/dl ## DT_NEEDED loading, APS2 and libdl on macOS
	$(B)/tests/test_dl $(B)/tests/dl

test-layout: $(B)/tests/test_layout $(B)/tests/libadd.dylib ## check the converted Mach-O layout
	$(B)/tests/test_layout $(B)/samples/libadd.so $(B)/tests/libadd.dylib

test-m1: $(B)/tests/test_m1 $(B)/tests/libadd.dylib ## dlopen + henge_link + call libadd on macOS
	$(B)/tests/test_m1 $(B)/tests/libadd.dylib

test-apk: $(B)/tests/test_apk $(B)/samples/sample.apk $(B)/henge-apk ## read the sample APK (zip + manifest)
	$(B)/tests/test_apk $(B)/samples/sample.apk $(B)/samples/libadd.so samples/apk/hello.txt
	$(B)/henge-apk info $(B)/samples/sample.apk

test: test-layout test-m1 test-patch test-dl test-apk ## run all macOS tests
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
