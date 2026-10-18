#!/bin/sh
# Load/unload loop. Run as root from the module dir inside the VM.
set -eu
fail() { echo "FAIL: $*" >&2; exit 1; }
# Never leave the module loaded behind a failed check.
if grep -q '^wavedevice ' /proc/modules; then
	fail "wavedevice already loaded (tools/stream.py?); rmmod it first"
fi
trap 'rmmod wavedevice 2>/dev/null || true' EXIT

DEVS="sine sawtooth triangle square"
P=/sys/module/wavedevice/parameters

dmesg -C
for i in $(seq 20); do
	insmod ./wavedevice.ko
	udevadm settle -t 10
	for d in $DEVS; do
		[ -c /dev/$d ] || fail "no /dev/$d after load $i"
		[ "$(stat -c %a /dev/$d)" = 444 ] || fail "/dev/$d mode $(stat -c %a /dev/$d)"
	done
	rmmod wavedevice
	udevadm settle -t 10
	for d in $DEVS; do
		[ ! -e /dev/$d ] || fail "/dev/$d survived unload $i"
	done
done

# An open fd must pin the module (.owner = THIS_MODULE).
insmod ./wavedevice.ko
udevadm settle -t 10
exec 3</dev/sine
! rmmod wavedevice 2>/dev/null || fail "rmmod succeeded with /dev/sine open"
exec 3<&-
rmmod wavedevice

# Load-time params are honored, and bad ones refuse the load.
insmod ./wavedevice.ko freq=1000 rate=44100 amp=64
[ "$(cat $P/freq) $(cat $P/rate) $(cat $P/amp)" = "1000 44100 64" ] ||
	fail "load-time params not applied"
rmmod wavedevice
for bad in freq=0 rate=0 amp=257 freq=banana; do
	! insmod ./wavedevice.ko $bad 2>/dev/null || fail "insmod $bad loaded"
done

if dmesg | grep -E 'BUG|WARNING|Oops|Call trace|refcount|leak'; then
	fail "kernel complained"
fi
echo "smoke: ok (20 load/unload cycles x 4 devices, fd pins module, load params)"
