#!/usr/bin/env python3
"""webtest_server.py: tests for the iPhone web-page idea.

Serves over HTTPS with a self-signed certificate, so an iPhone on the same Wi-Fi can open it in Safari.

  https://<pc-ip>:47900/          test 1: camera + WebCodecs H.264 encoder support and speed
  https://<pc-ip>:47900/wstest    test 2: send encoded frames to this PC over wss:// and measure delay
  https://<pc-ip>:47900/webcam    test 3: the real page: speaks the MyCam protocol to a FAKE PC (this script)

For test 3 this script plays the PC companion: it sends the MCMD commands (HELLO, then START, KEYFRAME, PAUSE,
RESUME, STOP on a timer) and checks every MCAM packet the page sends: magic, reserved bytes, length, CONFIG
contents (SPS/PPS present), the first frame being a key frame, increasing timestamps.

The pages POST their results to /report; they are printed here and saved in ./results/.

Run:  python pc\\tools\\webtest\\webtest_server.py [--scenario basic|hold]
Needs Python 3.8+ and openssl (comes with Git for Windows). Nothing to pip install.
"""
import argparse
import base64
import datetime
import hashlib
import http.server
import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time

PORT = 47900
HERE = os.path.dirname(os.path.abspath(__file__))
CERT_DIR = os.path.join(HERE, "cert")
RESULTS_DIR = os.path.join(HERE, "results")
GOLDEN = os.path.normpath(os.path.join(HERE, "..", "..", "..", "protocol", "golden.txt"))
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PAGES = {"/": "index.html", "/index.html": "index.html",
         "/wstest": "wstest.html", "/wstest.html": "wstest.html",
         "/webcam": "webcam.html", "/webcam.html": "webcam.html"}

# MyCam protocol v3 (see protocol/PROTOCOL.md)
PKT_NAMES = {0: "HELLO", 1: "CONFIG", 2: "FRAME", 3: "ORIENT", 4: "STATE", 5: "LOG", 6: "CAMERA"}
STATE_NAMES = {0: "idle", 1: "streaming", 2: "error", 3: "paused"}
CMD_HELLO, CMD_START, CMD_STOP, CMD_KEYFRAME, CMD_PAUSE, CMD_RESUME = 1, 2, 3, 4, 6, 7

# (seconds after the page's HELLO, command, argument, label)
SCENARIOS = {
    "basic": [(1.5, CMD_START, 0, "START"), (6.0, CMD_KEYFRAME, 0, "KEYFRAME"), (9.0, CMD_PAUSE, 0, "PAUSE"),
              (12.0, CMD_RESUME, 0, "RESUME"), (16.0, CMD_STOP, 0, "STOP")],
    "hold": [(1.5, CMD_START, 0, "START")],  # stay streaming (turn the phone to test rotation)
}
SCENARIO = "basic"


def mcmd(cmd, arg=0):
    return b"MCMD" + bytes([cmd, arg, 0, 0])


def lan_ip():
    """The PC's address on the local network (no packets are actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def find_openssl():
    found = shutil.which("openssl")
    if found:
        return found
    for candidate in (r"C:\Program Files\Git\usr\bin\openssl.exe",
                      r"C:\Program Files (x86)\Git\usr\bin\openssl.exe"):
        if os.path.exists(candidate):
            return candidate
    return None


def ensure_cert(ip):
    """Make (or reuse) a self-signed certificate that is valid for this PC's IP address."""
    os.makedirs(CERT_DIR, exist_ok=True)
    cert = os.path.join(CERT_DIR, "cert.pem")
    key = os.path.join(CERT_DIR, "key.pem")
    stamp = os.path.join(CERT_DIR, "ip.txt")
    if os.path.exists(cert) and os.path.exists(key) and os.path.exists(stamp):
        with open(stamp) as f:
            if f.read().strip() == ip:
                return cert, key
    openssl = find_openssl()
    if not openssl:
        sys.exit("openssl not found. Install Git for Windows (it includes openssl) or add openssl to PATH.")
    print("making a self-signed certificate for %s ..." % ip)
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    cmd = [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
           "-keyout", key, "-out", cert, "-days", "30",
           "-subj", "/CN=MyCam web test",
           "-addext", "subjectAltName=IP:%s,DNS:localhost" % ip]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("openssl failed:\n" + result.stderr)
    with open(stamp, "w") as f:
        f.write(ip)
    return cert, key


# ---- minimal WebSocket (RFC 6455): enough for binary messages, ping/pong and close ----

def recv_exact(rfile, n):
    data = rfile.read(n)
    if data is None or len(data) < n:
        raise EOFError("connection closed")
    return data


