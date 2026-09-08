#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
#
# Local preview server for the YJRQZ Boot WebFlash page.
#
# It serves cmd/lwip/webflash/vendors/hlk-7621a/index.html + style.css and
# answers the /api/* routes with mocked JSON, so the whole UI can be reviewed
# in a browser without a device and without touching any SPI flash.

import argparse
import json
import struct
import sys
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ASSET_DIR = (Path(__file__).resolve().parent.parent
             / "cmd" / "lwip" / "webflash" / "vendors" / "hlk-7621a")

EEPROM_OFFSET = 0x8000
EEPROM_SIZE = 0x200
FACTORY_SIZE = 0x10000
DEFAULT_MAC = bytes.fromhex("f862aa50132a")


def sample_eeprom():
    """A plausible MT7612 EEPROM: template header + a valid unicast MAC."""
    data = bytearray(b"\xff" * EEPROM_SIZE)
    data[0x00:0x04] = bytes.fromhex("62760100")   # chip id 0x7662, version 1
    data[0x04:0x0a] = DEFAULT_MAC
    data[0x0a:0x0c] = bytes.fromhex("1276")       # PCI id
    data[0x22:0x24] = bytes.fromhex("0100")       # antenna config
    data[0x34:0x38] = bytes.fromhex("14003400")   # NIC conf 0/1
    data[0x38:0x3a] = bytes.fromhex("00ff")       # 5G / 2.4G region
    data[0x3a] = 0x00                             # freq offset
    data[0x42:0x44] = bytes.fromhex("0000")       # NIC conf 2
    for i in range(0x56, 0x62):
        data[i] = 0x18                            # 2G tx power groups
    for i in range(0x62, 0x80):
        data[i] = 0x14                            # 5G tx power groups
    for i in range(0x80, 0x9e):
        data[i] = 0x16
    data[0x9e:0xa0] = bytes.fromhex("0000")       # xtal trim 2
    data[0xd0:0xd3] = bytes.fromhex("3c003c")     # target power / temp
    data[0x1e0:0x1fc] = bytes.fromhex("00" * 28)  # usage map
    return bytes(data)


def load_eeprom(path):
    data = Path(path).read_bytes()
    if len(data) != EEPROM_SIZE:
        sys.exit(f"{path}: expected {EEPROM_SIZE} bytes, got {len(data)}")
    return data


def partition_table(flash_size):
    return [
        {"name": "uboot", "offset": 0x00000, "size": 0x30000, "protected": True},
        {"name": "environment", "offset": 0x30000, "size": 0x10000, "protected": True},
        {"name": "factory", "offset": 0x40000, "size": FACTORY_SIZE, "protected": True},
        {"name": "firmware", "offset": 0x50000, "size": flash_size - 0x50000,
         "protected": False},
    ]


