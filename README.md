# wavedevice

`/dev/sine`, `/dev/sawtooth`, `/dev/triangle` and `/dev/square`, as a
Linux kernel module.

Someone on Mastodon said somebody should make `/dev/sine` and
`/dev/sawtooth`. Like `/dev/urandom`, but for waveforms. Half a
shitpost. This is that, and it works:

```
aplay -f U8 -r 48000 /dev/sine
```

You'll hear it.

## What you get

- Four character devices. Each one is an endless stream
  of unsigned 8-bit mono samples, centered on 128. The square has a
  pulse width.
- Defaults: 440 Hz at 48000 samples/s, amplitude 128 of 256 (about
  -6 dBFS). Full scale square waves hurt; that's why the default is half.
- Each `open()` gets its own wave, starting from phase 0. Each fd is a 
  separate wave. A file descriptor shared by threads or forks is one
  stream: each `read()` gets the next chunk.
- Reads never hit EOF and never come up short until a signal.
- Read only. Every write fails with `EINVAL`. Seeks get an `ESPIPE`.

The device doesn't pace itself. It produces bytes as fast as you read
them, about 900 MB/s when tested. Read a byte, get a byte.

### Listening

```
aplay -f U8 -r 48000 /dev/square
sox -t raw -e unsigned -b 8 -r 48000 -c 1 /dev/sawtooth -d
ffplay -f u8 -ar 48000 -ch_layout mono -nodisp /dev/triangle
head -c 480000 /dev/sine > ten-seconds.raw
```

## Install

### DKMS

DKMS rebuilds the module for each kernel you install.

```
sudo apt install dkms linux-headers-$(uname -r)   # or whatever
sudo make dkms-install
sudo modprobe wavedevice
```

Remove it with `sudo make dkms-uninstall`.

**Secure Boot:** You're kidding, right?

### Plain build

```
make                      # against the running kernel's headers
sudo insmod wavedevice.ko
```

### Kernel taint

You'll get an out-of-tree taint. If the kernel checks signatures
you get an `E` taint unless you deal with signing.

It doesn't trip the proprietary flag. See "LICENSE" for why.

## Tuning

Defaults are set as module parameters. Streams that are already
open aren't affected if these change.

```
sudo modprobe wavedevice freq=1000 rate=44100 amp=64 pw=1024
echo 220 | sudo tee /sys/module/wavedevice/parameters/freq
```

| param  | range        | default |
|--------|--------------|---------|
| `freq` | 1 .. rate/2  | 440     |
| `rate` | 1 .. 768000  | 48000   |
| `amp`  | 0 .. 256     | 128     |
| `pw`   | 0 .. 4096    | 2048    |

Invalid parameters get refused. `freq` can be above `rate/2` for the
time between writes. `open()` in that state fails with `EINVAL`, and
the kernel log says why.

To make settings stick, put them in `/etc/modprobe.d/wavedevice.conf`
or something:
`options wavedevice freq=440 amp=96`

### Retune a stream on an open fd: ioctl

Open file descriptors can have their parameters changed. The wave
carries on from where it was, so there's no click. That ought to
be enough for sweeps and glides. The API is in [`uapi/wavedevice.h`](uapi/wavedevice.h).
Changes apply only to that fd.

If you want a different shape, open that chardev. There's no way
to change an fd's shape.

```
# sweep.py:  python3 sweep.py | aplay -f U8 -r 48000
import fcntl, struct, sys
SET = 1 << 30 | 20 << 16 | ord("~") << 8 | 2   # WAVEDEVICE_SET_PARAMS
with open("/dev/sine", "rb", buffering=0) as f:
    for hz in range(200, 2000, 10):             # a 3.6 s upward sweep
        fcntl.ioctl(f, SET, struct.pack("5I", hz, 48000, 128, 2048, 0))
        sys.stdout.buffer.write(f.read(960))    # 20 ms at 48 kHz
```

### Pulse width

`/dev/square` has a pulse width, `pw`, in the SID's 12 bits: 2048 is
the classic 50% square, 1024 is a thin 25% pulse, 0 and 4096 are
flat (all low, all high). Set it with the module parameter for new
opens, or with the `pw` field of the ioctl to sweep it while the
wave runs. Sweeping it is PWM, and it sounds like a chorus.

The other three devices accept `pw` and ignore it, the way a SID
voice ignores the pulse register while it plays a sawtooth. Code
doesn't need to care which device it opened.

