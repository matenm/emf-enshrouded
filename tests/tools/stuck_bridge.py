"""A stand-in for a stuck game (scripts/bridge_clients_test.ps1): it serves \\\\.\\pipe\\emf-bridge-<its pid>, writes
<folder>\\emf\\bridge.txt like EMF does, reads requests and never answers. Ends after <seconds> or when
<folder>\\emf\\bridge.stop appears.
    python stuck_bridge.py <folder> [seconds]"""

import _winapi
import os
import sys
import threading
import time

folder = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
pipe = r"\\.\pipe\emf-bridge-%d" % os.getpid()
emf = os.path.join(folder, "emf")
os.makedirs(emf, exist_ok=True)


def serve_one():
    h = _winapi.CreateNamedPipe(pipe, _winapi.PIPE_ACCESS_DUPLEX, _winapi.PIPE_WAIT, _winapi.PIPE_UNLIMITED_INSTANCES,
                                65536, 65536, 0, _winapi.NULL)
    try:
        _winapi.ConnectNamedPipe(h, False)
    except OSError:
        pass
    threading.Thread(target=serve_one, daemon=True).start()  # the next client gets a new instance
    try:
        while True:
            data, _ = _winapi.ReadFile(h, 65536)  # read and say nothing
            if not data:
                break
    except OSError:
        pass
    finally:
        _winapi.CloseHandle(h)


threading.Thread(target=serve_one, daemon=True).start()
time.sleep(0.2)
with open(os.path.join(emf, "bridge.txt"), "w", encoding="utf-8") as f:
    f.write("pipe=%s\npid=%d\n" % (pipe, os.getpid()))
end = time.monotonic() + seconds
stop = os.path.join(emf, "bridge.stop")
while time.monotonic() < end and not os.path.exists(stop):
    time.sleep(0.1)
for path in (os.path.join(emf, "bridge.txt"), stop):
    try:
        os.remove(path)
    except OSError:
        pass
