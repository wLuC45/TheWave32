#!/usr/bin/env python3
"""Generic hardware smoke test for a flashed TheWave32 module.

Opens the serial CDC line, reads the `ready` banner, walks the CLI
surface advertised by `help`, drives start/stop, samples events, and
confirms the device still answers afterwards (i.e. it did not crash,
hang, or wedge under the refactor). Module-agnostic on purpose: it only
relies on the shared protocol (JSON lines, `help`, `stats`, ack shape).

Usage: hw_smoke.py <slug> [--port /dev/ttyACM0] [--run 8]

Run with the IDF python env (it ships pyserial):
  ~/.espressif/python_env/idf5.4_py3.14_env/bin/python scripts/hw_smoke.py <slug>
"""
from __future__ import annotations

import argparse
import json
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("slug")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--run", type=float, default=8.0, help="seconds to sample events")
    args = ap.parse_args()

    fails: list[str] = []

    def check(name: str, cond: bool, extra: str = "") -> None:
        print(("ok  " if cond else "FAIL") + f": {name}" + (f"  {extra}" if extra else ""))
        if not cond:
            fails.append(name)

    ser = serial.Serial(args.port, 115200, timeout=0.3)
    time.sleep(0.2)
    ser.reset_input_buffer()

    def send(cmd: str) -> None:
        ser.write((cmd + "\n").encode())
        ser.flush()

    def wait(pred, timeout: float = 3.0):
        end = time.time() + timeout
        while time.time() < end:
            line = ser.readline()
            if not line:
                continue
            try:
                obj = json.loads(line.decode("utf-8", "replace").strip())
            except Exception:
                continue
            if pred(obj):
                return obj
        return None

    # The board hard-resets after flashing, so the banner may already have
    # scrolled past; provoke a fresh answer with `version` either way.
    send("version")
    v = wait(lambda o: o.get("cmd") == "version" or o.get("event") == "ready", timeout=4.0)
    check("responds + correct module", bool(v) and v.get("module") == args.slug, str(v))

    send("help")
    h = wait(lambda o: o.get("cmd") == "help")
    cmds = set(h.get("commands", [])) if h else set()
    check("help lists commands", bool(cmds), str(sorted(cmds)))

    if "stats" in cmds:
        send("stats")
        s = wait(lambda o: o.get("cmd") == "stats")
        check("stats ok", bool(s) and s.get("ok") is True)
        if s:
            bad = [k for k, val in s.items()
                   if k not in ("cmd", "ok") and isinstance(val, float)]
            check("stats has no float fields", not bad, f"floats={bad}")

    started = False
    if "start" in cmds:
        send("start")
        a = wait(lambda o: o.get("cmd") == "start")
        # Some modules need args for start (e.g. a BSSID). An ack with
        # ok:false is a valid response, not a crash; only no response fails.
        check("start responded", bool(a), str(a))
        started = bool(a) and a.get("ok") is True

    events = 0
    if started:
        end = time.time() + args.run
        while time.time() < end:
            line = ser.readline()
            if not line:
                continue
            try:
                o = json.loads(line.decode("utf-8", "replace").strip())
            except Exception:
                continue
            if "event" in o:
                events += 1
        print(f"info: sampled {events} event(s) in {args.run:.0f}s")

    if "stop" in cmds:
        send("stop")
        wait(lambda o: o.get("cmd") == "stop")

    # Liveness after the workout: a crash/hang would make this time out.
    send("version")
    alive = wait(lambda o: o.get("cmd") == "version" or o.get("event") == "ready", timeout=4.0)
    check("still alive after run", bool(alive))

    if "stats" in cmds:
        send("stats")
        s2 = wait(lambda o: o.get("cmd") == "stats")
        if s2:
            print("final stats:", {k: s2[k] for k in s2 if k not in ("cmd", "ok")})

    ser.close()
    print("\nRESULT:", "PASS" if not fails else f"{len(fails)} FAILED: {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
