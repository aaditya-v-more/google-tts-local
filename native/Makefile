SDKROOT ?= $(shell env TMPDIR="$(TMP)" xcrun --sdk macosx --show-sdk-path)
CLANG ?= xcrun clang
ROOT := $(abspath ..)
BUILD := build
TMP := $(CURDIR)/tmp
ENV = env TMPDIR="$(TMP)" SDKROOT="$(SDKROOT)" CLANG_MODULE_CACHE_PATH="$(TMP)/clang-cache"
CFLAGS := -O2 -g -Wall -Wextra -mmacosx-version-min=13.0
VERSION ?= $(shell cat VERSION)

.PHONY: all probes check release clean
all: $(BUILD)/gtts-native $(BUILD)/gtts-engine $(BUILD)/libgtts-shim.dylib $(BUILD)/gtts-macho-convert.py

$(BUILD) tmp:
	mkdir -p "$@"

$(BUILD)/gtts-engine: main.c worker.m speech.c speech.h guest.c guest_fast.S guest_native.c guest_native.S guest_native.h $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c quad.h guest.h elf64.h hypervisor.entitlements | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -Wl,-export_dynamic -o "$@" main.c worker.m speech.c guest.c guest_fast.S guest_native.c guest_native.S $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c -framework Foundation -framework Security -framework Hypervisor -lffi
	$(ENV) codesign --force --sign - --entitlements hypervisor.entitlements "$@"

$(BUILD)/zstddeclib.o: third_party/zstddeclib.c | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -c "$<" -o "$@"

$(BUILD)/guest_native_fast.o: guest_native_fast.c guest.h guest_native.h | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -ffreestanding -fno-builtin -fno-stack-protector -c "$<" -o "$@"

$(BUILD)/guest_fast.o: guest_fast.S | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -ffreestanding -fno-builtin -c "$<" -o "$@"

$(BUILD)/macho_stubs.S: macho_stubs.py | $(BUILD) tmp
	$(ENV) PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$<" > "$@"

$(BUILD)/gtts-macho-convert.py: macho_convert.py | $(BUILD) tmp
	install -m 644 "$<" "$@"

$(BUILD)/libgtts-shim.dylib: macho_shim.c $(BUILD)/macho_stubs.S guest_native.S guest_fast.S $(BUILD)/guest_native_fast.o | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -dynamiclib -Wl,-undefined,dynamic_lookup -Wl,-install_name,@loader_path/libgtts-shim.dylib -o "$@" macho_shim.c $(BUILD)/macho_stubs.S guest_native.S guest_fast.S $(BUILD)/guest_native_fast.o
	$(ENV) codesign --force --sign - "$@"
	$(ENV) codesign --verify --strict "$@"

$(BUILD)/gtts-native: frontend.m VERSION third_party/zstd.h third_party/zstd_errors.h $(BUILD)/zstddeclib.o | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -DGTTS_VERSION='"$(VERSION)"' -fobjc-arc -fobjc-arc-exceptions -o "$@" frontend.m $(BUILD)/zstddeclib.o -framework Foundation -lcurl
	$(ENV) codesign --force --sign - "$@"

check: all $(BUILD)/check $(BUILD)/abi-check $(BUILD)/exception-check $(BUILD)/quad-check $(BUILD)/bridge-check $(BUILD)/guest_fast.o
	$(ENV) PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 check-fast.py $(BUILD)/guest_native_fast.o $(BUILD)/guest_fast.o
	$(ENV) PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 check-macho.py
	cd "$(ROOT)" && $(ENV) GTTS_NATIVE_BACKEND= "$(ROOT)/native/$(BUILD)/bridge-check"
	cd "$(ROOT)" && $(ENV) PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 native/check-fault.py "$(ROOT)/native/$(BUILD)/bridge-check"
	cd "$(ROOT)" && $(ENV) "$(ROOT)/native/$(BUILD)/quad-check"
	cd "$(ROOT)" && $(ENV) "$(ROOT)/native/$(BUILD)/abi-check"
	cd "$(ROOT)" && $(ENV) "$(ROOT)/native/check-exception.sh"
	cd "$(ROOT)" && $(ENV) PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 "$(ROOT)/native/check-worker.py"
	cd "$(ROOT)" && $(ENV) "$(ROOT)/native/$(BUILD)/check"

$(BUILD)/check: check.m | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -fobjc-arc -fobjc-arc-exceptions -o "$@" "$<" -framework Foundation

$(BUILD)/abi-check: abi-check.c abi-fixtures.S speech.c speech.h guest.c guest_fast.S guest_native.c guest_native.S guest_native.h $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c quad.h guest.h elf64.h hypervisor.entitlements | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -Wl,-export_dynamic -o "$@" abi-check.c abi-fixtures.S speech.c guest.c guest_fast.S guest_native.c guest_native.S $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c -framework Foundation -framework Security -framework Hypervisor -lffi
	$(ENV) codesign --force --sign - --entitlements hypervisor.entitlements "$@"

$(BUILD)/exception-check: exception-check.c guest.c guest_fast.S guest_native.c guest_native.S guest_native.h $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c quad.h guest.h elf64.h hypervisor.entitlements | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -Wl,-export_dynamic -o "$@" exception-check.c guest.c guest_fast.S guest_native.c guest_native.S $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c -framework Foundation -framework Security -framework Hypervisor -lffi
	$(ENV) codesign --force --sign - --entitlements hypervisor.entitlements "$@"

$(BUILD)/quad-check: quad-check.c quad.c quad.h | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -o "$@" quad-check.c quad.c

$(BUILD)/bridge-check: bridge-check.c bridge-check.S guest.c guest_fast.S guest_native.c guest_native.S guest_native.h $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c quad.h guest.h elf64.h hypervisor.entitlements | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -Wl,-export_dynamic -o "$@" bridge-check.c bridge-check.S guest.c guest_fast.S guest_native.c guest_native.S $(BUILD)/guest_native_fast.o elf_loader.c macho_loader.m shims.c quad.c -framework Foundation -framework Security -framework Hypervisor -lffi
	$(ENV) codesign --force --sign - --entitlements hypervisor.entitlements "$@"

release: | tmp
	TMPDIR="$(TMP)" ./release.sh "$(VERSION)"

probes: $(BUILD)/x18-test $(BUILD)/hv-probe

$(BUILD)/x18-test: x18_test.c x18_test.S x18.entitlements | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -ffreestanding -fno-builtin -fno-stack-protector -o "$@" x18_test.c x18_test.S
	$(ENV) codesign --force --sign - --entitlements x18.entitlements "$@"

$(BUILD)/hv-probe: hv_probe.c hv_spin.S hypervisor.entitlements | $(BUILD) tmp
	$(ENV) $(CLANG) $(CFLAGS) -o "$@" hv_probe.c hv_spin.S -framework Hypervisor
	$(ENV) codesign --force --sign - --entitlements hypervisor.entitlements "$@"

clean:
	rm -f $(BUILD)/gtts-native $(BUILD)/gtts-engine $(BUILD)/libgtts-shim.dylib $(BUILD)/gtts-macho-convert.py $(BUILD)/macho_stubs.S $(BUILD)/zstddeclib.o $(BUILD)/guest_native_fast.o $(BUILD)/guest_fast.o $(BUILD)/check $(BUILD)/abi-check $(BUILD)/exception-check $(BUILD)/quad-check $(BUILD)/bridge-check $(BUILD)/x18-test $(BUILD)/hv-probe
