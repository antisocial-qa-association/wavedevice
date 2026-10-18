#!/usr/bin/env python3
"""Behavior tests against the live device. Run as root with the module
loaded, from the module dir (needs build/wgref)."""
import ctypes
import errno
import fcntl
import mmap
import random
import struct
import os
import signal
import subprocess
import sys
import threading
import time

import numpy as np

DEV = "/dev/sine"
SHAPES = {"sine": 0, "sawtooth": 1, "triangle": 2, "square": 3}
PARAMS = "/sys/module/wavedevice/parameters/"
# Defaults the module ships with (shape, freq, rate, amp).
DEFAULTS = (0, 440, 48000, 128)
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL: {msg}", file=sys.stderr)


def ref(n, params=DEFAULTS, *retunes):
    """Reference bytes; each retune is (freq, rate, amp, nbytes)."""
    args = ["build/wgref", *map(str, params), str(n)]
    for r in retunes:
        args += map(str, r)
    return subprocess.run(args, check=True, capture_output=True).stdout


# asm-generic ioctl encoding (arm64, x86, riscv; not mips/ppc/sparc).
def _ioc(d, nr, size):
    return d << 30 | size << 16 | ord("~") << 8 | nr


GET = _ioc(2, 1, 16)
SET = _ioc(1, 2, 16)


def get(fd):
    return struct.unpack("4I", fcntl.ioctl(fd, GET, bytes(16)))


def set_(fd, freq, rate, amp, flags=0):
    """Returns 0 or the errno."""
    try:
        fcntl.ioctl(fd, SET, struct.pack("4I", freq, rate, amp, flags))
        return 0
    except OSError as e:
        return e.errno


def readn(n, bs, dev=DEV):
    fd = os.open(dev, os.O_RDONLY)
    out = bytearray()
    try:
        while len(out) < n:
            b = os.read(fd, min(bs, n - len(out)))
            if not b:
                raise AssertionError("EOF")
            out += b
    finally:
        os.close(fd)
    return bytes(out)


def test_matches_reference():
    n = 300_000
    want = ref(n)
    for bs in (1, 7, 4095, 4096, 4097, 65536, 1 << 20):
        check(readn(n, bs) == want, f"bs={bs} differs from reference")


def test_all_shapes_match_reference():
    n = 100_000
    for name, shape in SHAPES.items():
        want = ref(n, (shape, *DEFAULTS[1:]))
        for bs in (1, 4097):
            check(readn(n, bs, "/dev/" + name) == want,
                  f"/dev/{name} bs={bs} differs from reference")
        st = os.stat("/dev/" + name)
        check(st.st_mode & 0o777 == 0o444, f"/dev/{name} mode {st.st_mode:o}")


def getparam(name):
    with open(PARAMS + name) as f:
        return int(f.read())


def setparam(name, val):
    """Returns 0 or the errno the write failed with."""
    try:
        with open(PARAMS + name, "w") as f:
            f.write(str(val))
        return 0
    except OSError as e:
        return e.errno


def test_params():
    try:
        old = os.open(DEV, os.O_RDONLY)
        check(setparam("freq", 1000) == 0, "set freq")
        check(setparam("rate", 44100) == 0, "set rate")
        check(setparam("amp", 64) == 0, "set amp")
        check(readn(5000, 4096, "/dev/square") == ref(5000, (3, 1000, 44100, 64)),
              "new open ignores params")
        check(os.read(old, 5000) == ref(5000), "params disturbed open stream")
        os.close(old)

        for name, bad in (("freq", 0), ("freq", 384001), ("rate", 0),
                          ("rate", 768001), ("amp", 257), ("amp", -1),
                          ("freq", "loud"), ("rate", "\n")):
            before = getparam(name)
            check(setparam(name, bad) == errno.EINVAL, f"{name}={bad!r} accepted")
            check(getparam(name) == before, f"{name}={bad!r} changed value")

        # Each value in range, but not together: open() refuses.
        check(setparam("rate", 8000) == 0 and setparam("freq", 4001) == 0,
              "in-range values refused")
        try:
            os.close(os.open(DEV, os.O_RDONLY))
            check(False, "open with freq > rate/2 succeeded")
        except OSError as e:
            check(e.errno == errno.EINVAL, f"bad combo open errno {e.errno}")
        check(setparam("freq", 4000) == 0, "nyquist freq")
        os.close(os.open(DEV, os.O_RDONLY))
    finally:
        for name, val in zip(("freq", "rate", "amp"), DEFAULTS[1:]):
            setparam("freq", 1)  # always valid against any rate
            setparam(name, val)
        setparam("freq", DEFAULTS[1])
    check([getparam(n) for n in ("freq", "rate", "amp")] == list(DEFAULTS[1:]),
          "params not restored")


