# On Linux: `make` builds wavedevice.ko against the running kernel.
# On the dev Mac: `make vm-smoke` etc. drive the lima test VM (tools/vm).
KDIR ?= /lib/modules/$(shell uname -r)/build

modules clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

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

VM_TARGETS := vm-up vm-push vm-build vm-test vm-smoke vm-shell
$(VM_TARGETS):
	tools/vm $(@:vm-%=%)

.PHONY: modules clean test $(VM_TARGETS)
