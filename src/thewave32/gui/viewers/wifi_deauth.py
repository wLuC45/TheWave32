"""Custom viewer for wifi-deauth - scan list with click-to-attack."""

from __future__ import annotations

import time
from collections import deque
from typing import Any

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox, QSplitter, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget,
)

from thewave32.gui import theme

from ._scan_target import ScanTargetViewer


class WifiDeauthViewer(ScanTargetViewer):
    """Top: scanned APs (click row → attack idx).
    Middle: status (active, target, packets_sent rate sparkline).
    Bottom: target list (multi-target deauth)."""

    HISTORY = 120
    SCAN_ROW_LABEL = "attack"
    SCAN_ROW_COMMAND = "attack {idx}"
    SCAN_ROW_STATE = "attacking"

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._packets_history: deque[float] = deque(maxlen=self.HISTORY)
        self._last_packets = 0
        self._build()

    def _build(self) -> None:
        # --- top row of actions ---
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("scan", primary=True, optimistic_state="scanning"))
        actions.addWidget(self.make_action_button("stop", optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("clear targets", "clear"))
        actions.addWidget(self.make_action_button("stats"))
        actions.addSpacing(20)
        actions.addWidget(QLabel("attack SSID"))
        self.ssid_in = QLineEdit()
        self.ssid_in.setPlaceholderText("MyAP")
        actions.addWidget(self.ssid_in, 1)
        attack_btn = QPushButton("attack")
        attack_btn.setProperty("primary", True)
        attack_btn.clicked.connect(self._attack_ssid)
        self.register_action_button(attack_btn)
        actions.addWidget(attack_btn)

        # --- AP table (shared kernel) ---
        self._build_scan_table()
        # wifi-deauth shows the BSSID column slightly narrower than the
        # default to keep the action column visible at typical sizes.
        self.scans.setColumnWidth(2, 150)
        self.scans.setColumnWidth(5, 90)

        # --- status row + sparkline ---
        # `status` lives on the StatusStrip; `packets sent` is the
        # pkt/s plot below - neither is repeated here. Only `target`,
        # which nothing else shows, stays.
        status_row = QHBoxLayout()
        status_row.setSpacing(20)
        self.lbl_target = self._stat_label("-")
        status_row.addWidget(self._captioned("target", self.lbl_target))
        status_row.addStretch(1)

        self.plot = pg.PlotWidget()
        self.plot.setMaximumHeight(140)
        self.plot.setLabel("left", "pkt/s",
                           **{"color": theme.TEXT_DIM, "font-size": "9pt"})
        self.plot.showGrid(x=False, y=True, alpha=0.15)
        self.plot.setMouseEnabled(x=False, y=False)
        self.plot.hideButtons()
        self.plot.getAxis("left").setTextPen(pg.mkPen(theme.TEXT_MUTED))
        self.plot.getAxis("bottom").setTextPen(pg.mkPen(theme.TEXT_MUTED))
        self._curve = self.plot.plot(pen=pg.mkPen(theme.ACCENT, width=2))

        # --- targets list ---
        self.targets = QTableWidget(0, 4)
        self.targets.setHorizontalHeaderLabels(["#", "SSID/BSSID", "STA", "ch"])
        self.targets.horizontalHeader().setStretchLastSection(True)
        self.targets.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.targets.setColumnWidth(0, 40)

        # --- splitter (scans | status+targets) ---
        bottom = QWidget()
        bv = QVBoxLayout(bottom)
        bv.setContentsMargins(0, 0, 0, 0)
        bv.setSpacing(8)
        bv.addLayout(status_row)
        bv.addWidget(self.plot)
        targets_label = QLabel("Targets")
        targets_label.setProperty("heading", True)
        bv.addWidget(targets_label)
        bv.addWidget(self.targets, 1)

        split = QSplitter(Qt.Orientation.Vertical)
        scans_wrap = QWidget()
        sv = QVBoxLayout(scans_wrap)
        sv.setContentsMargins(0, 0, 0, 0)
        sv.setSpacing(8)
        scans_label = QLabel("Scanned APs (click `attack` to deauth)")
        scans_label.setProperty("heading", True)
        sv.addWidget(scans_label)
        sv.addWidget(self.scans, 1)
        split.addWidget(scans_wrap)
        split.addWidget(bottom)
        split.setSizes([300, 320])

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addWidget(split, 1)

        # Auto-poll stats so the live counter ticks. 1 Hz is enough.
        # Held back until the module is flashed (set_actions_enabled).
        self._poll = QTimer(self)
        self._poll.timeout.connect(lambda: self.send_command.emit("stats"))

    def set_actions_enabled(self, on: bool) -> None:
        super().set_actions_enabled(on)
        if on:
            self._poll.start(1000)
        else:
            self._poll.stop()

    def _stat_label(self, text: str, color: str = theme.TEXT) -> QLabel:
        lbl = QLabel(text)
        lbl.setStyleSheet(f"color: {color}; font-size: 14pt;")
        return lbl

    def _captioned(self, caption: str, widget: QWidget) -> QWidget:
        wrap = QWidget()
        v = QVBoxLayout(wrap)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(2)
        cap = QLabel(caption.upper())
        cap.setStyleSheet(f"color: {theme.TEXT_MUTED}; font-size: 8pt; letter-spacing: 1.2px;")
        v.addWidget(cap)
        v.addWidget(widget)
        return wrap

    def _attack_ssid(self) -> None:
        ssid = self.ssid_in.text().strip()
        if not ssid:
            return
        self.fire(f"attack {ssid}", optimistic_state="attacking", label=f"attack {ssid}")

    # ------------------------------------------------------------------
    # JSON ingestion
    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        evt = obj.get("event")
        cmd = obj.get("cmd")

        if evt == "ap":
            self._handle_scan_event(obj)
            return
        if cmd == "scan" and obj.get("ok"):
            if self._status is not None:
                self._status.record(
                    "scan", True, detail=f"{obj.get('count', 0)} APs"
                )
            return

        if cmd == "stats":
            active = bool(obj.get("active"))
            if self._status is not None:
                self._status.set_state("attacking" if active else "stopped")
            tc = obj.get("target_count", 0)
            ssid_or_bssid = obj.get("bssid", "-")
            self.lbl_target.setText(f"{tc}× {ssid_or_bssid}")
            packets = int(obj.get("packets_sent", 0))
            # pkt/s = delta since last poll (poll fires at 1Hz)
            rate = max(0.0, packets - self._last_packets)
            self._last_packets = packets
            self._packets_history.append(rate)
            self._curve.setData(list(self._packets_history))
            return

        if cmd == "targets":
            return  # the firmware emits one event per target; we render those instead.
        if evt == "target":
            self._upsert_target(obj)
            return

    # _handle_scan_event + _make_scan_row_button live in ScanTargetViewer.

    def _upsert_target(self, obj: dict[str, Any]) -> None:
        i = int(obj.get("idx", 0))
        # Reset table on idx 0 so dumps replace the previous list.
        if i == 0:
            self.targets.setRowCount(0)
        row = self.targets.rowCount()
        self.targets.insertRow(row)
        ident = obj.get("ssid") or obj.get("bssid", "")
        cells = [
            str(i),
            str(ident),
            str(obj.get("target", "")),
            str(obj.get("channel", "")),
        ]
        for col, val in enumerate(cells):
            it = QTableWidgetItem(val)
            it.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            self.targets.setItem(row, col, it)