def test_params_root_only():
    pid = os.fork()
    if pid == 0:
        os.setgid(65534)
        os.setuid(65534)
        rc = setparam("freq", 1000)
        # nobody can still read and play the devices
        ok = rc == errno.EACCES and len(readn(10, 10)) == 10
        os._exit(0 if ok else 1)
    _, status = os.waitpid(pid, 0)
    check(status == 0, "unprivileged user could set params or not read")
    check(getparam("freq") == DEFAULTS[1], "freq changed")


def test_ioctl_get_set():
    fd = os.open(DEV, os.O_RDONLY)
    other = os.open(DEV, os.O_RDONLY)
    try:
        check(get(fd) == (*DEFAULTS[1:], 0), f"GET {get(fd)}")
        a = os.read(fd, 1000)
        check(set_(fd, 880, 48000, 64) == 0, "SET refused")
        check(get(fd) == (880, 48000, 64, 0), f"GET after SET {get(fd)}")
        b = os.read(fd, 1000)
        check(a + b == ref(1000, DEFAULTS, (880, 48000, 64, 1000)),
              "retune not phase-continuous")
        check(get(other) == (*DEFAULTS[1:], 0), "SET leaked to another fd")
        check(os.read(other, 2000) == ref(2000), "other fd's stream changed")
        check(getparam("freq") == DEFAULTS[1], "SET changed module param")

        for bad in ((30000, 48000, 64), (440, 48000, 257), (440, 0, 64),
                    (0, 48000, 64), (440, 768001, 64)):
            check(set_(fd, *bad) == errno.EINVAL, f"SET {bad} accepted")
        check(set_(fd, 440, 48000, 64, flags=1) == errno.EINVAL,
              "nonzero flags accepted")
        check(get(fd) == (880, 48000, 64, 0), "failed SET changed params")
        c = os.read(fd, 1000)
        check(a + b + c == ref(1000, DEFAULTS, (880, 48000, 64, 2000)),
              "failed SET disturbed stream")

        for req, arg, want in ((_ioc(1, 2, 8), bytes(8), errno.ENOTTY),
                               (_ioc(2, 9, 16), bytes(16), errno.ENOTTY),
                               (0x5401, bytes(64), errno.ENOTTY),  # TCGETS
                               (GET, 0, errno.EFAULT), (SET, 0, errno.EFAULT)):
            try:
                fcntl.ioctl(fd, req, arg)
                check(False, f"ioctl {req:#x} succeeded")
            except OSError as e:
                check(e.errno == want, f"ioctl {req:#x} errno {e.errno}")
    finally:
        os.close(fd)
        os.close(other)


