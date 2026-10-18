#!/usr/bin/env python3
"""Behavior tests against the live device. Run as root with the module
loaded, from the module dir (needs build/wgref)."""
import errno
import os
import signal
import subprocess
import sys
import threading
import time

import numpy as np

DEV = "/dev/sine"
# Defaults the module ships with (shape, freq, rate, amp).
DEFAULTS = (0, 440, 48000, 128)
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL: {msg}", file=sys.stderr)


def ref(n, params=DEFAULTS):
    args = ["build/wgref", *map(str, params), str(n)]
    return subprocess.run(args, check=True, capture_output=True).stdout


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
        p.send_signal(sig)
        try:
            p.wait(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()
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