```
# pwm.py:  python3 pwm.py | aplay -f U8 -r 48000
import fcntl, struct, sys
SET = 1 << 30 | 20 << 16 | ord("~") << 8 | 2
with open("/dev/square", "rb", buffering=0) as f:
    for i in range(500):                        # a 5 s slow sweep
        pw = 2048 + int(1900 * abs((i % 100) / 50 - 1) - 950)
        fcntl.ioctl(f, SET, struct.pack("5I", 110, 48000, 128, pw, 0))
        sys.stdout.buffer.write(f.read(480))    # 10 ms at 48 kHz
```

(The ioctl number above uses the common Linux encoding: arm64, x86,
riscv and most others. On alpha, mips, parisc, powerpc and sparc it
differs; take it from the header.)

## How it works, and the homage

The oscillator is... approximately a 6581 SID. Very approximately.
A 24-bit phase accumulator ticks by `freq * 2^24 / rate` every
sample, and the top 8 bits become the output. Sawtooth is those bits
as they are. Triangle is the SID trick of XORing them with their own
top bit. Square is high while the phase is under the pulse width; at 2048
that's the top bit, inverted. The sine is fake. It's just a
a 256-entry table generated with [`tools/gensine.py`](tools/gensine.py).

It's integer-only, because kernels don't do floating point. It doesn't
band-limit either, aliasing is a thing. Just like back home.

This is not a SID. There's no ADSR, no filter, no ring mod, no mixing,
no voices. `/dev/sid` is left to someone with ambition and actual skill.

## Limitations

- **Sine at exactly `rate/2` is silent.** Every sample lands on a zero
  crossing. It's not technically a bug if you think about it. A square
  at `rate/2` works.
- **No `splice()` or `sendfile()`.** They fail with `EINVAL`. `pv`
  falls back to read and write without you noticing. `cat`, `dd` and
  every audio player use `read()`.
- **32-bit programs on a 64-bit kernel** should work with the ioctl,
  since the struct is all `__u32`. Nobody has tested it: the dev box is
  aarch64, which has no 32-bit mode.
- **Tested on** Linux 6.12 (Debian 13, arm64), including under KASAN,
  lockdep, kmemleak and UBSAN. It should build on 5.10 or newer. Older
  is on you.

## Development

The dev machine is a Mac, so everything kernel-related runs under
[lima](https://lima-vm.io) VM. Nothing is mounted from the Mac; the
tree is copied in with tar.

```
make vm-up        # create and provision the Debian 13 VM
make vm-check     # everything below
make vm-test      # userspace oscillator tests under ASan and UBSan
make vm-smoke     # 20 load/unload cycles, params, fd pins the module
make vm-devtest   # device behavior; byte-exact against the reference
make vm-dkms      # DKMS install, modprobe, read, uninstall
make checkpatch   # kernel style, --strict, pinned upstream script
```

Any compiler or sparse warning fails the build.

For the debug-kernel run, `tools/debugkernel.sh` builds a kernel with
KASAN, lockdep, kmemleak and UBSAN from Debian's own source, in a
second VM:

```
VM=wavedevice-debug CPUS=4 MEM=3 tools/vm up
VM=wavedevice-debug tools/vm push
limactl shell --workdir / wavedevice-debug -- \
	sh -c 'cd ~/wavedevice && sh tools/debugkernel.sh'
limactl shell --workdir / wavedevice-debug -- sudo systemctl reboot
VM=wavedevice-debug make vm-check
```

The reboot runs inside the guest because `limactl stop` seemed to
hang. The script refuses to run outside a VM. An unsigned
debug kernel the default boot entry.

`tools/stream.py` serves the devices on TCP ports 7671 to 7674 so you
can listen from another machine. It has no auth, binds `127.0.0.1`
unless you pass `--host`, and serves 8 connections at once:

```
tools/stream.py   # on the box with the module running
ffplay -f u8 -ar 48000 -ch_layout mono -nodisp tcp://127.0.0.1:7671
```

With lima, a port bound to the guest's localhost shows up on the Mac's
localhost too. Elsewhere, tunnel it with `ssh -L 7671:127.0.0.1:7671`.

## License

Public domain. See [LICENSE](LICENSE).

The kernel has no "public domain" license string, so the module
declares `MODULE_LICENSE("GPL and additional rights")`. That's true:
you have every right the GPL gives you, plus all the rest. It also
keeps the proprietary taint off. The compiled `.ko` is built against
GPL kernel headers, so any obligations that brings are between you
and the kernel. Good luck. The source is still public domain.
