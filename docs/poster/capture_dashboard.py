"""High-resolution dashboard screenshots via headless Edge (Chrome DevTools Protocol).

    python capture_dashboard.py [lan|public|admin|login]

Starts Edge headless with remote debugging, loads each page at a desktop or phone
viewport (device scale factor 2 / 3 for print), waits for Streamlit to draw, and
saves PNGs to screens/. Standard library only (a minimal WebSocket client).
"""
import base64
import json
import os
import socket
import struct
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "screens")
PORT = 9333
CRLF = "\r\n"
# Persistent profile for the admin site: `login` opens it in a visible window so the
# user signs in themselves; `admin` then reuses its cookie headless.
ADMIN_PROFILE = os.path.join(os.path.expanduser("~"), ".smart-garden-capture-profile")

SITES = {
    # site: (base URL, profile dir or None for a throwaway one, shots)
    # shot: name, path, width, height, scale, mobile, wait_s, clicks (button texts to press)
    "lan": ("http://192.168.4.30:8501", None, [
        ("desktop_home", "/", 1440, 1000, 2, False, 14, []),
        ("desktop_trends", "/trends", 1440, 1900, 2, False, 22, ["6 h"]),
        ("desktop_stage", "/stage", 1440, 1300, 2, False, 16, []),
        ("phone_home", "/", 390, 1500, 3, True, 14, []),
        ("phone_trends", "/trends", 390, 2400, 3, True, 22, ["6 h"]),
    ]),
    "public": ("https://smartgarden.tech", None, [
        ("public_desktop_home", "/", 1440, 1000, 2, False, 16, []),
        ("public_desktop_trends", "/trends", 1440, 1500, 2, False, 24, ["6 h"]),
        ("public_desktop_stage", "/stage", 1440, 1200, 2, False, 18, []),
        ("public_desktop_camera", "/camera", 1440, 2200, 2, False, 30, []),
        ("public_phone_home", "/", 390, 1400, 3, True, 16, []),
        ("public_phone_trends", "/trends", 390, 1800, 3, True, 24, ["6 h"]),
    ]),
    "admin": ("https://admin.smartgarden.tech", ADMIN_PROFILE, [
        ("admin_desktop_home", "/", 1440, 1000, 2, False, 16, []),
        ("admin_desktop_settings", "/settings", 1440, 1400, 2, False, 18, []),
        ("admin_phone_settings", "/settings", 390, 1600, 3, True, 18, []),
    ]),
}
BASE, PROFILE, SHOTS = SITES["lan"]


class Ws:
    """Just enough RFC 6455 for CDP: masked text frames out, (fragmented) frames in."""

    def __init__(self, url):
        u = urllib.parse.urlparse(url)
        self.s = socket.create_connection((u.hostname, u.port), timeout=120)
        key = base64.b64encode(os.urandom(16)).decode()
        lines = [f"GET {u.path} HTTP/1.1", f"Host: {u.hostname}:{u.port}", "Upgrade: websocket",
                 "Connection: Upgrade", f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
        self.s.sendall((CRLF.join(lines) + CRLF + CRLF).encode())
        buf = b""
        end = (CRLF + CRLF).encode()
        while end not in buf:
            buf += self.s.recv(4096)
        self.buf = buf.split(end, 1)[1]

    def send(self, text):
        data = text.encode()
        head = bytearray([0x81])
        n = len(data)
        if n < 126:
            head.append(0x80 | n)
        elif n < 65536:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        mask = os.urandom(4)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.s.sendall(bytes(head) + mask + body)

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(1 << 20)
            if not chunk:
                raise ConnectionError("websocket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        parts = []
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            payload = self._read(n)
            if (b0 & 0x0F) in (0x9, 0xA):  # ping / pong
                continue
            parts.append(payload)
            if b0 & 0x80:
                return b"".join(parts).decode("utf-8", "replace")

    def close(self):
        self.s.close()


class Cdp:
    def __init__(self, ws):
        self.ws, self.n = ws, 0

    def call(self, method, **params):
        self.n += 1
        my = self.n
        self.ws.send(json.dumps({"id": my, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == my:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})


def shoot(cdp, name, path, w, h, scale, mobile, wait_s, clicks):
    cdp.call("Emulation.setDeviceMetricsOverride", width=w, height=h,
             deviceScaleFactor=scale, mobile=mobile)
    if mobile:
        cdp.call("Emulation.setUserAgentOverride",
                 userAgent="Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 "
                           "(KHTML, like Gecko) Chrome/126.0 Mobile Safari/537.36")
    cdp.call("Page.navigate", url=BASE + path)
    time.sleep(wait_s)
    for text in clicks:
        js = ("(() => { const b = [...document.querySelectorAll('button, label, p, div')]"
              f".find(e => e.innerText && e.innerText.trim() === {json.dumps(text)});"
              " if (b) { b.click(); return true; } return false; })()")
        cdp.call("Runtime.evaluate", expression=js)
        time.sleep(10)
    # hide Streamlit's own toolbar and header for a cleaner picture
    cdp.call("Runtime.evaluate", expression=(
        "document.querySelectorAll('header, [data-testid=\"stToolbar\"], "
        "[data-testid=\"stDecoration\"], [data-testid=\"stStatusWidget\"]')"
        ".forEach(e => e.style.display = 'none')"))
    time.sleep(1.5)
    shot = cdp.call("Page.captureScreenshot", format="png")
    out = os.path.join(OUT, f"{name}_raw.png")
    with open(out, "wb") as f:
        f.write(base64.b64decode(shot["data"]))
    print("saved", out)


def login():
    """Open the admin site in a visible window on the capture profile; the user signs in."""
    os.makedirs(ADMIN_PROFILE, exist_ok=True)
    print("Sign in to the admin site in the Edge window, then close that window.")
    subprocess.run([EDGE, f"--user-data-dir={ADMIN_PROFILE}", "--new-window",
                    SITES["admin"][0] + "/login"])


def main():
    os.makedirs(OUT, exist_ok=True)
    prof = PROFILE or tempfile.mkdtemp(prefix="edge-cdp-")
    proc = subprocess.Popen([EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                             f"--remote-debugging-port={PORT}", f"--user-data-dir={prof}",
                             "--window-size=1440,1000", "about:blank"])
    try:
        page = None
        for _ in range(60):
            try:
                pages = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json").read())
                page = next(p for p in pages if p.get("type") == "page")
                break
            except Exception:
                time.sleep(0.5)
        ws = Ws(page["webSocketDebuggerUrl"])
        cdp = Cdp(ws)
        cdp.call("Page.enable")
        for s in SHOTS:
            shoot(cdp, *s)
        ws.close()
    finally:
        proc.terminate()


if __name__ == "__main__":
    import sys
    site = sys.argv[1] if len(sys.argv) > 1 else "lan"
    if site == "login":
        login()
    else:
        BASE, PROFILE, SHOTS = SITES[site]
        main()
