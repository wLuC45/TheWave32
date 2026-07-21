"""Custom viewer for wifi-bssid-clone - scan list with click-to-target.

The firmware accepts:
  scan                         → emits {"event":"ap", ...} per AP
  target <N>                   → pick from scan cache by index
  target <bssid> <ssid> <chan> → manual
  start / stop                 → raise / drop the cloned AP
  stats                        → ssid, bssid, channel, running

The previous CounterViewer-based viewer offered no way to pick a
target, so the user couldn't actually clone anything from the GUI.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox, QSplitter,
    QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

from thewave32.gui import theme

from ._scan_target import ScanTargetViewer


class WifiBssidCloneViewer(ScanTargetViewer):
    """Scanned APs table on top, current target + state strip below."""

    SCAN_ROW_LABEL = "set target"
    SCAN_ROW_COMMAND = "target {idx}"

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._build()

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("scan", primary=True,
                                                  optimistic_state="scanning"))
        actions.addWidget(self.make_action_button("start", primary=True,
                                                  optimistic_state="running"))
        actions.addWidget(self.make_action_button("stop",
                                                  optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("stats"))
        actions.addStretch(1)

        # --- security row ---------------------------------------------
        sec = QHBoxLayout()
        sec.setSpacing(8)
        sec.addWidget(QLabel("password"))
        self.psk_edit = QLineEdit()
        self.psk_edit.setPlaceholderText("8..63 chars (empty = open)")
        self.psk_edit.setEchoMode(QLineEdit.EchoMode.Password)
        sec.addWidget(self.psk_edit, 2)
        psk_btn = QPushButton("set")
        psk_btn.clicked.connect(self._set_password)
        self.register_action_button(psk_btn)
        sec.addWidget(psk_btn)
        psk_clear_btn = QPushButton("clear (→ open)")
        psk_clear_btn.clicked.connect(
            lambda: self.fire("password clear", label="password clear")
        )
        self.register_action_button(psk_clear_btn)
        sec.addWidget(psk_clear_btn)
        sec.addSpacing(16)
        sec.addWidget(QLabel("hidden"))
        hidden_on = QPushButton("on")
        hidden_on.clicked.connect(lambda: self.fire("hidden on", label="hidden on"))
        self.register_action_button(hidden_on)
        sec.addWidget(hidden_on)
        hidden_off = QPushButton("off")
        hidden_off.clicked.connect(lambda: self.fire("hidden off", label="hidden off"))
        self.register_action_button(hidden_off)
        sec.addWidget(hidden_off)

        # --- scan results table (shared kernel) -----------------------
        self._build_scan_table()

        # --- manual target row -----------------------------------------
        manual = QHBoxLayout()
        manual.setSpacing(8)
        manual.addWidget(QLabel("manual target"))
        self.bssid_edit = QLineEdit()
        self.bssid_edit.setPlaceholderText("aa:bb:cc:dd:ee:ff")
        manual.addWidget(self.bssid_edit, 2)
        self.ssid_edit = QLineEdit()
        self.ssid_edit.setPlaceholderText("MySSID")
        manual.addWidget(self.ssid_edit, 2)
        self.chan_spin = QSpinBox()
        self.chan_spin.setRange(1, 14)
        self.chan_spin.setValue(6)
        self.chan_spin.setPrefix("ch ")
        manual.addWidget(self.chan_spin)
        manual_btn = QPushButton("set target")
        manual_btn.clicked.connect(self._set_manual_target)
        self.register_action_button(manual_btn)
        manual.addWidget(manual_btn)

        # --- live target tile -------------------------------------------
        target_card = QWidget()
        target_card.setStyleSheet(
            f"QWidget {{ background: {theme.SURFACE}; "
            f"border-left: 3px solid {theme.ACCENT}; "
            f"border-top-right-radius: 6px; border-bottom-right-radius: 6px; "
            f"padding: 12px 18px; }}"
        )
        tcl = QVBoxLayout(target_card)
        tcl.setContentsMargins(12, 8, 12, 8)
        tcl.setSpacing(4)
        tc_cap = QLabel("CURRENT CLONE TARGET")
        tc_cap.setStyleSheet(
            f"color: {theme.TEXT_MUTED}; font-size: 8pt; letter-spacing: 1.4px; "
            f"font-weight: 600; background: transparent;"
        )
        self.lbl_target = QLabel("-")
        self.lbl_target.setStyleSheet(
            f"color: {theme.TEXT}; "
            f"font-size: 18pt; font-weight: 500; background: transparent;"
        )
        self.lbl_state = QLabel("waiting")
        self.lbl_state.setStyleSheet(
            f"color: {theme.TEXT_MUTED}; font-size: 9pt; background: transparent;"
        )
        tcl.addWidget(tc_cap)
        tcl.addWidget(self.lbl_target)
        tcl.addWidget(self.lbl_state)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addLayout(sec)
        layout.addWidget(target_card)
        scans_label = QLabel("Scanned APs (click a row's `set` to clone)")
        scans_label.setProperty("heading", True)
        layout.addWidget(scans_label)
        layout.addWidget(self.scans, 1)
        layout.addLayout(manual)

        # 1 Hz stats poll while gate is open.
        self._poll = QTimer(self)
        self._poll.timeout.connect(lambda: self.send_command.emit("stats"))

    def set_actions_enabled(self, on: bool) -> None:
        super().set_actions_enabled(on)
        if on:
            self._poll.start(1000)
        else:
            self._poll.stop()

    # ------------------------------------------------------------------

    def _set_password(self) -> None:
        pwd = self.psk_edit.text().strip()
        if not pwd:
            return
        # Mask the actual value in the action label so it doesn't leak
        # to the StatusStrip / log file.
        self.psk_edit.clear()
        self.fire(f"password {pwd}", label=f"password set ({len(pwd)} ch)")

    def _set_manual_target(self) -> None:
        bssid = self.bssid_edit.text().strip()
        ssid = self.ssid_edit.text().strip()
        if not bssid or not ssid:
            return
        chan = self.chan_spin.value()
        self.fire(f"target {bssid} {ssid} {chan}",
                  label=f"target {ssid}@ch{chan}")

    def _set_target_idx(self, idx: int) -> None:
        self.fire(f"target {idx}", label=f"target #{idx}")

    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        evt = obj.get("event")
        cmd = obj.get("cmd")

        if evt == "ap":
            self._handle_scan_event(obj)
            return

        if cmd == "stats":
            running = bool(obj.get("running"))
            self.lbl_target.setText(
                f"{obj.get('ssid', '-')} @ {obj.get('bssid', '-')} "
                f"(ch {obj.get('channel', '?')})"
            )
            self.lbl_state.setText(
                "● cloning, twin AP raised" if running else "○ idle (call start)"
            )
            self.lbl_state.setStyleSheet(
                f"color: {theme.SUCCESS if running else theme.TEXT_MUTED}; "
                f"font-size: 9pt; background: transparent;"
            )
            if self._status is not None:
                self._status.set_state("running" if running else "stopped")
            return

        if cmd == "target" and obj.get("ok"):
            self.lbl_target.setText(
                f"{obj.get('ssid', '-')} @ {obj.get('bssid', '-')} "
                f"(ch {obj.get('channel', '?')})"
            )
            self.lbl_state.setText("target set - call start to clone")
            self.lbl_state.setStyleSheet(
                f"color: {theme.WARNING}; font-size: 9pt; background: transparent;"
            )
            return

    # _handle_scan_event + _make_scan_row_button live in ScanTargetViewer.
