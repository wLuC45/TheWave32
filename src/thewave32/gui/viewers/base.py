"""BaseViewer + helpers shared by every per-module visualisation.

All viewers inherit from ``BaseViewer``; that class provides:

* ``StatusStrip`` - a slim header showing the module's run state plus
  the timestamp of the last user action. Every click on a button created
  via ``make_action_button`` updates the strip *optimistically* so the
  user gets instant visual feedback (the firmware's ack later confirms
  it or flips it to an error state).
* ``make_action_button`` - a one-liner for constructing a styled button
  that sends a command, records it on the strip, and (optionally)
  switches the strip into a guess-this-state mode.
* Default ``on_json`` that translates start/stop acks into state
  transitions; subclasses calling ``super().on_json(obj)`` get this for
  free.
"""

from __future__ import annotations

import time
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QDialog, QDialogButtonBox, QHBoxLayout, QLabel, QPlainTextEdit,
    QPushButton, QSizePolicy, QVBoxLayout, QWidget,
)

from thewave32.gui import theme


_STATES: dict[str, tuple[str, str, str]] = {
    # name        ->  (dot,  colour,                label)
    "idle":          ("o",   theme.TEXT_MUTED,      "idle"),
    "ready":         ("*",   theme.ACCENT,          "ready"),
    "running":       ("*",   theme.SUCCESS,         "running"),
    "scanning":      ("*",   theme.WARNING,         "scanning"),
    "attacking":     ("*",   theme.DANGER,          "attacking"),
    "stopped":       ("o",   theme.TEXT_MUTED,      "stopped"),
    "capturing":     ("*",   theme.SUCCESS,         "capturing"),
    "streaming":     ("*",   theme.SUCCESS,         "streaming"),
    "connected":     ("*",   theme.SUCCESS,         "connected"),
    "error":         ("x",   theme.DANGER,          "error"),
}


class StatusStrip(QWidget):
    """Slim header band: state dot + label on the left, last-action on the right.

    Designed to be the first widget in every viewer's layout - gives the
    user an unmistakable "where am I?" cue.
    """

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        # Slim strip - one row of monospace text. No padding to the
        # extreme, but still readable.
        self.setObjectName("ViewerStatusStrip")
        self.setMinimumHeight(22)
        self.setMaximumHeight(22)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(2, 0, 2, 0)
        layout.setSpacing(8)

        self.dot = QLabel("o")
        self.dot.setStyleSheet(
            f"color: {theme.TEXT_MUTED}; font-size: 11pt; background: transparent;"
        )
        self.state_label = QLabel("idle")
        self.state_label.setStyleSheet(
            f"color: {theme.TEXT_DIM}; font-weight: 500; background: transparent;"
        )
        self.action_label = QLabel("")
        self.action_label.setStyleSheet(
            f"color: {theme.TEXT_MUTED}; "
            f"font-size: 8.5pt; background: transparent;"
        )
        layout.addWidget(self.dot)
        layout.addWidget(self.state_label)
        layout.addStretch(1)
        layout.addWidget(self.action_label)

    def set_state(self, state: str) -> None:
        dot, color, label = _STATES.get(state, ("?", theme.TEXT_MUTED, state))
        self.dot.setText(dot)
        self.dot.setStyleSheet(
            f"color: {color}; font-size: 11pt; background: transparent;"
        )
        self.state_label.setText(label)
        self.state_label.setStyleSheet(
            f"color: {color}; font-weight: 500; background: transparent;"
        )

    def record(self, label: str, ok: bool | None = None, detail: str = "") -> None:
        ts = time.strftime("%H:%M:%S")
        sym = "ok" if ok is True else ("err" if ok is False else ">")
        text = f"{label} {sym} {ts}"
        if detail:
            text += f"  |  {detail}"
        self.action_label.setText(text)


