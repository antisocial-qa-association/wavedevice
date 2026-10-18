#!/bin/sh
# Load/unload loop. Run as root from the module dir inside the VM.
set -eu
fail() { echo "FAIL: $*" >&2; exit 1; }
# Never leave the module loaded behind a failed check.
trap 'rmmod wavedevice 2>/dev/null || true' EXIT

dmesg -C
for i in $(seq 20); do
	insmod ./wavedevice.ko
	udevadm settle -t 10
	[ -c /dev/sine ] || fail "no /dev/sine after load $i"
	[ "$(stat -c %a /dev/sine)" = 444 ] || fail "/dev/sine mode $(stat -c %a /dev/sine)"
	rmmod wavedevice
	udevadm settle -t 10
	[ ! -e /dev/sine ] || fail "/dev/sine survived unload $i"
done

# An open fd must pin the module (.owner = THIS_MODULE).
insmod ./wavedevice.ko
udevadm settle -t 10
exec 3</dev/sine
! rmmod wavedevice 2>/dev/null || fail "rmmod succeeded with /dev/sine open"
exec 3<&-
rmmod wavedevice

if dmesg | grep -E 'BUG|WARNING|Oops|Call trace|refcount|leak'; then
	fail "kernel complained"
fi
echo "smoke: ok (20 load/unload cycles, open fd pins module)"
