"""Host-side playbooks - sequence runner over the serial CLI.

A playbook is a plain-text script that the GUI executes step by step
against the currently-flashed module. It composes existing module
commands without any firmware change. Three step kinds:

  * a bare line  →  send that line as a serial command
  * ``sleep N``  →  wait N seconds (accepts decimals)
  * ``wait <event> [timeout]``  →  block until the firmware emits a
    JSON line whose ``"event"`` field matches; default timeout 30 s

Header lines start with ``#`` and may carry directives:

  # module: wifi-deauth      → restrict this playbook to one module
  # desc: scan + first attack → human-readable description in the UI

Anything else after ``#`` is a comment and ignored. Blank lines are
ignored too.

The runner is module-agnostic; it just writes the strings you wrote.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

from PySide6.QtCore import QObject, QTimer, Signal


# --- model -----------------------------------------------------------


@dataclass
class PlaybookStep:
    kind: str           # "cmd" | "sleep" | "wait"
    raw: str            # original line (for UI display)
    payload: str = ""   # for cmd
    duration: float = 0.0   # for sleep
    event_name: str = ""    # for wait
    timeout: float = 30.0   # for wait


class PlaybookParseError(ValueError):
    def __init__(self, line_no: int, message: str) -> None:
        self.line_no = line_no
        super().__init__(f"line {line_no}: {message}")


@dataclass
class Playbook:
    name: str
    description: str = ""
    module: Optional[str] = None   # slug constraint, or None for any
    steps: list[PlaybookStep] = field(default_factory=list)

    @classmethod
    def parse(cls, text: str, name: str = "<inline>") -> "Playbook":
        pb = cls(name=name)
        for i, raw in enumerate(text.splitlines(), start=1):
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                body = line[1:].strip()
                if body.lower().startswith("module:"):
                    pb.module = body.split(":", 1)[1].strip() or None
                elif body.lower().startswith("desc:"):
                    pb.description = body.split(":", 1)[1].strip()
                continue
            tok = line.split()
            if tok[0] == "sleep":
                if len(tok) != 2:
                    raise PlaybookParseError(i, "usage: sleep <seconds>")
                try:
                    dur = float(tok[1])
                except ValueError:
                    raise PlaybookParseError(i, f"bad number: {tok[1]!r}")
                if dur < 0 or dur > 600:
                    raise PlaybookParseError(i, "sleep must be 0..600 s")
                pb.steps.append(PlaybookStep(kind="sleep", raw=line, duration=dur))
            elif tok[0] == "wait":
                if len(tok) < 2 or len(tok) > 3:
                    raise PlaybookParseError(
                        i, "usage: wait <event_name> [timeout_s]"
                    )
                ev = tok[1]
                tmo = 30.0
                if len(tok) == 3:
                    try:
                        tmo = float(tok[2])
                    except ValueError:
                        raise PlaybookParseError(i, f"bad timeout: {tok[2]!r}")
                    if tmo <= 0 or tmo > 600:
                        raise PlaybookParseError(i, "timeout must be 0..600 s")
                pb.steps.append(PlaybookStep(
                    kind="wait", raw=line, event_name=ev, timeout=tmo,
                ))
            else:
                pb.steps.append(PlaybookStep(kind="cmd", raw=line, payload=line))
        return pb


# --- runner ----------------------------------------------------------


class PlaybookRunner(QObject):
    """Drives a Playbook over a `send_cmd(str)` callable. Subscribers
    feed inbound JSON via :meth:`feed_json` so ``wait`` steps can
    resolve. Emits per-step + final signals so the GUI can show
    progress."""

    step_started = Signal(int, str)        # idx, raw
    step_done    = Signal(int, str, str)   # idx, raw, detail
    finished     = Signal(bool, str)       # ok, summary
    log          = Signal(str)

    def __init__(self, playbook: Playbook,
                 send_cmd: Callable[[str], None],
                 parent=None) -> None:
        super().__init__(parent)
        self._pb = playbook
        self._send = send_cmd
        self._idx = 0
        self._cancelled = False
        self._waiting_event: Optional[str] = None
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self._on_timeout)

    # --- control ---

    def start(self) -> None:
        self.log.emit(f"▶ {self._pb.name}: {len(self._pb.steps)} step(s)")
        self._idx = 0
        self._cancelled = False
        self._next()

    def stop(self) -> None:
        if self._cancelled:
            return
        self._cancelled = True
        self._waiting_event = None
        self._timer.stop()
        self.log.emit("■ stopped by user")
        self.finished.emit(False, "cancelled")

    def feed_json(self, obj: dict) -> None:
        if self._cancelled or self._waiting_event is None:
            return
        if obj.get("event") == self._waiting_event:
            ev = self._waiting_event
            self._waiting_event = None
            self._timer.stop()
            self._after_step(detail=f"got {ev}")

    # --- internals ---

    def _next(self) -> None:
        if self._cancelled:
            return
        if self._idx >= len(self._pb.steps):
            self.log.emit("✓ done")
            self.finished.emit(True, "complete")
            return
        step = self._pb.steps[self._idx]
        self.step_started.emit(self._idx, step.raw)
        self.log.emit(f"  {self._idx + 1}/{len(self._pb.steps)}  {step.raw}")
        if step.kind == "cmd":
            try:
                self._send(step.payload)
            except Exception as e:  # noqa: BLE001
                self.log.emit(f"✗ send failed: {e}")
                self.finished.emit(False, f"send failed at step {self._idx}")
                return
            # Tiny delay between cmds so the firmware has time to ack
            # before the next one arrives.
            self._timer.start(150)
        elif step.kind == "sleep":
            self._timer.start(int(step.duration * 1000))
        elif step.kind == "wait":
            self._waiting_event = step.event_name
            self._timer.start(int(step.timeout * 1000))

    def _on_timeout(self) -> None:
        if self._cancelled:
            return
        step = self._pb.steps[self._idx]
        if step.kind == "wait":
            ev = self._waiting_event
            self._waiting_event = None
            self.log.emit(f"✗ timeout waiting for event:{ev}")
            self.finished.emit(False, f"timeout at step {self._idx}")
            return
        # cmd post-delay or sleep finished
        self._after_step(detail="ok")

    def _after_step(self, detail: str = "") -> None:
        step = self._pb.steps[self._idx]
        self.step_done.emit(self._idx, step.raw, detail)
        self._idx += 1
        self._next()


# --- discovery -------------------------------------------------------


def user_playbook_dir() -> Path:
    import os
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "thewave32" / "playbooks"


def repo_playbook_dir() -> Optional[Path]:
    """Repo-shipped examples next to the source tree."""
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / "playbooks"
        if candidate.is_dir():
            return candidate
    return None


def discover_playbooks() -> list[Playbook]:
    """Load every ``*.txt`` from the user's config dir AND the repo's
    playbooks/ directory. User entries override built-ins on name
    collision."""
    found: dict[str, Playbook] = {}
    for d in (repo_playbook_dir(), user_playbook_dir()):
        if d is None or not d.is_dir():
            continue
        for f in sorted(d.glob("*.txt")):
            try:
                pb = Playbook.parse(f.read_text(encoding="utf-8"), name=f.stem)
            except (OSError, PlaybookParseError):
                continue
            found[pb.name] = pb
    return list(found.values())


__all__ = [
    "Playbook", "PlaybookStep", "PlaybookRunner", "PlaybookParseError",
    "discover_playbooks", "user_playbook_dir", "repo_playbook_dir",
]