def ws_read_frame(rfile):
    b1, b2 = recv_exact(rfile, 2)
    opcode = b1 & 0x0F
    length = b2 & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(rfile, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(rfile, 8))[0]
    if length > 9_000_000:
        raise ValueError("frame too big")
    mask = recv_exact(rfile, 4) if (b2 & 0x80) else None
    payload = recv_exact(rfile, length) if length else b""
    if mask and length:
        m = (mask * (length // 4 + 1))[:length]
        payload = (int.from_bytes(payload, "big") ^ int.from_bytes(m, "big")).to_bytes(length, "big")
    return opcode, payload


def ws_frame(opcode, payload=b""):
    n = len(payload)
    if n < 126:
        head = bytes([0x80 | opcode, n])
    elif n < 65536:
        head = bytes([0x80 | opcode, 126]) + struct.pack(">H", n)
    else:
        head = bytes([0x80 | opcode, 127]) + struct.pack(">Q", n)
    return head + payload


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(p * len(s)))]


def nal_types(data):
    """NAL unit types found in an Annex-B byte string (7 = SPS, 8 = PPS, 5 = IDR)."""
    types = []
    i, n = 0, len(data)
    while i + 3 < n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                types.append(data[i + 3] & 0x1F)
                i += 3
                continue
            if data[i + 2] == 0 and data[i + 3] == 1 and i + 4 < n:
                types.append(data[i + 4] & 0x1F)
                i += 4
                continue
        i += 1
    return types


