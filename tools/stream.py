#!/usr/bin/env python3
"""Serve the wavedevice devices over TCP for listening from another box.

Port 7671 is /dev/sine, 7672 sawtooth, 7673 triangle, 7674 square. Each
connection opens the device afresh, so it gets its own wave from phase
0. TCP backpressure paces it to the listener. On the listening side:

    ffplay -f u8 -ar 48000 -ch_layout mono -nodisp tcp://HOST:7671
"""
import shutil
import socketserver
import threading

SHAPES = ("sine", "sawtooth", "triangle", "square")
BASE = 7671


def handler_for(dev):
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            with open(dev, "rb", buffering=0) as f, \
                    self.request.makefile("wb") as out:
                try:
                    shutil.copyfileobj(f, out, 4096)
                except (BrokenPipeError, ConnectionResetError):
                    pass
    return Handler


socketserver.ThreadingTCPServer.allow_reuse_address = True
socketserver.ThreadingTCPServer.daemon_threads = True
for i, shape in enumerate(SHAPES):
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", BASE + i),
                                          handler_for("/dev/" + shape))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f"/dev/{shape} on :{BASE + i}", flush=True)
threading.Event().wait()
