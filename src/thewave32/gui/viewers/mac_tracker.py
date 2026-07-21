"""Presence indicator + RSSI sparklines per watched MAC."""

from __future__ import annotations

import time
from collections import deque
from typing import Any

import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget,
)

from thewave32.gui import theme

from .base import BaseViewer


class MacTrackerViewer(BaseViewer):
    """Top: watch table (MAC, label, present, last RSSI).
    Bottom: multi-line RSSI plot (one trace per MAC, themed colours).
    """

    HISTORY = 240  # ~4 min if events come 1 Hz

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._series: dict[str, deque[tuple[float, int]]] = {}
        self._labels: dict[str, str] = {}
        self._row_for_mac: dict[str, int] = {}
        # Persistent pyqtgraph curve handles - re-using these and calling
        # `setData` instead of `plot.clear()` + `plot.plot()` keeps the
        # OpenGL state hot and avoids legend recreation.
        self._curves: dict[str, Any] = {}
        self._build()

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("start", primary=True, optimistic_state="running"))
        actions.addWidget(self.make_action_button("stop", optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("list"))
        actions.addWidget(self.make_action_button("clear"))
        actions.addWidget(self.make_action_button("stats"))
        actions.addSpacing(20)
        actions.addWidget(QLabel("add"))
        self.mac_input = QLineEdit()
        self.mac_input.setPlaceholderText("aa:bb:cc:dd:ee:ff Phone")
        actions.addWidget(self.mac_input, 1)
        add = QPushButton("watch")
        add.setProperty("primary", True)
        add.clicked.connect(self._add_watch)
        self.register_action_button(add)
        actions.addWidget(add)

        # Table.
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["MAC", "Label", "Present", "RSSI"])
        self.table.horizontalHeader().setStretchLastSection(False)
        self.table.setColumnWidth(0, 160)
        self.table.setColumnWidth(2, 90)
        self.table.setColumnWidth(3, 70)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)

        # Plot.
        self.plot = pg.PlotWidget()
        self.plot.setMinimumHeight(220)
        self.plot.setLabel("left", "RSSI", units="dBm",
                           **{"color": theme.TEXT_DIM, "font-size": "10pt"})
        self.plot.setLabel("bottom", "Time", units="s",
                           **{"color": theme.TEXT_DIM, "font-size": "10pt"})
        self.plot.showGrid(x=True, y=True, alpha=0.12)
        self.plot.hideButtons()
        self.plot.getAxis("left").setTextPen(pg.mkPen(theme.TEXT_MUTED))
        self.plot.getAxis("bottom").setTextPen(pg.mkPen(theme.TEXT_MUTED))
        self.plot.addLegend(offset=(-10, 10), labelTextColor=QColor(theme.TEXT_DIM))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addWidget(self.table, 1)
        layout.addWidget(self.plot, 2)

    def _add_watch(self) -> None:
        text = self.mac_input.text().strip()
        if not text:
            return
        self.send_command.emit(f"add {text}")
        self.mac_input.clear()
        self.send_command.emit("list")

    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        evt = obj.get("event")
        if evt == "watch":
            self._upsert_row(obj.get("mac", ""), obj.get("label", ""),
                             bool(obj.get("present")), int(obj.get("last_rssi", 0)))
            return
        if evt == "entered":
            mac = obj.get("mac", "")
            if self._status is not None:
                self._status.record("entered", detail=f"{mac} @ {obj.get('rssi','?')} dBm")
            self._upsert_row(mac, obj.get("label", ""), True, int(obj.get("rssi", 0)))
            self._append_sample(mac, int(obj.get("rssi", 0)), float(obj.get("ts", time.time() * 1000)))
            return
        if evt == "left":
            mac = obj.get("mac", "")
            if self._status is not None:
                self._status.record("left", detail=mac)
            self._upsert_row(mac, obj.get("label", ""), False, int(obj.get("last_rssi", 0)))

    def _upsert_row(self, mac: str, label: str, present: bool, rssi: int) -> None:
        if not mac:
            return
        self._labels[mac] = label
        if mac in self._row_for_mac:
            row = self._row_for_mac[mac]
        else:
            row = self.table.rowCount()
            self.table.insertRow(row)
            self._row_for_mac[mac] = row
            self._series[mac] = deque(maxlen=self.HISTORY)
        items = [
            QTableWidgetItem(mac),
            QTableWidgetItem(label or "-"),
            QTableWidgetItem("● present" if present else "○ absent"),
            QTableWidgetItem(str(rssi) if rssi else "-"),
        ]
        items[2].setForeground(
            QColor(theme.SUCCESS) if present else QColor(theme.TEXT_MUTED)
        )
        items[3].setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        for i, it in enumerate(items):
            it.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, i, it)
        if rssi:
            self._append_sample(mac, rssi, time.time() * 1000)

    def _append_sample(self, mac: str, rssi: int, ts_ms: float) -> None:
        if mac not in self._series:
            self._series[mac] = deque(maxlen=self.HISTORY)
        self._series[mac].append((ts_ms / 1000.0, rssi))
        self._refresh_plot()

    def _refresh_plot(self) -> None:
        # In-place updates only - no plot.clear() so pyqtgraph keeps its
        # cached PlotCurveItem state and the legend doesn't churn. New
        # MACs allocate a fresh curve once; subsequent updates reuse it.
        for i, (mac, pts) in enumerate(self._series.items()):
            if not pts:
                continue
            xs = [p[0] - pts[0][0] for p in pts]
            ys = [p[1] for p in pts]
            curve = self._curves.get(mac)
            if curve is None:
                color = theme.CHART_PALETTE[i % len(theme.CHART_PALETTE)]
                curve = self.plot.plot(
                    xs, ys,
                    pen=pg.mkPen(color, width=2),
                    name=self._labels.get(mac) or mac,
                )
                self._curves[mac] = curve
            else:
                curve.setData(xs, ys)
