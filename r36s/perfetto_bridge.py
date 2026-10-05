#!/usr/bin/env python3
"""perfetto_bridge -- ui.perfetto.dev "Record new trace" for the R36S build.

Click Start in the Perfetto UI and get the game's timeline (xtrace, docs/02
section 12) from the device:

  UI --ws://127.0.0.1:8037/traced--> tracebox websocket_bridge
     --/tmp/perfetto-consumer--> THIS (a fake traced: the Consumer IPC subset
     the Record page uses) --ssh--> R36S: kill -USR2 nfsu2_recomp (start),
     kill -USR2 again (stop: the game writes RECOMP_TRACE), scp it back,
     stream its packets to the UI.

The protocol part follows cortrace/scripts/perfetto_record_bridge.py
(cortrace docs/03): IPCFrame framing [u32 LE size][IPCFrame], BindService ->
method table, EnableTracingResponse deferred until the trace is over,
ReadBuffers streamed in replies under the 128 KiB IPC frame cap.

Usage (the game running on the device with RECOMP_TRACE set and
RECOMP_TRACE_SECS=0, as r36s/Need for Speed Underground 2.sh's env.txt does):

  r36s/perfetto_bridge.py                 # starts tracebox websocket_bridge too
  # ui.perfetto.dev -> Record new trace -> Linux -> WebSocket -> Start tracing
  #   "Buffers and duration": the recording length (Stop ends it early)

  r36s/perfetto_bridge.py --local         # the game on this machine instead

Options: --host ark@192.168.3.71 (password: $R36S_PASS, default "ark", AGENT.md),
--remote-trace /roms/ports/nfs8/nfsu2.pftrace, --keep DIR (keep every trace).
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

CONSUMER_SOCK = "/tmp/perfetto-consumer"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IPC_MAX_PAYLOAD = 96 * 1024          # kIPCBufferSize is 128 KiB per frame
DEFAULT_SECS = 10

# ---- protobuf wire helpers (varint / length-delimited only) ---------------


def _varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def _fv(field, value):
    return _varint((field << 3) | 0) + _varint(value)


def _fb(field, data):
    return _varint((field << 3) | 2) + _varint(len(data)) + data


def _fs(field, s):
    return _fb(field, s.encode())


def _read_varint(buf, pos):
    shift = result = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def parse_fields(buf):
    """(field, wire type, int or bytes) for each field of a message."""
    pos, n = 0, len(buf)
    while pos < n:
        key, pos = _read_varint(buf, pos)
        field, wt = key >> 3, key & 7
        if wt == 0:
            val, pos = _read_varint(buf, pos)
        elif wt == 2:
            ln, pos = _read_varint(buf, pos)
            val, pos = buf[pos:pos + ln], pos + ln
        elif wt == 5:
            val, pos = buf[pos:pos + 4], pos + 4
        elif wt == 1:
            val, pos = buf[pos:pos + 8], pos + 8
        else:
            raise ValueError(f"wire type {wt}")
        yield field, wt, val


def field(buf, num, default=None):
    for f, _, v in parse_fields(buf):
        if f == num:
            return v
    return default


# ---- ConsumerPort ---------------------------------------------------------

METHODS = [(1, "EnableTracing"), (2, "DisableTracing"), (3, "ReadBuffers"),
           (4, "FreeBuffers"), (5, "Flush"), (6, "StartTracing"),
           (7, "ChangeTraceConfig"), (8, "GetTraceStats"), (9, "ObserveEvents"),
           (10, "QueryServiceState"), (11, "QueryCapabilities")]
METHOD_BY_ID = dict(METHODS)


def bind_reply(request_id):
    methods = b"".join(_fb(3, _fv(1, mid) + _fs(2, name)) for mid, name in METHODS)
    reply = _fv(1, 1) + _fv(2, 1) + methods          # success, service_id 1
    return _fv(2, request_id) + _fb(4, reply)         # msg_bind_service_reply


def invoke_reply(request_id, payload=b"", has_more=False, success=True):
    inner = _fv(1, 1 if success else 0)
    if has_more:
        inner += _fv(2, 1)
    if payload:
        inner += _fb(3, payload)
    return _fv(2, request_id) + _fb(6, inner)         # msg_invoke_method_reply


def service_state():
    # TracingServiceState: num_sessions 3, num_sessions_started 4,
    # tracing_service_version 5, supports_tracing_sessions 7
    state = _fv(3, 0) + _fv(4, 0) + _fs(5, "nfsu2 xtrace bridge (R36S)") + _fv(7, 1)
    return _fb(1, state)


def trace_packets(data):
    """A trace file is Trace{repeated TracePacket packet = 1}: the raw packets."""
    return [v for f, wt, v in parse_fields(data) if f == 1 and wt == 2]


def readbuffers_chunks(packets):
    """ReadBuffersResponse{repeated Slice slices = 2}, Slice{data 1,
    last_slice_for_packet 2}, each payload under IPC_MAX_PAYLOAD."""
    last = _fv(2, 1)
    parts, size = [], 0
    for p in packets:
        s = _fb(2, _fb(1, p) + last)
        if parts and size + len(s) > IPC_MAX_PAYLOAD:
            yield b"".join(parts)
            parts, size = [], 0
        parts.append(s)
        size += len(s)
    if parts:
        yield b"".join(parts)


def send_frame(conn, frame):
    conn.sendall(struct.pack("<I", len(frame)) + frame)


# ---- the device (or this machine) -----------------------------------------


class Target:
    """Starts and stops a recording in the running game, fetches the file."""

    def __init__(self, a):
        self.local = a.local
        self.remote = a.remote_trace
        self.host = a.host
        self.binary = a.binary
        self.ctl = f"/tmp/nfsu2-bridge-ssh-{os.getuid()}"
        self.env = dict(os.environ, SSHPASS=os.environ.get("R36S_PASS", "ark"))

    def sh(self, cmd, timeout=20):
        if self.local:
            argv = ["sh", "-c", cmd]
        else:
            # One multiplexed connection: each call after the first is a few ms.
            argv = ["sshpass", "-e", "ssh", "-o", "StrictHostKeyChecking=no",
                    "-o", "ControlMaster=auto", "-o", f"ControlPath={self.ctl}",
                    "-o", "ControlPersist=600", "-o", "ConnectTimeout=5",
                    self.host, cmd]
        r = subprocess.run(argv, env=self.env, capture_output=True, text=True,
                           timeout=timeout, check=False)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def clear(self):
        """Remove the last trace: the game renames a complete new one there."""
        self.sh(f"rm -f {self.remote}")

    def ready(self):
        rc, out, _ = self.sh(f"test -f {self.remote} && echo yes")
        return rc == 0 and out == "yes"

    def signal(self):
        rc, out, err = self.sh(f"p=$(pidof {self.binary}) && kill -USR2 $p && echo $p")
        if rc != 0:
            raise RuntimeError(f"{self.binary} not running on {'this machine' if self.local else self.host} ({err})")
        return out

    def fetch(self, dst):
        if self.local:
            shutil.copyfile(self.remote, dst)
            return
        r = subprocess.run(["sshpass", "-e", "scp", "-q", "-o", "StrictHostKeyChecking=no",
                            "-o", f"ControlPath={self.ctl}", f"{self.host}:{self.remote}", dst],
                           env=self.env, timeout=120, check=False)
        if r.returncode != 0:
            raise RuntimeError("scp failed")

    def clocks(self):
        rc, out, _ = self.sh("echo $(($(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)/1000))MHz "
                             "$(($(cat /sys/class/thermal/thermal_zone0/temp)/1000))C")
        return out if rc == 0 else "?"


class Session:
    """One recording: started by EnableTracing, ended by its duration or
    DisableTracing; EnableTracingResponse goes out when the file is back."""

    def __init__(self, bridge, conn, request_id, secs):
        self.b, self.conn, self.req, self.secs = bridge, conn, request_id, secs
        self.stop_ev = threading.Event()
        self.done = threading.Event()
        self.packets = []
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        b = self.b
        try:
            b.target.clear()
            pid = b.target.signal()
            b.log(f"recording on pid {pid} for {self.secs:g} s ({b.target.clocks()})")
            self.stop_ev.wait(self.secs)
            b.log(f"stopping ({b.target.clocks()})")
            b.target.signal()
            deadline = time.time() + 60          # writing ~10 MB on the SD card
            while not b.target.ready():
                if time.time() > deadline:
                    raise RuntimeError(f"no new {b.target.remote} after 60 s")
                time.sleep(0.3)
            dst = os.path.join(b.keep, time.strftime("nfsu2-%Y%m%d-%H%M%S.pftrace"))
            b.target.fetch(dst)
            data = open(dst, "rb").read()
            self.packets = trace_packets(data)
            b.log(f"trace {len(data) / 1e6:.1f} MB, {len(self.packets)} packets -> {dst}")
        except Exception as e:  # noqa: BLE001 -- reported to the log, the UI gets an empty trace
            b.log(f"recording failed: {e}")
        finally:
            self.done.set()
            try:   # the deferred EnableTracingResponse{disabled = true}: "trace over"
                send_frame(self.conn, invoke_reply(self.req, _fv(1, 1)))
            except OSError:
                b.log("UI connection gone before the trace was ready")


class Bridge:
    def __init__(self, target, keep, verbose=True):
        self.target, self.keep, self.verbose = target, keep, verbose
        self.session = None
        self.lock = threading.Lock()

    def log(self, *a):
        if self.verbose:
            print("[bridge]", *a, file=sys.stderr, flush=True)

    def enable(self, conn, request_id, args):
        # EnableTracingRequest{trace_config 1}; TraceConfig.duration_ms = 3
        cfg = field(args, 1, b"")
        ms = field(cfg, 3, 0) if cfg else 0
        secs = ms / 1000.0 if ms else DEFAULT_SECS
        with self.lock:
            if self.session and not self.session.done.is_set():
                self.log("a recording is already running; ignoring this start")
                return
            self.session = Session(self, conn, request_id, secs)

    def dispatch(self, conn, request_id, name, args):
        s = self.session
        if name == "EnableTracing":
            self.enable(conn, request_id, args)          # replied when the trace is back
        elif name == "DisableTracing":
            if s:
                s.stop_ev.set()
            send_frame(conn, invoke_reply(request_id))
        elif name == "ReadBuffers":
            if s:
                s.done.wait()
            n = 0
            for chunk in readbuffers_chunks(s.packets if s else []):
                send_frame(conn, invoke_reply(request_id, chunk, has_more=True))
                n += 1
            send_frame(conn, invoke_reply(request_id))
            self.log(f"streamed {len(s.packets) if s else 0} packets in {n} replies")
        elif name == "QueryServiceState":
            send_frame(conn, invoke_reply(request_id, service_state()))
        else:
            send_frame(conn, invoke_reply(request_id))

    def handle(self, conn, frame):
        request_id, bind, invoke = 0, None, None
        for f, wt, v in parse_fields(frame):
            if f == 2 and wt == 0:
                request_id = v
            elif f == 3 and wt == 2:
                bind = v
            elif f == 5 and wt == 2:
                invoke = v
        if bind is not None:
            send_frame(conn, bind_reply(request_id))
        elif invoke is not None:
            # InvokeMethod{service_id 1, method_id 2, args_proto 3}
            name = METHOD_BY_ID.get(field(invoke, 2, 0), "?")
            if name not in ("QueryServiceState", "QueryCapabilities", "ObserveEvents", "GetTraceStats"):
                self.log(name)
            self.dispatch(conn, request_id, name, field(invoke, 3, b""))

    def serve(self, conn):
        buf = b""
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= 4:
                    size = struct.unpack("<I", buf[:4])[0]
                    if len(buf) < 4 + size:
                        break
                    frame, buf = buf[4:4 + size], buf[4 + size:]
                    self.handle(conn, frame)
        except OSError:
            pass
        finally:
            conn.close()


def start_websocket_bridge(tools):
    """tracebox websocket_bridge (ws:8037 <-> /tmp/perfetto-consumer), fetched
    once into build-r36s/tools (its launcher downloads the binary on first run)."""
    tb = os.path.join(tools, "tracebox")
    if not os.path.exists(tb):
        os.makedirs(tools, exist_ok=True)
        print("[bridge] fetching tracebox", file=sys.stderr)
        subprocess.run(["curl", "-sL", "-m", "300", "-o", tb, "https://get.perfetto.dev/tracebox"], check=True)
        os.chmod(tb, 0o755)
    return subprocess.Popen([sys.executable, tb, "websocket_bridge"])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="ark@192.168.3.71")
    ap.add_argument("--remote-trace", default="/roms/ports/nfs8/nfsu2.pftrace",
                    help="RECOMP_TRACE on the device (or here, with --local)")
    ap.add_argument("--binary", default="nfsu2_recomp", help="process name to signal")
    ap.add_argument("--local", action="store_true", help="the game runs on this machine")
    ap.add_argument("--keep", default=os.path.join(REPO, "build-r36s", "trace"),
                    help="where fetched traces are kept")
    ap.add_argument("--sock", default=CONSUMER_SOCK)
    ap.add_argument("--no-websocket-bridge", action="store_true",
                    help="tracebox websocket_bridge is already running")
    a = ap.parse_args(argv)
    if a.local and a.remote_trace.startswith("/roms/"):
        a.remote_trace = "/tmp/nfs.pftrace"
    os.makedirs(a.keep, exist_ok=True)

    bridge = Bridge(Target(a), a.keep)
    wsb = None if a.no_websocket_bridge else start_websocket_bridge(os.path.join(REPO, "build-r36s", "tools"))
    if os.path.exists(a.sock):
        os.unlink(a.sock)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(a.sock)
    srv.listen(4)
    print(f"[bridge] {a.sock} ready: ui.perfetto.dev -> Record new trace -> Linux -> "
          f"WebSocket -> Start tracing ({'local' if a.local else a.host}, {a.binary})",
          file=sys.stderr)
    try:
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=bridge.serve, args=(conn,), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        if os.path.exists(a.sock):
            os.unlink(a.sock)
        if wsb:
            wsb.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
