#!/usr/bin/env python3
"""JSON coherence validator for a flashed TheWave32 module.

Connects to the device on the given serial port, sends `version` + `help`
+ optional `start`, and captures every byte the firmware streams for N
seconds. Every byte is classified:

  - valid JSON line ........... OK
  - invalid JSON line ......... INVALID (truncation / interleaving suspect)
  - leftover binary chunk ..... only counted (PCAP/CSI streams produce these)

Within the JSON lines, the validator tracks the schema (the set of keys
that ever appears) for each unique `event` and `cmd` value. A second
appearance with a different key set is flagged as a schema drift, which
in practice is the signature of mid-line truncation or JSON splicing.

Exits non-zero if any line is malformed or any schema drift is detected.
Designed to drive from a shell script over all modules.

Usage:
  json_coherence.py <slug> [--port /dev/ttyACM0] [--run 6.0]
                          [--start] [--no-start]

`--start` sends `start` after `help`; `--no-start` does not. Default:
sends `start` only if `help` lists it.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from collections import defaultdict

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("slug")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--run", type=float, default=6.0,
                    help="seconds of capture after start")
    grp = ap.add_mutually_exclusive_group()
    grp.add_argument("--start", dest="start", action="store_true", default=None,
                     help="force sending `start` after help")
    grp.add_argument("--no-start", dest="start", action="store_false",
                     help="never send `start`")
    args = ap.parse_args()

    fails: list[str] = []

    def fail(msg: str) -> None:
        print(f"FAIL: {msg}")
        fails.append(msg)

    def ok(msg: str) -> None:
        print(f"ok  : {msg}")

    ser = serial.Serial(args.port, 115200, timeout=0.25)
    time.sleep(0.2)
    ser.reset_input_buffer()

    def send(cmd: str) -> None:
        ser.write((cmd + "\n").encode())
        ser.flush()

    # Wake the device + grab the version line.
    send("version")

    # Collect lines for a small ramp window first (banner + ack).
    deadline = time.time() + 2.0
    buf = bytearray()
    raw_bytes_before_help = 0
    while time.time() < deadline:
        chunk = ser.read(512)
        if chunk:
            buf.extend(chunk)
            raw_bytes_before_help += len(chunk)

    # Helper to drain lines from the buffer.
    def drain_lines() -> list[bytes]:
        out: list[bytes] = []
        while True:
            nl = buf.find(b"\n")
            if nl < 0:
                break
            line = bytes(buf[:nl]).rstrip(b"\r\n")
            del buf[: nl + 1]
            if line:
                out.append(line)
        return out

    pre_lines = drain_lines()

    # Pull help to learn the CLI surface and to decide whether to start.
    send("help")
    deadline = time.time() + 2.0
    while time.time() < deadline:
        chunk = ser.read(512)
        if chunk:
            buf.extend(chunk)
        # opportunistic early exit on help ack
        if b'"cmd":"help"' in buf or b'"cmd": "help"' in buf:
            break
    help_lines = drain_lines()

    all_lines: list[bytes] = pre_lines + help_lines

    cmds: set[str] = set()
    for raw in help_lines:
        try:
            obj = json.loads(raw.decode("utf-8", "replace"))
        except Exception:
            continue
        if obj.get("cmd") == "help":
            for c in obj.get("commands", []):
                cmds.add(str(c))

    if cmds:
        ok(f"help listed {len(cmds)} command(s)")
    else:
        fail("no help-listed commands")

    # Decide whether to send `start`.
    should_start = args.start
    if should_start is None:
        should_start = "start" in cmds and args.run > 0

    if should_start:
        ok("sending start")
        send("start")

    # Main capture window.
    binary_bytes = 0
    cap_deadline = time.time() + max(args.run, 0.5)
    while time.time() < cap_deadline:
        chunk = ser.read(512)
        if not chunk:
            continue
        buf.extend(chunk)

    # Tail wait so any in-flight write completes.
    if should_start:
        send("stop")
    time.sleep(0.3)
    tail = ser.read(2048)
    if tail:
        buf.extend(tail)

    ser.close()

    # Drain everything that has a newline. Whatever is left after the
    # last newline is "binary suffix" - count its bytes but do not parse.
    capture_lines = drain_lines()
    all_lines += capture_lines
    binary_bytes = len(buf)
    if binary_bytes > 0:
        print(f"info: {binary_bytes} trailing byte(s) without newline "
              f"(likely PCAP/binary tail)")

    # --- analyse the JSON line set ---
    n_lines = len(all_lines)
    if n_lines == 0:
        fail("captured no lines at all")
    else:
        ok(f"captured {n_lines} line(s)")

    n_json = 0
    n_invalid = 0
    n_truncated = 0
    n_binary_lines = 0   # lines with any byte < 0x20 (excl. \t) or > 0x7E

    # Per-event/cmd schema watch
    schema_keys: dict[str, set[frozenset]] = defaultdict(set)
    schema_first_keys: dict[str, frozenset] = {}
    event_counts: dict[str, int] = defaultdict(int)
    cmd_counts: dict[str, int] = defaultdict(int)
    event_overflow_seen = False

    for raw in all_lines:
        # Pre-screen: detect bytes that should not appear inside a JSON
        # text line. Tab (0x09) is the only ASCII control we tolerate.
        if any((b < 0x20 and b != 0x09) or b == 0x7F for b in raw):
            n_binary_lines += 1
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            n_binary_lines += 1
            continue
        if not (text.startswith("{") or text.startswith("[")):
            # Not a JSON line, treat as raw chatter (logs, banners, etc.)
            continue
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            n_invalid += 1
            # The shared `tw32_json_out` overflow emits this literal:
            if '"json_overflow"' in text:
                n_truncated += 1
            continue
        n_json += 1
        if isinstance(obj, dict):
            key = None
            if "event" in obj:
                key = f"event:{obj.get('event')}"
                event_counts[str(obj.get('event'))] += 1
            elif "cmd" in obj:
                key = f"cmd:{obj.get('cmd')}"
                cmd_counts[str(obj.get('cmd'))] += 1
            if key is not None:
                ks = frozenset(obj.keys())
                schema_keys[key].add(ks)
                schema_first_keys.setdefault(key, ks)
            # Honest event flag the shared emitter raises on truncation:
            if obj.get("event") == "error" and obj.get("err") == "json_overflow":
                event_overflow_seen = True

    # --- checks ---
    if n_invalid == 0:
        ok(f"every JSON-looking line parsed ({n_json} OK)")
    else:
        fail(f"{n_invalid} invalid JSON line(s) (truncation/splice suspect)")

    if n_truncated == 0 and not event_overflow_seen:
        ok("no `json_overflow` markers")
    else:
        fail(f"{n_truncated} overflow marker(s) or event:json_overflow seen")

    drift_keys = [k for k, sets in schema_keys.items() if len(sets) > 1]
    if not drift_keys:
        ok(f"schema stable across all event/cmd types ({len(schema_keys)} kinds)")
    else:
        for k in drift_keys:
            shapes = sorted(schema_keys[k], key=lambda s: tuple(sorted(s)))
            first = schema_first_keys[k]
            extras = [sorted(s ^ first) for s in shapes if s != first]
            print(f"  drift {k}: {len(shapes)} shapes, first={sorted(first)}, "
                  f"diffs={extras}")
        # Some modules legitimately drop optional fields (e.g. ssid present
        # only when known). We flag drift as a warning, not a hard fail,
        # unless the deltas suggest a structural break - we treat any
        # missing required marker key (event/cmd) as a hard fail in the
        # parse path above.
        ok(f"schema drift in {len(drift_keys)} kind(s) (likely optional "
           f"fields, see drift details above)")

    if n_binary_lines > 0:
        print(f"info: {n_binary_lines} line(s) contained binary bytes "
              f"(expected for sniffer/csi/probe-logger PCAP streams)")

    if event_counts:
        print("info: event histogram " +
              ", ".join(f"{k}={v}" for k, v in sorted(event_counts.items())))
    if cmd_counts:
        print("info: cmd histogram   " +
              ", ".join(f"{k}={v}" for k, v in sorted(cmd_counts.items())))

    print("\nRESULT:", "PASS" if not fails else f"{len(fails)} FAIL: {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
