#!/usr/bin/env python3
"""Commission a Stikka printer node over its serial AT interface.

Standard library only (POSIX: Linux/macOS). Flow:
  1. open the port, ask you to hold BOOT for 10 s (config mode)
  2. send the settings as AT+ commands, wifi first, pairing code last
  3. wait for the node to pair with stikka-server, then print AT+status

Examples:
  scripts/commission.py --wifi-ssid Lab --wifi-password s3cret \\
      --server-url https://stikka.example.org --name lobby-printer \\
      --type zpl_network --set target.host=10.0.0.5 --label-width 100 --label-length 150
  scripts/commission.py --status                   # just show the node's settings
  scripts/commission.py --config site.json --loop  # commission a batch, one node at a time

--config takes an INI file (or a JSON object) of AT setting names (as in
AT+help) plus "type", e.g.

  [DEFAULT]                  ; applies to every node
  wifi.ssid = Lab
  wifi.password = s3cret
  server.url = https://stikka.example.org
  [lobby]                    ; pick with --profile lobby (optional if it is the only section)
  name = lobby-printer
  type = zpl_network
  target.host = 10.0.0.5

Command-line options override the file. Secrets can also come from the environment
(STIKKA_WIFI_PASSWORD, STIKKA_ADMIN_PASSWORD) to keep them out of `ps`.
The pairing code is one-time per node, so it is prompted for unless --pairing-code is given.
"""
import argparse
import configparser
import getpass
import glob
import json
import os
import re
import select
import sys
import termios
import time

OK_TIMEOUT = 8.0


class AtError(Exception):
    pass


