#!/bin/sh
# Load the module, run tests/devtest.py against it, check dmesg. Run as
# root from the module dir inside the VM, after `make build/wgref`.
set -eu
fail() { echo "FAIL: $*" >&2; exit 1; }
if grep -q '^wavedevice ' /proc/modules; then
	fail "wavedevice already loaded (tools/stream.py?); rmmod it first"
fi
trap 'rmmod wavedevice 2>/dev/null || true' EXIT

dmesg -C
insmod ./wavedevice.ko
udevadm settle -t 10
python3 tests/devtest.py
rmmod wavedevice
if dmesg | grep -E 'BUG|WARNING|Oops|Call trace|refcount|leak|hung'; then
	fail "kernel complained"
fi
