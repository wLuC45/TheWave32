"""Auto-rebuild stale firmware modules before discovery.

Scans `firmware/<slug>/` directories that look buildable (have main/ +
CMakeLists.txt) and rebuilds the ones whose sources are newer than the
artefacts in `modules/<slug>/`. A failure in one module does not abort the
others — the failure is logged and that module is left without fresh
artefacts (registry will then skip it).
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from thewave32 import log as _log
from thewave32 import idf as _idf

_logger = _log.get("compiler")

_ARTIFACT_NAMES = ("firmware.bin", "bootloader.bin", "partition-table.bin")
_SOURCE_IGNORE = {"build", ".git", "__pycache__"}


@dataclass
class BuildResult:
    slug: str
    rebuilt: bool
    ok: bool
    reason: str = ""


def _is_buildable(fw_dir: Path) -> bool:
    return (fw_dir / "main").is_dir() and (fw_dir / "CMakeLists.txt").is_file()


def _newest_source_mtime(fw_dir: Path) -> float:
    newest = 0.0
    for root, dirs, files in os.walk(fw_dir):
        dirs[:] = [d for d in dirs if d not in _SOURCE_IGNORE]
        for f in files:
            try:
                m = (Path(root) / f).stat().st_mtime
            except OSError:
                continue
            if m > newest:
                newest = m
    return newest


def _oldest_artefact_mtime(out_dir: Path) -> float | None:
    """Returns the oldest mtime among required artefacts; None if any missing."""
    oldest = float("inf")
    for name in _ARTIFACT_NAMES:
        p = out_dir / name
        if not p.is_file():
            return None
        m = p.stat().st_mtime
        if m < oldest:
            oldest = m
    return oldest


def is_stale(fw_dir: Path, out_dir: Path) -> bool:
    art_mtime = _oldest_artefact_mtime(out_dir)
    if art_mtime is None:
        return True
    return _newest_source_mtime(fw_dir) > art_mtime


def _locate_export_sh() -> Path | None:
    """Find ESP-IDF's export.sh. Honours $IDF_PATH, falls back to thewave32.idf."""
    env = os.environ.get("IDF_PATH")
    if env:
        cand = Path(env) / "export.sh"
        if cand.is_file():
            return cand
    try:
        cand = _idf.locate() / "export.sh"
        if cand.is_file():
            return cand
    except Exception:
        pass
    return None


def _run_idf_build(fw_dir: Path) -> tuple[bool, str]:
    """Invoke `idf.py build` for fw_dir. Returns (ok, combined_output_tail)."""
    export_sh = _locate_export_sh()
    if export_sh is None:
        return False, "ESP-IDF export.sh not found (set $IDF_PATH)"

    # Source export.sh, then idf.py build, in one shell so PATH is set up.
    cmd = f'set -e; source "{export_sh}" >/dev/null 2>&1; idf.py -B build build'
    _logger.debug("build cmd in %s: %s", fw_dir, cmd)
    try:
        proc = subprocess.run(
            ["bash", "-c", cmd],
            cwd=str(fw_dir),
            capture_output=True,
            text=True,
            timeout=600,
        )
    except subprocess.TimeoutExpired:
        return False, "idf.py build timed out after 600s"
    except OSError as e:
        return False, f"failed to spawn bash: {e}"

    out = (proc.stdout or "") + (proc.stderr or "")
    tail = "\n".join(out.strip().splitlines()[-15:])
    if proc.returncode != 0:
        return False, f"rc={proc.returncode}\n{tail}"
    return True, tail


def _copy_artefacts(fw_dir: Path, out_dir: Path) -> tuple[bool, str]:
    build = fw_dir / "build"
    boot = build / "bootloader" / "bootloader.bin"
    parts = build / "partition_table" / "partition-table.bin"
    app: Path | None = None
    for p in build.glob("*.bin"):
        if p.name in ("bootloader.bin", "partition-table.bin"):
            continue
        app = p
        break
    if not boot.is_file() or not parts.is_file() or app is None:
        return False, "missing build artefacts after idf.py build"
    out_dir.mkdir(parents=True, exist_ok=True)
    for src, dst in (
        (boot,  out_dir / "bootloader.bin"),
        (parts, out_dir / "partition-table.bin"),
        (app,   out_dir / "firmware.bin"),
    ):
        shutil.copy2(src, dst)
        # Bump mtime to "now" so later staleness checks see this build as
        # fresh, even if IDF didn't rebuild every artefact (e.g. bootloader
        # often stays untouched and would otherwise look stale forever).
        os.utime(dst, None)
    return True, ""


def rebuild_one(fw_dir: Path, out_dir: Path) -> BuildResult:
    slug = fw_dir.name
    _logger.info("building %s", slug)
    ok, tail = _run_idf_build(fw_dir)
    if not ok:
        _logger.error("build failed for %s: %s", slug, tail)
        return BuildResult(slug=slug, rebuilt=True, ok=False, reason=tail)
    ok, msg = _copy_artefacts(fw_dir, out_dir)
    if not ok:
        _logger.error("artefact copy failed for %s: %s", slug, msg)
        return BuildResult(slug=slug, rebuilt=True, ok=False, reason=msg)
    _logger.info("rebuilt %s OK", slug)
    return BuildResult(slug=slug, rebuilt=True, ok=True)


def ensure_fresh(repo_root: Path) -> list[BuildResult]:
    """Walk firmware/<slug>/ and rebuild stale modules.

    Returns one BuildResult per buildable firmware dir. Skipped (up-to-date)
    modules are reported with rebuilt=False, ok=True. Build failures are
    isolated: the loop continues and other modules still get a chance.
    """
    fw_root = repo_root / "firmware"
    out_root = repo_root / "modules"
    if not fw_root.is_dir():
        _logger.warning("no firmware/ dir at %s, skipping auto-build", fw_root)
        return []

    results: list[BuildResult] = []
    for fw_dir in sorted(p for p in fw_root.iterdir() if p.is_dir()):
        if not _is_buildable(fw_dir):
            continue
        slug = fw_dir.name
        out_dir = out_root / slug
        try:
            stale = is_stale(fw_dir, out_dir)
        except Exception as e:
            _logger.exception("staleness check failed for %s: %s", slug, e)
            results.append(BuildResult(slug=slug, rebuilt=False, ok=False, reason=str(e)))
            continue
        if not stale:
            _logger.debug("%s up-to-date", slug)
            results.append(BuildResult(slug=slug, rebuilt=False, ok=True))
            continue
        try:
            results.append(rebuild_one(fw_dir, out_dir))
        except Exception as e:
            _logger.exception("unexpected error building %s: %s", slug, e)
            results.append(BuildResult(slug=slug, rebuilt=True, ok=False, reason=str(e)))
    return results


def repo_root_from_modules(modules_root: Path) -> Path:
    """Given a modules/ path, return its parent (the repo root)."""
    return modules_root.resolve().parent
