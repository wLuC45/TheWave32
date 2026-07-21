"""Per-session capture of every JSON line + outbound command.

The Python logger already writes a rotating diagnostic log
(~/.cache/thewave32/thewave32.log). That covers app/state/error
messages but does not preserve the firmware's actual JSON stream -
which is the part the user often wants to grep/replay later.

This module opens one append-only ``.log`` file per serial session,
named by the moment it started, and dumps every inbound JSON object
plus every outbound command with an ISO-8601 timestamp and a direction
marker (``<`` inbound, ``>`` outbound). Format is one record per line
so ``jq`` / ``grep`` work cleanly.
"""

from __future__ import annotations

import datetime as _dt
import json
import os
from pathlib import Path
from typing import Any, Optional


def default_dir() -> Path:
    env = os.environ.get("THEWAVE32_SESSION_LOG_DIR")
    if env:
        return Path(env)
    return Path.home() / ".cache" / "thewave32" / "sessions"


class SessionLog:
    """Open a fresh ``.log`` per session. Call ``open()`` when the serial
    worker comes up, ``record_in``/``record_out``/``record_event`` as
    traffic flows, ``close()`` on disconnect.

    The instance is cheap to keep around when closed (writes become
    no-ops) so callers don't need to worry about lifetime.
    """

    def __init__(self, base_dir: Optional[Path] = None) -> None:
        self._dir = base_dir or default_dir()
        self._fh = None
        self._path: Optional[Path] = None

    @property
    def path(self) -> Optional[Path]:
        return self._path

    def open(self, port: str = "?") -> Path:
        """Begin a new session log. Returns the resolved file path so
        callers can show it in the UI."""
        self.close()
        self._dir.mkdir(parents=True, exist_ok=True)
        ts = _dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
        safe_port = port.replace("/", "_").lstrip("_") or "unknown"
        self._path = self._dir / f"thewave32-{ts}-{safe_port}.log"
        self._fh = self._path.open("a", encoding="utf-8")
        self._write_meta("session_start", {"port": port})
        return self._path

    def close(self) -> None:
        if self._fh is not None:
            try:
                self._write_meta("session_end", {})
                self._fh.flush()
                self._fh.close()
            except Exception:
                pass
            self._fh = None

    def record_in(self, obj: dict[str, Any]) -> None:
        self._write_line("<", json.dumps(obj, ensure_ascii=False))

    def record_out(self, cmd: str) -> None:
        self._write_line(">", cmd)

    def record_raw(self, data: bytes) -> None:
        # Best-effort decode. Binary streams (PCAP, CSI) get ``[N bytes]``
        # so the log stays line-based.
        try:
            text = data.decode("utf-8").rstrip()
            if not text:
                return
            self._write_line("!", text)
        except UnicodeDecodeError:
            self._write_line("!", f"[{len(data)} bytes binary]")

    # --- private ------------------------------------------------------

    def _write_meta(self, kind: str, body: dict[str, Any]) -> None:
        self._write_line("*", json.dumps({"meta": kind, **body}))

    def _write_line(self, dirn: str, payload: str) -> None:
        if self._fh is None:
            return
        ts = _dt.datetime.now().isoformat(timespec="milliseconds")
        try:
            self._fh.write(f"{ts} {dirn} {payload}\n")
            self._fh.flush()
        except Exception:
            # Don't take down the GUI if disk is full / FS is RO.
            self._fh = None