class BaseViewer(QWidget):
    """Abstract base for per-module viewers."""

    send_command = Signal(str)

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(parent)
        self.slug = slug
        self._status: StatusStrip | None = None
        self._action_buttons: list[QPushButton] = []
        self._actions_enabled: bool = False        # gated until flashed
        self._gate_message = (
            "Flash this module first to enable its commands."
        )
        # About-dialog metadata populated by the host when a module is
        # selected. The viewer's own HELP class attribute provides the
        # data-flow context; the manifest fields below describe the
        # firmware itself. Both feed the "Sobre" dialog.
        self._about_name: str = ""
        self._about_version: str = ""
        self._about_target: str = ""
        self._about_description: str = ""

    # --- helpers ----------------------------------------------------

    def make_status_strip(self) -> StatusStrip:
        self._status = StatusStrip()
        # Show the "needs flash" hint by default so the user understands
        # why the buttons are disabled the first time they pick a module.
        if not self._actions_enabled:
            self._status.record(self._gate_message)
        return self._status

    def set_actions_enabled(self, on: bool) -> None:
        """Toggle every button registered through ``make_action_button``
        (and ``register_action_button``). The MainWindow flips this on
        after a successful flash for the module's slug, and back off
        when the user picks a different module that hasn't been flashed
        yet."""
        self._actions_enabled = on
        for btn in self._action_buttons:
            btn.setEnabled(on)
        if self._status is not None:
            if on:
                self._status.record("ready", True, detail="commands enabled")
                self._status.set_state("ready")
            else:
                self._status.record(self._gate_message)
                self._status.set_state("idle")

    def register_action_button(self, btn: QPushButton) -> QPushButton:
        """Hook for hand-built buttons (e.g. the per-row 'attack'
        buttons in the deauth viewer) so they participate in the same
        flash gate."""
        self._action_buttons.append(btn)
        btn.setEnabled(self._actions_enabled)
        return btn

    def fire(self, cmd: str, *, optimistic_state: str | None = None,
             label: str | None = None) -> None:
        """Send a command and give the user instant visual feedback.

        ``optimistic_state`` lets the caller declare what state the
        viewer is *about to be* in before the firmware confirms - e.g.
        clicking 'start' optimistically flips to 'running'. The next
        ``cmd: <name>`` ack from the firmware re-applies (or corrects)
        the state via ``BaseViewer.on_json``.
        """
        if self._status is not None:
            self._status.record(label or cmd)
            if optimistic_state:
                self._status.set_state(optimistic_state)
        self.send_command.emit(cmd)

    def make_action_button(
        self,
        label: str,
        cmd: str | None = None,
        *,
        primary: bool = False,
        optimistic_state: str | None = None,
    ) -> QPushButton:
        btn = QPushButton(label)
        if primary:
            btn.setProperty("primary", True)
            btn.style().unpolish(btn)
            btn.style().polish(btn)
        btn.setEnabled(self._actions_enabled)
        the_cmd = cmd or label
        the_state = optimistic_state
        btn.clicked.connect(
            lambda _checked=False, c=the_cmd, s=the_state, lbl=label:
                self.fire(c, optimistic_state=s, label=lbl)
        )
        self._action_buttons.append(btn)
        return btn

    # --- "Sobre" (about) dialog -----------------------------------

    def set_about_info(self, name: str, version: str, target: str,
                       description: str) -> None:
        """Host-side hook: stash the manifest metadata so the in-viewer
        Sobre button can show it. The viewer's class-level HELP attribute
        provides the second half of the dialog body (data-flow notes)."""
        self._about_name = name or ""
        self._about_version = version or ""
        self._about_target = target or ""
        self._about_description = description or ""

    def show_about_dialog(self) -> None:
        """Open a compact modal dialog with the module's full information:
        manifest description + viewer HELP. The header card no longer
        carries this text inline; this dialog is the single entry point."""
        help_text = (getattr(self, "HELP", "") or "").strip()
        manifest_desc = (self._about_description or "").strip()

        dlg = QDialog(self)
        dlg.setWindowTitle(f"Sobre - {self._about_name or self.slug}")
        # Keep the dialog tight. A small min width so wrapped paragraphs
        # don't go single-column-of-one-word; nothing else is fixed.
        dlg.setMinimumWidth(420)

        lay = QVBoxLayout(dlg)
        lay.setContentsMargins(14, 12, 14, 12)
        lay.setSpacing(8)

        # Identity line: name v<version> | target | slug
        bits: list[str] = []
        if self._about_name:
            bits.append(self._about_name)
        if self._about_version:
            bits.append(f"v{self._about_version}")
        if self._about_target:
            bits.append(self._about_target)
        bits.append(self.slug)
        head = QLabel(" | ".join(bits))
        head.setStyleSheet(
            f"color: {theme.TERM_YELLOW}; font-weight: 700; "
            f"font-size: 10pt; background: transparent;"
        )
        head.setWordWrap(True)
        lay.addWidget(head)

        if manifest_desc:
            cap1 = QLabel("DESCRIPTION")
            cap1.setStyleSheet(
                f"color: {theme.TERM_BLUE}; font-size: 7.5pt; "
                f"letter-spacing: 1.2px; font-weight: 700;"
            )
            lay.addWidget(cap1)
            d1 = QLabel(manifest_desc)
            d1.setWordWrap(True)
            d1.setStyleSheet(
                f"color: {theme.TEXT}; font-size: 9pt; background: transparent;"
            )
            lay.addWidget(d1)

        if help_text:
            cap2 = QLabel("VIEWER")
            cap2.setStyleSheet(
                f"color: {theme.TERM_BLUE}; font-size: 7.5pt; "
                f"letter-spacing: 1.2px; font-weight: 700;"
            )
            lay.addWidget(cap2)
            d2 = QLabel(help_text)
            d2.setWordWrap(True)
            d2.setStyleSheet(
                f"color: {theme.TEXT_DIM}; font-size: 9pt; background: transparent;"
            )
            lay.addWidget(d2)

        if not manifest_desc and not help_text:
            empty = QLabel("No description available.")
            empty.setStyleSheet(f"color: {theme.TEXT_MUTED};")
            lay.addWidget(empty)

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        btns.rejected.connect(dlg.reject)
        btns.accepted.connect(dlg.accept)
        btns.button(QDialogButtonBox.StandardButton.Close).clicked.connect(dlg.accept)
        lay.addWidget(btns)
        dlg.exec()

    # --- lifecycle hooks ------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        """Translates start/stop/scan/attack acks into state transitions."""
        if self._status is None:
            return
        cmd = obj.get("cmd")
        ok = obj.get("ok")
        if cmd is None or ok is None:
            return
        if cmd == "start":
            if ok:
                self._status.record(cmd, True)
                self._status.set_state("running")
            else:
                self._status.record(cmd, False, detail=obj.get("err", ""))
                self._status.set_state("error")
        elif cmd == "stop":
            if ok:
                self._status.record(cmd, True)
                self._status.set_state("stopped")
            else:
                self._status.record(cmd, False, detail=obj.get("err", ""))
        elif cmd in ("attack", "scan"):
            if ok:
                self._status.record(cmd, True)
            else:
                self._status.record(cmd, False, detail=obj.get("err", ""))

    def on_raw(self, data: bytes) -> None:
        """Default: ignore. Override for binary streams."""

    def request_binary_mode(self, on: bool) -> None:
        """Default: no-op. The MainWindow patches this so a viewer can
        ask the SerialWorker to switch parsers."""

    def teardown(self) -> None:
        """Release everything that outlives the widget tree, called by
        the host just before this viewer is removed and deleted.

        Without this, swapping away from a binary-streaming viewer
        (CSI / sniffer) leaves the SerialWorker stuck in binary mode so
        the next module looks dead, and a still-pending poll QTimer can
        fire one stale `stats` after the swap. Subclasses overriding
        this must call ``super().teardown()``.
        """
        from PySide6.QtCore import QTimer
        for t in self.findChildren(QTimer):
            t.stop()
        # Drop the serial worker out of binary mode in case this was a
        # PCAP/CSI viewer that never got an explicit `stop`.
        try:
            self.request_binary_mode(False)
        except Exception:  # noqa: BLE001 - teardown must not raise
            pass
        # Detach the outbound command signal so a timer that fires in
        # the gap before deleteLater() runs can't reach the host.
        try:
            self.send_command.disconnect()
        except (RuntimeError, TypeError):
            pass


__all__ = ["BaseViewer", "StatusStrip"]