def stamp_now():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "MyCamWebTest"

    def log_message(self, fmt, *args):
        print("%s  %s  %s" % (stamp_now(), self.client_address[0], fmt % args))

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ws":
            self._websocket_echo()
        elif path == "/webcam-ws":
            self._websocket_fake_pc()
        elif path == "/golden.txt":
            if os.path.exists(GOLDEN):
                with open(GOLDEN, "rb") as f:
                    self._send(200, f.read())
            else:
                self._send(404, "golden.txt not found at " + GOLDEN)
        elif path in PAGES:
            with open(os.path.join(HERE, PAGES[path]), "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        else:
            self._send(404, "not found")

    def do_POST(self):
        if self.path != "/report":
            self._send(404, "not found")
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(min(length, 1_000_000))
        try:
            report = json.loads(raw)
        except ValueError:
            self._send(400, "bad json")
            return
        os.makedirs(RESULTS_DIR, exist_ok=True)
        path = os.path.join(RESULTS_DIR, time.strftime("results-%Y%m%d-%H%M%S.json"))
        with open(path, "w") as f:
            json.dump(report, f, indent=2)
        print("\n===== results from the phone =====")
        print(json.dumps(report, indent=2))
        print("saved to %s\n" % path)
        self._send(200, "ok")

    def _ws_handshake(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key or self.headers.get("Upgrade", "").lower() != "websocket":
            self._send(400, "expected a websocket upgrade")
            return False
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        return True

    # ---- test 2: echo the 12-byte header back so the phone can time the round trip ----
    def _websocket_echo(self):
        if not self._ws_handshake():
            return
        print("%s  websocket (echo) open from %s" % (stamp_now(), self.client_address[0]))
        video_msgs = 0
        video_bytes = 0
        first = last = None
        gaps = []
        try:
            while True:
                opcode, payload = ws_read_frame(self.rfile)
                now = time.perf_counter()
                if opcode == 0x2:
                    if len(payload) >= 12:
                        self.wfile.write(ws_frame(0x2, payload[:12]))
                    if len(payload) > 64:
                        video_msgs += 1
                        video_bytes += len(payload)
                        if last is not None:
                            gaps.append((now - last) * 1000.0)
                        if first is None:
                            first = now
                        last = now
                elif opcode == 0x8:
                    self.wfile.write(ws_frame(0x8))
                    break
                elif opcode == 0x9:
                    self.wfile.write(ws_frame(0xA, payload))
        except (EOFError, ConnectionError, OSError, ssl.SSLError, ValueError):
            pass
        finally:
            if video_msgs and first is not None and last is not None and last > first:
                span = last - first
                print("%s  websocket closed. received %d video messages, %.1f MB, %.1f Mbps over %.1f s; "
                      "gap between messages avg %.1f ms, p95 %.1f ms, max %.1f ms" % (
                          stamp_now(), video_msgs, video_bytes / 1e6,
                          video_bytes * 8 / span / 1e6, span,
                          sum(gaps) / len(gaps), pct(gaps, 0.95), max(gaps)))
            else:
                print("%s  websocket closed" % stamp_now())

    # ---- test 3: play the PC companion for the real page ----
    def _websocket_fake_pc(self):
        if not self._ws_handshake():
            return
        lock = threading.Lock()
        done = threading.Event()
        st = {"hello": False, "config": False, "frames": 0, "keys": 0, "bytes": 0,
              "last_pts": None, "problems": 0, "t_hello": None}

        def say(text):
            print("%s  %s" % (stamp_now(), text), flush=True)

        def warn(text):
            st["problems"] += 1
            say("WARNING: " + text)

        def send_msg(data):
            with lock:
                self.wfile.write(ws_frame(0x2, data))

        def scenario():
            try:
                while not done.is_set() and not st["hello"]:
                    send_msg(mcmd(CMD_HELLO))
                    say("PC -> page HELLO")
                    done.wait(2.0)
                if done.is_set():
                    return
                t0 = time.time()
                for delay, cmd, arg, label in SCENARIOS[SCENARIO]:
                    remaining = t0 + delay - time.time()
                    if remaining > 0 and done.wait(remaining):
                        return
                    send_msg(mcmd(cmd, arg))
                    say("PC -> page %s arg=%d" % (label, arg))
            except (OSError, ssl.SSLError, ValueError):
                done.set()

        def on_packet(msg):
            if len(msg) < 20 or msg[:4] != b"MCAM":
                warn("not a MyCam packet (%d bytes, starts %s)" % (len(msg), msg[:4].hex()))
                return
            ptype, flags = msg[4], msg[5]
            pts = struct.unpack(">q", msg[8:16])[0]
            length = struct.unpack(">I", msg[16:20])[0]
            payload = msg[20:]
            if msg[6:8] != b"\0\0":
                warn("reserved bytes are not zero")
            if length != len(payload):
                warn("length field says %d but the payload is %d bytes" % (length, len(payload)))
            if ptype == 0:
                version = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else None
                st["hello"] = True
                st["t_hello"] = time.time()
                say("page -> PC HELLO version=%s%s" % (version, "" if version == 3 else "   <-- expected 3"))
            elif ptype == 1:
                if len(payload) < 7:
                    warn("CONFIG payload too short (%d bytes)" % len(payload))
                    return
                w, h, sensor = struct.unpack(">HHH", payload[:6])
                facing = payload[6]
                types = nal_types(payload[7:])
                ok = 7 in types and 8 in types
                st["config"] = True
                say("page -> PC CONFIG %dx%d sensor=%d facing=%d sps_pps=%dB nal_types=%s%s" % (
                    w, h, sensor, facing, len(payload) - 7, types, "" if ok else "   <-- SPS/PPS missing"))
            elif ptype == 2:
                key = bool(flags & 1)
                if flags & ~1:
                    warn("unexpected flag bits 0x%02x on FRAME" % flags)
                if not st["config"]:
                    warn("FRAME before any CONFIG")
                if st["frames"] == 0 and not key:
                    warn("the first FRAME is not a key frame")
                if st["last_pts"] is not None and pts <= st["last_pts"]:
                    warn("timestamp did not increase (%d after %d)" % (pts, st["last_pts"]))
                st["last_pts"] = pts
                st["frames"] += 1
                st["bytes"] += len(payload)
                st["keys"] += 1 if key else 0
                if key or st["frames"] % 30 == 1:
                    say("page -> PC FRAME %dB %s pts=%dus (%d frames so far)" % (
                        len(payload), "KEY" if key else "delta", pts, st["frames"]))
            elif ptype == 3:
                deg = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else None
                say("page -> PC ORIENT rotation=%s" % deg)
            elif ptype == 4:
                state = payload[0] if payload else None
                facing = payload[1] if len(payload) > 1 else None
                say("page -> PC STATE state=%s facing=%s" % (STATE_NAMES.get(state, state), facing))
            elif ptype == 5:
                say("page -> PC LOG %r" % payload.decode("utf-8", "replace"))
            elif ptype == 6:
                say("page -> PC CAMERA (%d bytes)" % len(payload))
            else:
                warn("unknown packet type %d" % ptype)

        say("websocket (fake PC) open from %s, scenario '%s'" % (self.client_address[0], SCENARIO))
        worker = threading.Thread(target=scenario, daemon=True)
        worker.start()
        try:
            while True:
                opcode, payload = ws_read_frame(self.rfile)
                if opcode == 0x2:
                    on_packet(payload)
                elif opcode == 0x8:
                    with lock:
                        self.wfile.write(ws_frame(0x8))
                    break
                elif opcode == 0x9:
                    with lock:
                        self.wfile.write(ws_frame(0xA, payload))
        except (EOFError, ConnectionError, OSError, ssl.SSLError, ValueError):
            pass
        finally:
            done.set()
            say("websocket closed. %d frames (%d key), %.1f MB, %d problem(s) found" % (
                st["frames"], st["keys"], st["bytes"] / 1e6, st["problems"]))


def main():
    global SCENARIO
    parser = argparse.ArgumentParser(description="MyCam iPhone web test server")
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="basic",
                        help="what the fake PC does on /webcam: basic = START, KEYFRAME, PAUSE, RESUME, STOP on a "
                             "timer; hold = START and keep streaming (for turning the phone)")
    args = parser.parse_args()
    SCENARIO = args.scenario

    ip = lan_ip()
    cert, key = ensure_cert(ip)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print("MyCam web test server")
    print("  On the iPhone (same Wi-Fi), open Safari and go to:")
    print("      https://%s:%d/          (test 1: encoder)" % (ip, PORT))
    print("      https://%s:%d/wstest    (test 2: send frames to the PC)" % (ip, PORT))
    print("      https://%s:%d/webcam    (test 3: the real page, fake PC, scenario '%s')" % (ip, PORT, SCENARIO))
    print("  Safari will warn the certificate is not private: that is expected.")
    if not os.path.exists(GOLDEN):
        print("  note: %s not found, so the page's self-test cannot read golden.txt" % GOLDEN)
    print("  Ctrl+C to quit.\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("bye")


if __name__ == "__main__":
    main()
