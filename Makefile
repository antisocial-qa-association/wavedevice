# On Linux: `make` builds wavedevice.ko against the running kernel.
# On the dev Mac: `make vm-smoke` etc. drive the lima test VM (tools/vm).
KDIR ?= /lib/modules/$(shell uname -r)/build

modules clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

VM_TARGETS := vm-up vm-push vm-build vm-smoke vm-shell
$(VM_TARGETS):
	tools/vm $(@:vm-%=%)

.PHONY: modules clean $(VM_TARGETS)
