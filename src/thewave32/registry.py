from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from thewave32 import log as _log
from thewave32 import manifest as manifest_mod
from thewave32.errors import ManifestError, ModuleNotFoundError, Tw32Error

_logger = _log.get("registry")


@dataclass(frozen=True)
class Module:
    name: str
    path: Path
    manifest: manifest_mod.Manifest

    @property
    def description(self) -> str:
        return self.manifest.module.description

    @property
    def version(self) -> str:
        return self.manifest.module.version

    @property
    def target(self) -> str:
        return self.manifest.module.target

    @property
    def author(self) -> str | None:
        return self.manifest.module.author


@dataclass(frozen=True)
class LoadFailure:
    name: str
    path: Path
    reason: str


def _load_one(folder: Path) -> Module:
    toml = folder / "module.toml"
    m = manifest_mod.load(toml)
    for art in m.flash.artifacts:
        if not (folder / art.path).is_file():
            raise ManifestError(
                path=str(toml),
                reason=f"artifact file not found: {art.path}",
            )
    return Module(name=m.module.name, path=folder, manifest=m)


def discover(modules_root: Path) -> list[Module]:
    """Return all valid modules under modules_root, sorted by name.

    Bad modules (missing artefacts, malformed manifest) are logged and
    skipped — they do NOT raise. Use `discover_with_errors` if you need
    to surface the failures to the user.
    """
    mods, _ = discover_with_errors(modules_root)
    return mods


def discover_with_errors(
    modules_root: Path,
) -> tuple[list[Module], list[LoadFailure]]:
    out: list[Module] = []
    fails: list[LoadFailure] = []
    if not modules_root.is_dir():
        _logger.warning("modules_root not a directory: %s", modules_root)
        return out, fails
    for child in sorted(modules_root.iterdir()):
        if not child.is_dir() or not (child / "module.toml").is_file():
            continue
        try:
            out.append(_load_one(child))
        except Tw32Error as e:
            _logger.error("module %s skipped: %s", child.name, e)
            fails.append(LoadFailure(name=child.name, path=child, reason=str(e)))
        except Exception as e:  # noqa: BLE001 — never let one bad module kill the tool
            _logger.exception("module %s skipped (unexpected): %s", child.name, e)
            fails.append(LoadFailure(name=child.name, path=child, reason=repr(e)))
    return out, fails


def get(modules_root: Path, name: str) -> Module:
    folder = modules_root / name
    if not (folder / "module.toml").is_file():
        raise ModuleNotFoundError(name)
    return _load_one(folder)