class PreviewHandler(BaseHTTPRequestHandler):
    server_version = "YJRQZPreview/1.0"
    eeprom = sample_eeprom()
    flash_size = 32 * 1024 * 1024

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s\n" % (fmt % args))

    # ---------------------------------------------------------------- helpers
    def send_bytes(self, body, content_type, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, obj, status=200):
        self.send_bytes(json.dumps(obj).encode(), "application/json", status)

    def send_asset(self, name, content_type):
        path = ASSET_DIR / name
        if not path.is_file():
            self.send_json({"ok": False, "error": f"asset not found: {name}"}, 404)
            return
        self.send_bytes(path.read_bytes(), content_type)

    # ------------------------------------------------------------------- HTTP
    def do_GET(self):
        path = self.path.split("?", 1)[0]

        if path in ("/", "/index.html"):
            return self.send_asset("index.html", "text/html; charset=utf-8")
        if path == "/style.css":
            return self.send_asset("style.css", "text/css; charset=utf-8")
        if path == "/api/info":
            return self.send_json({
                "product": "YJRQZ Boot",
                "version": "U-Boot 2026.07 (preview)",
                "device": "hilink,hlk-7621a-evb",
                "ip": "192.168.31.66",
                "load_address": "0x80001000",
                "max_upload": 0x1fb0000,
                "flash_size": self.flash_size,
                "erase_size": 0x1000,
                "partitions": partition_table(self.flash_size),
            })
        if path == "/api/eeprom":
            return self.send_json({"ok": True, "size": EEPROM_SIZE,
                                   "hex": self.eeprom.hex()})
        if path == "/api/job":
            return self.send_json({"ok": True, "active": False, "phase": "idle",
                                   "progress": 0, "processed": 0, "total": 0,
                                   "target": "", "error": "",
                                   "reboot_pending": False})
        if path == "/api/env":
            return self.send_json({"ipaddr": "192.168.31.66",
                                   "netmask": "255.255.255.0",
                                   "serverip": "192.168.31.100",
                                   "bootdelay": "3"})
        if path == "/api/backup/eeprom":
            return self.send_bytes(self.eeprom, "application/octet-stream")
        if path.startswith("/api/backup/"):
            name = path.rsplit("/", 1)[-1]
            return self.send_bytes(b"\xff" * 4096, "application/octet-stream")

        self.send_json({"ok": False, "error": "not found"}, 404)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""

        if path == "/api/upload/eeprom":
            detail = "512-byte MT7612/MT7662 EEPROM verified"
            if len(body) != EEPROM_SIZE:
                return self.send_json({
                    "ok": False, "target": "eeprom", "size": len(body),
                    "crc32": "00000000",
                    "detail": f"expected {EEPROM_SIZE} bytes"}, 422)
            return self.send_json({"ok": True, "target": "eeprom",
                                   "size": len(body), "crc32": "5a17c0de",
                                   "detail": detail})
        if path.startswith("/api/upload/"):
            target = path.rsplit("/", 1)[-1]
            return self.send_json({"ok": True, "target": target,
                                   "size": len(body), "crc32": "1badc0de",
                                   "detail": f"mock {target} upload accepted"})
        if path == "/api/flash":
            return self.send_json({"ok": True, "target": "eeprom",
                                   "phase": "erasing"}, 202)
        if path == "/api/env":
            return self.send_json({"ok": True,
                                   "message": "environment saved (preview)"})
        if path == "/api/env/reset":
            return self.send_json({"ok": True,
                                   "message": "default boot environment restored (preview)"})
        if path == "/api/factory-reset":
            return self.send_json({"ok": True, "target": "firmware",
                                   "phase": "erasing"}, 202)
        if path == "/api/reboot":
            return self.send_json({"ok": True, "message": "device will reboot shortly (preview)"})

        self.send_json({"ok": False, "error": "not found"}, 404)


def main():
    parser = argparse.ArgumentParser(
        description="Preview the YJRQZ Boot WebFlash page with mocked APIs.")
    parser.add_argument("-p", "--port", type=int, default=8000,
                        help="listen port (default: 8000)")
    parser.add_argument("-b", "--bind", default="127.0.0.1",
                        help="listen address (default: 127.0.0.1)")
    parser.add_argument("-e", "--eeprom", metavar="FILE",
                        help="preview a real 512-byte eeprom.bin instead of the sample")
    parser.add_argument("--no-browser", action="store_true",
                        help="do not open a browser automatically")
    args = parser.parse_args()

    if not (ASSET_DIR / "index.html").is_file():
        sys.exit(f"page not found: {ASSET_DIR / 'index.html'}")
    if args.eeprom:
        PreviewHandler.eeprom = load_eeprom(args.eeprom)

    url = f"http://{args.bind}:{args.port}/"
    server = ThreadingHTTPServer((args.bind, args.port), PreviewHandler)
    print(f"YJRQZ Boot WebFlash preview: {url}")
    print(f"assets : {ASSET_DIR}")
    print("apis   : /api/info /api/eeprom /api/job /api/env (mocked, no flash access)")
    print("stop   : Ctrl-C")
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
