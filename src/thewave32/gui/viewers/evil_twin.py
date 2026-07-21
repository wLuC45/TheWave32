"""Live client-association viewer for wifi-evil-twin."""

from __future__ import annotations

import time
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox, QTableWidget,
    QTableWidgetItem, QVBoxLayout,
)

from .base import BaseViewer


class EvilTwinClientsViewer(BaseViewer):
    """Real-time list of clients that have associated to the rogue AP."""

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._row_for_mac: dict[str, int] = {}
        self._build()

    def _build(self) -> None:
        # Two action rows so the layout stays usable on narrow widths.
        target_row = QHBoxLayout()
        target_row.setSpacing(8)
        target_row.addWidget(QLabel("SSID"))
        self.ssid = QLineEdit()
        self.ssid.setPlaceholderText("FreeWiFi")
        target_row.addWidget(self.ssid, 1)
        target_row.addWidget(QLabel("chan"))
        self.chan = QSpinBox()
        self.chan.setRange(1, 14)
        self.chan.setValue(6)
        target_row.addWidget(self.chan)
        target = QPushButton("set target")
        target.clicked.connect(self._set_target)
        self.register_action_button(target)
        target_row.addWidget(target)

        # --- security row -------------------------------------------------
        sec_row = QHBoxLayout()
        sec_row.setSpacing(8)
        sec_row.addWidget(QLabel("password"))
        self.psk_edit = QLineEdit()
        self.psk_edit.setPlaceholderText("8..63 chars (empty = open)")
        self.psk_edit.setEchoMode(QLineEdit.EchoMode.Password)
        sec_row.addWidget(self.psk_edit, 2)
        psk_btn = QPushButton("set")
        psk_btn.clicked.connect(self._set_password)
        self.register_action_button(psk_btn)
        sec_row.addWidget(psk_btn)
        psk_clear = QPushButton("clear (→ open)")
        psk_clear.clicked.connect(
            lambda: self.fire("password clear", label="password clear")
        )
        self.register_action_button(psk_clear)
        sec_row.addWidget(psk_clear)
        sec_row.addSpacing(12)
        sec_row.addWidget(QLabel("hidden"))
        h_on = QPushButton("on")
        h_on.clicked.connect(lambda: self.fire("hidden on", label="hidden on"))
        self.register_action_button(h_on)
        sec_row.addWidget(h_on)
        h_off = QPushButton("off")
        h_off.clicked.connect(lambda: self.fire("hidden off", label="hidden off"))
        self.register_action_button(h_off)
        sec_row.addWidget(h_off)
        sec_row.addStretch(1)

        run_row = QHBoxLayout()
        run_row.setSpacing(8)
        run_row.addWidget(self.make_action_button("start", primary=True, optimistic_state="running"))
        run_row.addWidget(self.make_action_button("stop", optimistic_state="stopped"))
        run_row.addWidget(self.make_action_button("clients"))
        run_row.addWidget(self.make_action_button("stats"))
        run_row.addStretch(1)

        self.status = QLabel("No target set.")
        self.status.setProperty("secondary", True)
        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["MAC", "Status", "Last event @"])
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(target_row)
        layout.addLayout(sec_row)
        layout.addLayout(run_row)
        layout.addWidget(self.status)
        layout.addWidget(self.table, 1)

    def _set_password(self) -> None:
        pwd = self.psk_edit.text().strip()
        if not pwd:
            return
        self.psk_edit.clear()
        self.fire(f"password {pwd}", label=f"password set ({len(pwd)} ch)")

    def _set_target(self) -> None:
        ssid = self.ssid.text().strip()
        if not ssid:
            return
        self.fire(f"target {ssid} {self.chan.value()}", label=f"target {ssid}")

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        evt = obj.get("event")
        cmd = obj.get("cmd")
        if evt == "client_connected" and self._status is not None:
            self._status.record("client_connected", detail=obj.get("mac", ""))
        elif evt == "client_disconnected" and self._status is not None:
            self._status.record("client_disconnected", detail=obj.get("mac", ""))
        if cmd == "stats":
            # Run state is on the StatusStrip; per-client status is the
            # table below. Only the lifetime aggregate stays here.
            self.status.setText(
                f"total connects: {obj.get('total_connects', 0)}"
            )
            return
        if cmd == "target" and obj.get("ok"):
            self.status.setText("Target set.")
            return
        if evt == "client_connected":
            self._upsert(obj.get("mac", ""), "● connected", QColor(80, 200, 100))
            return
        if evt == "client_disconnected":
            self._upsert(obj.get("mac", ""), f"○ disconnected (reason {obj.get('reason','?')})", QColor(180, 180, 180))
            return
        if evt == "client":   # from `clients` dump
            self._upsert(obj.get("mac", ""), "● connected", QColor(80, 200, 100))

    def _upsert(self, mac: str, status: str, fg: QColor) -> None:
        if not mac:
            return
        ts = time.strftime("%H:%M:%S")
        if mac in self._row_for_mac:
            row = self._row_for_mac[mac]
        else:
            row = self.table.rowCount()
            self.table.insertRow(row)
            self._row_for_mac[mac] = row
            mac_item = QTableWidgetItem(mac)
            mac_item.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, 0, mac_item)
        st = QTableWidgetItem(status)
        st.setForeground(fg)
        ts_item = QTableWidgetItem(ts)
        for i, it in enumerate((st, ts_item), start=1):
            it.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, i, it)
