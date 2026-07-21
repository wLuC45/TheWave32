from __future__ import annotations

import csv
import io
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable

from thewave32 import idf, log as _log
from thewave32.errors import BuildError
from thewave32.manifest import Input, Partition

_logger = _log.get("builder")


def _check_no_control_chars(inp: Input, value: str) -> None:
    """Reject control characters in user-supplied string/choice values.

    Even with CSV quoting in place, a newline or NUL in an NVS string is
    almost never intended and is the classic vector for slipping extra
    rows past the manifest. Quoting keeps the row count honest; this keeps
    the *value* honest. Printable text (incl. spaces) is allowed.
    """
    for ch in value:
        if ch == "\x00" or (ord(ch) < 0x20 and ch not in ()):
            raise BuildError(
                stage="nvs",
                stderr=(
                    f"input '{inp.key}': control character "
                    f"{ord(ch):#04x} not allowed in an NVS string value"
                ),
            )


def _nvs_partition_gen_path() -> Path:
    return idf.nvs_partition_gen()


def _spiffsgen_path() -> Path:
    return idf.spiffsgen()


def _encode_value(inp: Input, value: object) -> tuple[str, str]:
    """Return (encoding, csv_value) for a given input."""
    match inp.type:
        case "string":
            return "string", str(value)
        case "int":
            return "i32", str(int(value))  # type: ignore[arg-type]
        case "bool":
            return "u8", "1" if bool(value) else "0"
        case "choice":
            return "string", str(value)
        case _:
            raise BuildError(stage="nvs", stderr=f"unsupported nvs input type: {inp.type}")


def build_nvs(
    partition: Partition,
    inputs_with_values: Iterable[tuple[Input, object]],
    out: Path,
) -> None:
    """Generate an NVS partition image from input values.

    The temporary CSV is unlinked in a finally block — on success and
    on subprocess failure — so a long-lived TUI session does not leave
    breadcrumbs behind in /tmp.
    """
    # Build the CSV with the stdlib writer so any value containing a comma,
    # quote or newline is quoted/escaped instead of breaking the row layout.
    # Concatenating with an f-string (the previous approach) let a value
    # like "foo\nevil,data,string,bar" inject an extra NVS key.
    buf = io.StringIO()
    writer = csv.writer(buf, quoting=csv.QUOTE_MINIMAL, lineterminator="\n")
    writer.writerow(["key", "type", "encoding", "value"])
    n_rows = 0
    seen_namespaces: set[str] = set()
    for inp, value in inputs_with_values:
        if inp.target != "nvs":
            continue
        ns = inp.namespace
        assert ns is not None  # validated in manifest
        if ns not in seen_namespaces:
            writer.writerow([ns, "namespace", "", ""])
            seen_namespaces.add(ns)
        encoding, csv_value = _encode_value(inp, value)
        if encoding == "string":
            _check_no_control_chars(inp, csv_value)
        writer.writerow([inp.key, "data", encoding, csv_value])
        n_rows += 1

    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, encoding="utf-8") as f:
        f.write(buf.getvalue())
        csv_path = Path(f.name)

    try:
        cmd = [
            sys.executable,
            str(_nvs_partition_gen_path()),
            "generate",
            str(csv_path),
            str(out),
            hex(partition.size),
        ]
        _logger.info("build_nvs: rows=%d size=%s out=%s",
                     n_rows, hex(partition.size), out.name)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            _logger.error("build_nvs failed: %s",
                          (result.stderr or result.stdout).strip()[:200])
            raise BuildError(stage="nvs", stderr=result.stderr or result.stdout)
    finally:
        csv_path.unlink(missing_ok=True)


def build_spiffs(
    partition: Partition,
    inputs_with_values: Iterable[tuple[Input, object]],
    out: Path,
) -> None:
    """Generate a SPIFFS partition image from input values.

    Staging directory is removed in finally on every exit path.
    """
    staging = Path(tempfile.mkdtemp(prefix="thewave32-spiffs-"))
    try:
        for inp, value in inputs_with_values:
            if inp.target != "spiffs":
                continue
            assert inp.dest is not None
            target = staging / inp.dest.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            if inp.type == "file":
                src = Path(str(value))
                if not src.is_file():
                    raise BuildError(stage="spiffs", stderr=f"input file not found: {src}")
                shutil.copy(src, target)
            else:
                target.write_text(str(value), encoding="utf-8")

        cmd = [
            sys.executable,
            str(_spiffsgen_path()),
            hex(partition.size),
            str(staging),
            str(out),
        ]
        _logger.info("build_spiffs: size=%s out=%s",
                     hex(partition.size), out.name)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            _logger.error("build_spiffs failed: %s",
                          (result.stderr or result.stdout).strip()[:200])
            raise BuildError(stage="spiffs", stderr=result.stderr or result.stdout)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
