# On Linux: `make` builds wavedevice.ko against the running kernel.
# On the dev Mac: `make vm-smoke` etc. drive the lima test VM (tools/vm).
KDIR ?= /lib/modules/$(shell uname -r)/build

modules clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

# DKMS: rebuilds the module for every kernel you install. As root:
#   make dkms-install    copy source to /usr/src, build, install
#   make dkms-uninstall  remove from every kernel and /usr/src
VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DKMS_SRC := /usr/src/wavedevice-$(VERSION)
DKMS_FILES := Kbuild Makefile dkms.conf LICENSE wavedevice.c wavegen.h \
	sine_table.h uapi/wavedevice.h

dkms-install:
	install -d $(DKMS_SRC)/uapi
	for f in $(DKMS_FILES); do install -m 644 $$f $(DKMS_SRC)/$$f; done
	dkms install wavedevice/$(VERSION)

dkms-uninstall:
	dkms remove wavedevice/$(VERSION) --all
	rm -rf $(DKMS_SRC)

# Userspace tests of the generator core (wavegen.h). Needs a Linux-ish
# cc with ASan/UBSan; from the Mac use `make vm-test`.
TEST_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all

test: build/test_wavegen
	build/test_wavegen
	python3 tools/gensine.py | cmp - sine_table.h

build/test_wavegen: tests/test_wavegen.c wavegen.h sine_table.h
	mkdir -p build
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_wavegen.c

build/wgref: tests/wgref.c wavegen.h sine_table.h
	mkdir -p build
	$(CC) $(TEST_CFLAGS) -o $@ tests/wgref.c

# Kernel style check with a pinned upstream checkpatch.pl (fetched once).
# wavegen.h's userspace typedef shim trips PREFER_KERNEL_TYPES; ignored.
CHECKPATCH_REF := v6.12.111
CHECKPATCH := build/checkpatch/checkpatch.pl
CHECKPATCH_URL := https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/scripts

checkpatch: $(CHECKPATCH)
	perl $(CHECKPATCH) --no-tree --strict --ignore PREFER_KERNEL_TYPES \
		-f wavedevice.c wavegen.h uapi/wavedevice.h sine_table.h

$(CHECKPATCH):
	mkdir -p build/checkpatch
	for f in checkpatch.pl spelling.txt const_structs.checkpatch; do \
		curl -sSf -o build/checkpatch/$$f "$(CHECKPATCH_URL)/$$f?h=$(CHECKPATCH_REF)" || exit 1; \
	done

VM_TARGETS := vm-up vm-push vm-build vm-test vm-smoke vm-devtest vm-dkms vm-check \
	vm-shell
$(VM_TARGETS):
	tools/vm $(@:vm-%=%)

.PHONY: modules clean dkms-install dkms-uninstall test checkpatch $(VM_TARGETS)
