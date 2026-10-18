#!/usr/bin/env python3
"""Serve the wavedevice devices over TCP for listening from another box.

Port 7671 is /dev/sine, 7672 sawtooth, 7673 triangle, 7674 square. Each
connection opens the device afresh, so it gets its own wave from phase
0. TCP backpressure paces it to the listener:

    ffplay -f u8 -ar 48000 -ch_layout mono -nodisp tcp://HOST:7671

No auth. Binds 127.0.0.1 unless you pass --host (e.g. --host 0.0.0.0);
anyone who can reach the port gets audio. At most --max connections
are served at once; extra ones are closed straight away.
"""
import argparse
import shutil
import socketserver
import threading

SHAPES = ("sine", "sawtooth", "triangle", "square")

ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=7671, help="first of four")
ap.add_argument("--max", type=int, default=8, help="connections at once")
args = ap.parse_args()
slots = threading.BoundedSemaphore(args.max)


def handler_for(dev):
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            if not slots.acquire(blocking=False):
                return
            try:
                with open(dev, "rb", buffering=0) as f, \
                        self.request.makefile("wb") as out:
                    shutil.copyfileobj(f, out, 4096)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                slots.release()
    return Handler


socketserver.ThreadingTCPServer.allow_reuse_address = True
socketserver.ThreadingTCPServer.daemon_threads = True
for i, shape in enumerate(SHAPES):
    srv = socketserver.ThreadingTCPServer((args.host, args.port + i),
                                          handler_for("/dev/" + shape))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f"/dev/{shape} on {args.host}:{args.port + i}", flush=True)
threading.Event().wait()
