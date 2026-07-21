from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path


@dataclass
class State:
    last_port: str | None = None
    current_module: str | None = None
    flashed_at: str | None = None
    inputs_used: dict = field(default_factory=dict)


def default_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "thewave32" / "state.json"


def load(path: Path) -> State:
    if not path.is_file():
        return State()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return State()
    return State(
        last_port=data.get("last_port"),
        current_module=data.get("current_module"),
        flashed_at=data.get("flashed_at"),
        inputs_used=data.get("inputs_used", {}),
    )


def save(path: Path, st: State) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(asdict(st), indent=2), encoding="utf-8")