class Node:
    def __init__(self, path: str, baud: int):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, f"B{baud}", None)
        if speed is None:
            raise SystemExit(f"unsupported baud rate {baud}")
        attrs[0] = 0                                   # iflag: raw
        attrs[1] = 0                                   # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                   # lflag: no echo/canonical
        attrs[4] = attrs[5] = speed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def close(self) -> None:
        os.close(self.fd)

    def _read(self, timeout: float) -> bytes:
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def lines(self, timeout: float):
        """Yield complete lines as they arrive until `timeout` seconds pass."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.buf += self._read(0.1)
            while b"\n" in self.buf or b"\r" in self.buf:
                m = re.search(rb"[\r\n]+", self.buf)
                line, self.buf = self.buf[:m.start()], self.buf[m.end():]
                if line.strip():
                    yield line.decode("utf-8", "replace").strip()

    def drain(self) -> None:
        self.buf = b""
        while self._read(0.05):
            pass

    def send(self, line: str) -> None:
        os.write(self.fd, line.encode() + b"\r")

    def command(self, line: str, timeout: float = OK_TIMEOUT, quiet: bool = False) -> list[str]:
        """Run one AT command; return its output lines. Raises AtError on ERROR."""
        self.drain()
        self.send(line)
        out: list[str] = []
        for text in self.lines(timeout):
            if text == "OK":
                return out
            if text.startswith("ERROR"):
                raise AtError(f"{line}: {text}")
            # The node echoes what it receives; skip that.
            if text == line or line.startswith(text) or text.endswith(line):
                continue
            out.append(text)
            if not quiet:
                print("   ", text)
        raise AtError(f"{line}: no OK within {timeout:.0f}s")


def find_port() -> str:
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")
                   + glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
                   + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/cu.SLAB_USBtoUART*"))
    if not ports:
        raise SystemExit("no serial port found; plug the node in or pass --port")
    if len(ports) > 1:
        print("several serial ports found:", ", ".join(ports), file=sys.stderr)
        raise SystemExit("pass --port to choose one")
    return ports[0]


def wait_for_config_mode(node: Node, timeout: float) -> None:
    print("Hold the BOOT button for 10 seconds until the status LED shows config mode...")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        node.drain()
        node.send("AT")
        for text in node.lines(1.0):
            if text == "OK":
                print("Config mode is active.")
                return
    raise SystemExit(f"node did not enter config mode within {timeout:.0f}s")


def quote(value: str) -> str:
    if '"' in value:
        raise SystemExit(f"values may not contain double quotes: {value!r}")
    return f'"{value}"'


# Every AT setting, mirrored from kSettings in src/at_interface.cpp (plus the
# three non-table commands type/webui/loglevel). (key, group, metavar, help)
# Each becomes a --<key-with-dashes> flag and a valid key in --config files.
SETTINGS = [
    ("wifi.ssid", "Wi-Fi", "NAME", "Wi-Fi network name"),
    ("wifi.password", "Wi-Fi", "PASSWORD", "Wi-Fi password (empty = open network; env STIKKA_WIFI_PASSWORD)"),
    ("server.url", "stikka-server", "URL", "stikka-server base URL, e.g. https://stikka.example.org"),
    ("server.code", "stikka-server", "CODE", "one-time pairing code (prompted for if omitted and server.url is set)"),
    ("name", "Identity", "NAME", "printer / node name (also the mDNS hostname)"),
    ("details", "Identity", "TEXT", "free-text details shown next to the printer, e.g. 'Front desk'"),
    ("location", "Identity", "auto|LAT,LON|''", "coordinates only: auto = Wi-Fi geolocation by the server, 'LAT,LON' = fixed, empty = none"),
    ("admin.password", "Identity", "PASSWORD", "web UI password, user 'admin' (empty = none; env STIKKA_ADMIN_PASSWORD)"),
    ("type", "Printer", "TYPE", "printer type: zpl_network | zpl_usb_host | ql_usb_host | seiko_usb_host (or 1-4)"),
    ("target.host", "Printer", "HOST", "network printer host (zpl_network only)"),
    ("target.port", "Printer", "PORT", "network printer port, 1-65535 (default 9100)"),
    ("dpi", "Printer", "DPI", "printer resolution in dpi, 72-1200"),
    ("label.width", "Label", "MM", "label width in mm, 1-1000"),
    ("label.length", "Label", "MM", "label length in mm, 0 = endless, max 5000"),
    ("zpl.compression", "ZPL", "enable|disable", "printer accepts :Z64:/:B64: graphics"),
    ("ql.printhead", "Brother QL", "720|1296", "head width in px"),
    ("ql.invalidate", "Brother QL", "BYTES", "invalidate bytes, 1-2000 (200; 400 on QL-8xx)"),
    ("ql.autocut", "Brother QL", "enable|disable", "auto cut"),
    ("ql.feed", "Brother QL", "DOTS", "feed margin in dots, 0-1000"),
    ("ql.rightmargin", "Brother QL", "DOTS", "right margin in dots, 0-1296 (0 = auto-center)"),
    ("seiko.maxdots", "Seiko SLP", "DOTS", "max dots per line, 1-2000"),
    ("seiko.density", "Seiko SLP", "0-255", "print density (0 = 100%%)"),
    ("seiko.speed", "Seiko SLP", "0-255", "speed (0 = draft, 2 = fine)"),
    ("uart.tx", "UART printer", "PIN", "printer UART TX pin, 0-48 (serial method)"),
    ("uart.rx", "UART printer", "PIN", "printer UART RX pin, 0-48 (serial method)"),
    ("uart.baud", "UART printer", "BAUD", "printer UART baud rate, 300-2000000 (serial method)"),
    ("status.interval", "Node", "SECONDS", "status push interval, 1-3600"),
    ("debug", "Node", "enable|disable", "serial log output after leaving config mode"),
    ("loglevel", "Node", "error|warn|info|debug", "log verbosity"),
    ("webui", "Node", "enable|disable", "the node's own web UI (default on)"),
]
KNOWN_KEYS = {k for k, *_ in SETTINGS}
# Flags that predate the generated ones keep working.
ALIASES = {"server.code": ["--pairing-code"]}


def flag_for(key: str) -> str:
    return "--" + key.replace(".", "-")


# Order matters: Wi-Fi first, type before its dependants (it can pre-fill the
# dpi), the server URL and the one-time pairing code last.
FIRST = ["wifi.ssid", "wifi.password", "type", "name", "details", "location"]
LAST = ["server.url", "server.code"]
TEXT_KEYS = {"wifi.ssid", "wifi.password", "name", "details", "location", "server.url", "server.code",
             "admin.password", "target.host"}


def ordered(settings: dict[str, str]) -> list[tuple[str, str]]:
    keys = [k for k in FIRST if k in settings]
    keys += [k for k in settings if k not in FIRST and k not in LAST]
    keys += [k for k in LAST if k in settings]
    return [(k, settings[k]) for k in keys]


def apply(node: Node, settings: dict[str, str], pair_timeout: float) -> bool:
    for key, value in ordered(settings):
        shown = "<hidden>" if "password" in key or key == "server.code" else value
        print(f"AT+{key} {shown}")
        arg = quote(value) if key in TEXT_KEYS else value
        node.command(f"AT+{key} {arg}", quiet=True)

    if "server.code" in settings:
        print("Waiting for the node to pair with the server...")
        end = time.monotonic() + pair_timeout
        status = ""
        while time.monotonic() < end:
            time.sleep(2)
            for line in node.command("AT+status", quiet=True):
                if line.startswith("Server:"):
                    status = line
            if "connected" in status and "disconnected" not in status:
                break
            if "enrolling:" in status or "not paired:" in status:
                print(" ", status)
                break
        print(" ", status or "no server status")
        return "paired as" in status and "disconnected" not in status
    return True


def load_config(path: str, profile: str | None) -> dict[str, str]:
    """Read settings from an INI file (.ini/.cfg/.conf) or a JSON object.

    INI: [DEFAULT] keys apply to every node; the section named by --profile
    (or the only section, if there is just one) is layered on top. Values are
    taken literally, with or without surrounding double quotes.
    """
    if not path.lower().endswith((".ini", ".cfg", ".conf")):
        with open(path) as f:
            return {k: str(v) for k, v in json.load(f).items()}
    cp = configparser.ConfigParser(interpolation=None)
    cp.optionxform = str  # keep key case as written
    if not cp.read(path):
        raise SystemExit(f"cannot read {path}")
    sections = cp.sections()
    if profile:
        if profile not in sections:
            raise SystemExit(f"{path} has no [{profile}] section (found: {', '.join(sections) or 'none'})")
    elif len(sections) == 1:
        profile = sections[0]
    elif len(sections) > 1:
        raise SystemExit(f"{path} has several sections ({', '.join(sections)}); choose one with --profile")
    items = dict(cp.defaults())
    if profile:
        items.update(cp.items(profile))
    return {k.strip(): v.strip().removeprefix('"').removesuffix('"') for k, v in items.items()}


def build_settings(args) -> dict[str, str]:
    s: dict[str, str] = {}
    if args.config:
        s.update(load_config(args.config, args.profile))
    for key in s:
        if key not in KNOWN_KEYS:
            raise SystemExit(f"unknown setting {key!r} in {args.config} (see --help for the list)")
    if os.environ.get("STIKKA_WIFI_PASSWORD") is not None:
        s["wifi.password"] = os.environ["STIKKA_WIFI_PASSWORD"]
    if os.environ.get("STIKKA_ADMIN_PASSWORD") is not None:
        s["admin.password"] = os.environ["STIKKA_ADMIN_PASSWORD"]
    for key in KNOWN_KEYS:
        value = getattr(args, "opt_" + key.replace(".", "_"))
        if value is not None:
            s[key] = value
    for item in args.set:
        if "=" not in item:
            raise SystemExit(f"--set expects key=value, got {item!r}")
        k, v = item.split("=", 1)
        s[k.strip()] = v
    return s


def main() -> int:
    p = argparse.ArgumentParser(
        description="Commission a Stikka printer node over its serial AT interface.",
        epilog="Settings not given are left untouched on the node. Boolean-ish values take enable|disable.",
    )
    conn = p.add_argument_group("connection")
    conn.add_argument("--port", help="serial port (default: the only /dev/ttyACM*/ttyUSB*/cu.usb* found)")
    conn.add_argument("--baud", type=int, default=115200)
    conn.add_argument("--config-timeout", type=float, default=120, help="seconds to wait for config mode (default 120)")
    conn.add_argument("--pair-timeout", type=float, default=45, help="seconds to wait for pairing (default 45)")
    src = p.add_argument_group("settings file")
    src.add_argument("--config", help="INI (.ini/.cfg/.conf) or JSON file of AT setting names to values")
    src.add_argument("--profile", help="section of the INI file to use on top of [DEFAULT]")
    groups: dict[str, argparse._ArgumentGroup] = {}
    for key, group, metavar, help_text in SETTINGS:
        g = groups.setdefault(group, p.add_argument_group(f"settings: {group}"))
        g.add_argument(flag_for(key), *ALIASES.get(key, []), dest="opt_" + key.replace(".", "_"), metavar=metavar, help=f"AT+{key}: {help_text}")
    other = p.add_argument_group("other")
    other.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="any AT setting by name, repeatable (the flags above are shortcuts)")
    other.add_argument("--status", action="store_true", help="only print AT+status and exit")
    other.add_argument("--reboot", action="store_true", help="AT+reboot at the end instead of AT+exit")
    other.add_argument("--loop", action="store_true", help="commission several nodes in a row")
    args = p.parse_args()

    base = build_settings(args)
    path = args.port or find_port()
    failures = 0
    while True:
        settings = dict(base)
        if not args.status and "server.url" in settings and "server.code" not in settings:
            code = input("Pairing code for this node (empty to skip): ").strip()
            if code:
                settings["server.code"] = code
        node = Node(path, args.baud)
        try:
            wait_for_config_mode(node, args.config_timeout)
            if args.status:
                node.command("AT+status")
                node.command("AT+exit", quiet=True)
            else:
                good = apply(node, settings, args.pair_timeout)
                print("Final status:")
                node.command("AT+status")
                node.command("AT+reboot" if args.reboot else "AT+exit", quiet=True)
                print("Done." if good else "Done, but the node did not pair.")
                failures += 0 if good else 1
        except AtError as e:
            print(f"error: {e}", file=sys.stderr)
            failures += 1
        finally:
            node.close()
        if not args.loop:
            break
        if input("Press Enter for the next node (q to quit): ").strip().lower().startswith("q"):
            break
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
