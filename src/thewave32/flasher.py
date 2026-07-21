from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

from serial.tools import list_ports

from thewave32 import log as _log
from thewave32.errors import DeviceNotFoundError, Tw32Error, FlashError, WrongChipError

_logger = _log.get("flasher")

KNOWN_VID_PIDS: set[tuple[int, int]] = {
    (0x303A, 0x1001),  # Espressif native USB CDC
    (0x303A, 0x4001),  # Espressif USB JTAG
    (0x10C4, 0xEA60),  # CP210x
    (0x1A86, 0x7523),  # CH340 (older)
    (0x1A86, 0x55D3),  # CH343 single serial
    (0x1A86, 0x55D4),  # CH343
}


class AmbiguousPortError(Tw32Error):
    def __init__(self, ports: list[str]) -> None:
        self.ports = ports
        super().__init__(f"multiple candidate ports: {', '.join(ports)} (use --port)")


def find_ports() -> list:
    """Return ListPortInfo objects whose VID/PID match a known ESP USB-serial chip."""
    matches = [p for p in list_ports.comports() if (p.vid, p.pid) in KNOWN_VID_PIDS]
    _logger.debug("find_ports: %d match(es) — %s",
                  len(matches), [p.device for p in matches])
    return matches


def resolve_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    found = find_ports()
    if not found:
        raise DeviceNotFoundError()
    if len(found) > 1:
        raise AmbiguousPortError([p.device for p in found])
    return found[0].device


def _esptool_cmd() -> list[str]:
    return [sys.executable, "-m", "esptool"]


_CHIP_RE = re.compile(r"(?:Chip is|Chip type:|Detecting chip type\.\.\.)\s+([A-Za-z0-9-]+)")


def detect_chip(port: str) -> str:
    """Return a normalized chip id like 'esp32s3' or raise Tw32Error."""
    cmd = _esptool_cmd() + ["--port", port, "chip_id"]
    _logger.info("detect_chip: %s", port)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        _logger.warning("detect_chip rc=%d: %s", result.returncode,
                        (result.stderr or result.stdout).strip()[:200])
        raise Tw32Error(f"esptool chip_id failed: {result.stderr or result.stdout}")
    match = _CHIP_RE.search(result.stdout)
    if not match:
        raise Tw32Error(f"could not parse chip id from output: {result.stdout!r}")
    raw = match.group(1).lower().replace("-", "")
    _logger.info("detect_chip: %s → %s", port, raw)
    return raw


def require_chip(port: str, expected: str) -> None:
    detected = detect_chip(port)
    if detected != expected:
        raise WrongChipError(detected=detected, expected=expected)


def flash(
    *,
    port: str,
    baud: int,
    artifacts: Iterable[tuple[int, Path]],
    log_path: Path,
    chip: str | None = None,
) -> None:
    """Invoke esptool to write all artifacts; logs full output to log_path."""
    cmd = _esptool_cmd()
    if chip:
        cmd += ["--chip", chip]
    cmd += ["--port", port, "--baud", str(baud)]
    # esptool operations/reset modes use underscores; the hyphenated
    # aliases are not accepted by all esptool >=4.7 builds (e.g. the 4.11
    # that ships with IDF 5.4), so use the portable underscore form.
    cmd += ["--before", "default_reset", "--after", "hard_reset"]
    cmd += ["write_flash", "--flash_size", "detect"]
    for offset, path in artifacts:
        cmd += [hex(offset), str(path)]

    log_path.parent.mkdir(parents=True, exist_ok=True)
    _logger.info("flash: port=%s baud=%d chip=%s artifacts=%s",
                 port, baud, chip,
                 [(hex(o), p.name) for o, p in artifacts])
    result = subprocess.run(cmd, capture_output=True, text=True)
    log_path.write_text((result.stdout or "") + (result.stderr or ""))
    if result.returncode != 0:
        _logger.error("flash failed rc=%d (full log: %s)",
                      result.returncode, log_path)
        raise FlashError(
            returncode=result.returncode,
            stderr=result.stderr or result.stdout,
            log_path=str(log_path),
        )
    _logger.info("flash ok (log: %s)", log_path)
