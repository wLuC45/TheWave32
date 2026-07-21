from __future__ import annotations

import datetime as _dt
import tempfile
from pathlib import Path
from typing import Iterable

from thewave32 import builder, flasher, log as _log, state
from thewave32.manifest import Input
from thewave32.registry import Module

_logger = _log.get("pipeline")


def execute_flash(
    *,
    mod: Module,
    port: str,
    resolved: list[tuple[Input, object]],
    baud: int = 921600,
    state_path: Path | None = None,
    flash_log_path: Path | None = None,
) -> None:
    """Run the full flash pipeline for one module.

    Both the CLI and the TUI call this to ensure the same orchestration:
    chip detection, NVS/SPIFFS staging in a self-cleaning workdir, esptool
    invocation, and on-disk state update. Required-input validation must
    happen *before* this function — call ``manifest.resolve_inputs`` to get
    ``resolved``. Raises any ``Tw32Error`` subclass on failure; in all
    paths the staging workdir is removed.
    """
    _logger.info("execute_flash: module=%s port=%s baud=%d resolved_keys=%s",
                 mod.name, port, baud, [i.key for i, _ in resolved])
    flasher.require_chip(port, expected=mod.target)

    artifacts: list[tuple[int, Path]] = [
        (a.offset, mod.path / a.path) for a in mod.manifest.flash.artifacts
    ]
    _logger.debug("base artifacts: %s",
                  [(hex(o), p.name) for o, p in artifacts])

    with tempfile.TemporaryDirectory(prefix="thewave32-build-") as workdir_str:
        workdir = Path(workdir_str)
        if "nvs" in mod.manifest.partitions:
            nvs_inputs = [(i, v) for i, v in resolved if i.target == "nvs"]
            if nvs_inputs:
                out = workdir / "nvs.bin"
                builder.build_nvs(mod.manifest.partitions["nvs"], nvs_inputs, out)
                artifacts.append((mod.manifest.partitions["nvs"].offset, out))
        if "spiffs" in mod.manifest.partitions:
            sp_inputs = [(i, v) for i, v in resolved if i.target == "spiffs"]
            if sp_inputs:
                out = workdir / "spiffs.bin"
                builder.build_spiffs(mod.manifest.partitions["spiffs"], sp_inputs, out)
                artifacts.append((mod.manifest.partitions["spiffs"].offset, out))

        log_path = flash_log_path or (Path.home() / ".cache" / "thewave32" / "last-flash.log")
        flasher.flash(
            port=port,
            baud=baud,
            artifacts=artifacts,
            log_path=log_path,
            chip=mod.target,
        )

    sp = state_path or state.default_path()
    st = state.load(sp)
    st.last_port = port
    st.current_module = mod.name
    st.flashed_at = _dt.datetime.now(_dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    st.inputs_used = {i.key: v for i, v in resolved}
    state.save(sp, st)
    _logger.info("flashed %s to %s", mod.name, port)


def parse_cli_inputs(entries: Iterable[str]) -> dict[str, str]:
    """Turn a list of ``key=value`` strings into a dict, raising on malformed entries."""
    from thewave32.errors import Tw32Error
    out: dict[str, str] = {}
    for entry in entries:
        if "=" not in entry:
            raise Tw32Error(f"--input must be key=value, got: {entry}")
        k, v = entry.split("=", 1)
        out[k] = v
    return out