def test_ioctl_sweep():
    """Many retunes between reads of odd sizes; must match exactly."""
    rnd = random.Random(7)
    for name, shape in SHAPES.items():
        fd = os.open("/dev/" + name, os.O_RDONLY)
        got = bytearray(os.read(fd, 333))
        segs = []
        for _ in range(100):
            rate = rnd.choice((8000, 22050, 44100, 48000, 96000))
            seg = (rnd.randint(1, rate // 2), rate, rnd.randint(0, 256),
                   rnd.randint(1, 9000))
            check(set_(fd, *seg[:3]) == 0, f"SET {seg}")
            got += os.read(fd, seg[3])
            segs.append(seg)
        os.close(fd)
        check(bytes(got) == ref(333, (shape, *DEFAULTS[1:]), *segs),
              f"/dev/{name} sweep differs from reference")


def test_ioctl_unprivileged_and_shape():
    pid = os.fork()
    if pid == 0:
        os.setgid(65534)
        os.setuid(65534)
        fd = os.open("/dev/square", os.O_RDONLY)
        ok = set_(fd, 1000, 48000, 256) == 0 and \
            os.read(fd, 48) == ref(48, (3, 1000, 48000, 256))
        os._exit(0 if ok else 1)
    _, status = os.waitpid(pid, 0)
    check(status == 0, "nobody can't retune own fd, or shape changed")


def test_full_and_zero_reads():
    fd = os.open(DEV, os.O_RDONLY)
    try:
        check(len(os.read(fd, 1 << 20)) == 1 << 20, "short 1 MiB read")
        check(os.read(fd, 0) == b"", "zero-length read")
    finally:
        os.close(fd)


def test_frequency():
    rate = DEFAULTS[2]
    x = np.frombuffer(readn(rate * 4, 65536), dtype=np.uint8).astype(float)
    spec = np.abs(np.fft.rfft(x - x.mean()))
    peak = np.argmax(spec) * rate / len(x)
    check(abs(peak - DEFAULTS[1]) <= 0.5, f"peak at {peak} Hz")


def test_signal_interrupts_big_read():
    n = 1 << 29
    signal.signal(signal.SIGALRM, lambda *_: None)
    fd = os.open(DEV, os.O_RDONLY)
    try:
        signal.setitimer(signal.ITIMER_REAL, 0.02)
        t0 = time.monotonic()
        got = len(os.read(fd, n))
        dt = time.monotonic() - t0
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        os.close(fd)
    check(0 < got < n, f"interrupted read returned {got}")
    check(dt < 0.5, f"interrupted read took {dt:.3f}s")


def test_kill():
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
        p = subprocess.Popen(["cat", DEV], stdout=subprocess.DEVNULL)
        time.sleep(0.2)
        check(p.poll() is None, f"cat exited early ({p.returncode})")
        p.send_signal(sig)
        try:
            p.wait(timeout=2)
            check(p.returncode == -sig, f"cat {sig.name}: rc {p.returncode}")
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            check(False, f"cat survived {sig.name}")


def test_not_writable_or_seekable():
    fd = os.open(DEV, os.O_RDWR)  # root may open it; writes must still fail
    try:
        try:
            os.write(fd, b"x")
            check(False, "write succeeded")
        except OSError as e:
            check(e.errno == errno.EINVAL, f"write errno {e.errno}")
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            check(False, "lseek succeeded")
        except OSError as e:
            check(e.errno == errno.ESPIPE, f"lseek errno {e.errno}")
    finally:
        os.close(fd)


def test_independent_opens():
    want = ref(3000)
    a, b = os.open(DEV, os.O_RDONLY), os.open(DEV, os.O_RDONLY)
    try:
        got_a, got_b = bytearray(), bytearray()
        for _ in range(3):
            got_a += os.read(a, 1000)
            got_b += os.read(b, 1000)
    finally:
        os.close(a)
        os.close(b)
    check(bytes(got_a) == want and bytes(got_b) == want,
          "opens share or disturb each other's phase")


def test_shared_fd_is_one_stream():
    """Threads sharing an fd each get the next chunk of one wave."""
    threads, per, bs = 4, 200, 4096
    want = ref(threads * per * bs)
    index = {want[i:i + bs]: i // bs for i in range(0, len(want), bs)}
    check(len(index) == threads * per, "reference chunks not unique")
    fd = os.open(DEV, os.O_RDONLY)
    chunks = []
    lock = threading.Lock()

    def worker():
        for _ in range(per):
            c = os.read(fd, bs)
            with lock:
                chunks.append(c)

    ts = [threading.Thread(target=worker) for _ in range(threads)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    os.close(fd)
    seen = sorted(index.get(c, -1) for c in chunks)
    check(seen == list(range(threads * per)),
          "shared-fd reads tore or skipped the stream")


def test_fault_keeps_stream_continuous():
    """A read that faults partway delivers what fit, and the next read
    continues the wave exactly where the good bytes stopped."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.read.restype = ctypes.c_ssize_t
    libc.read.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
    libc.mprotect.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)
    pg = mmap.PAGESIZE
    m = mmap.mmap(-1, 2 * pg)
    base = ctypes.addressof(ctypes.c_char.from_buffer(m))
    check(libc.mprotect(base + pg, pg, 0) == 0, "mprotect")
    start = 100  # so the fault lands mid-chunk, not on a chunk boundary
    fd = os.open(DEV, os.O_RDONLY)
    try:
        got = libc.read(fd, base + start, 2 * pg)
        # copy_to_user may stop a little short of the fault, never past it.
        check(pg - start - 64 <= got <= pg - start,
              f"faulting read returned {got}")
        want = ref(got + 5100)
        first = m[start:start + got]
        rest = os.read(fd, 5000)
        check(first + rest == want[:got + 5000],
              "stream skipped or repeated after fault")
        check(libc.read(fd, None, 10) == -1 and
              ctypes.get_errno() == errno.EFAULT, "NULL read not EFAULT")
        check(os.read(fd, 100) == want[got + 5000:],
              "stream moved after an all-fault read")
    finally:
        os.close(fd)
        libc.mprotect(base + pg, pg, 3)
        m.close()


def test_throughput():
    n = 256 << 20
    fd = os.open(DEV, os.O_RDONLY)
    t0 = time.monotonic()
    left = n
    while left:
        left -= len(os.read(fd, min(left, 1 << 20)))
    dt = time.monotonic() - t0
    os.close(fd)
    print(f"info: {n / dt / 1e6:.0f} MB/s "
          f"({n / dt / DEFAULTS[2]:.0f}x realtime)")


for name, fn in list(globals().items()):
    if name.startswith("test_"):
        fn()
if failures:
    sys.exit(f"devtest: {failures} failure(s)")
print("devtest: ok")
