"""InteractionsPanel - unified table of user-side events across modules.

Anything that represents a real-world interaction with the device gets
funneled here so the user can audit a session without scanning the
firehose Console:

  * Wi-Fi clients associating / disassociating to a rogue AP
  * Credentials captured (EAP-ID, future captive-portal POSTs)
  * BLE host pairings + subscribes
  * USB host enumerations

The MainWindow's `_on_json` classifies incoming JSON and calls
``append`` here. Credential captures auto-switch the workspace tab to
this panel so the user notices.
"""

from __future__ import annotations

import csv
import time
from typing import Iterable

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QBrush
from PySide6.QtWidgets import (
    QComboBox, QFileDialog, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
    QPushButton, QSizePolicy, QTableWidget, QTableWidgetItem, QVBoxLayout,
    QWidget,
)

from thewave32.gui import theme


KIND_COLORS: dict[str, str] = {
    "Wi-Fi associate":       theme.SUCCESS,
    "Wi-Fi disassociate":    theme.TEXT_MUTED,
    "Captive-portal creds":  theme.DANGER,
    "EAP-ID captured":       theme.DANGER,
    "BLE host paired":       theme.PRIMARY,
    "BLE host wrote":        theme.PRIMARY_HI,
    "USB host enumerated":   theme.PRIMARY,
}


class InteractionsPanel(QWidget):
    """Live audit log of user-side events. Shaped like a SIEM dashboard:
    big counter cards on top, filterable table below, CSV export."""

    cleared = Signal()

    _COLUMNS = ("time", "module", "kind", "who", "detail")

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._rows: list[dict[str, str]] = []
        self._build()

    def _build(self) -> None:
        # --- counter strip on top ----------------------------------------
        self._counters: dict[str, QLabel] = {}
        cards = QHBoxLayout()
        cards.setSpacing(12)
        for kind in ("Wi-Fi associate", "Captive-portal creds",
                     "BLE host paired", "USB host enumerated"):
            card = self._make_card(kind, "0")
            cards.addWidget(card["wrap"])
            self._counters[kind] = card["val"]
        cards.addStretch(1)

        # --- filter row --------------------------------------------------
        filter_row = QHBoxLayout()
        filter_row.setSpacing(8)
        filter_row.addWidget(QLabel("Module"))
        self.module_filter = QComboBox()
        self.module_filter.addItem("(all)")
        self.module_filter.currentTextChanged.connect(self._refresh_rows)
        filter_row.addWidget(self.module_filter)
        filter_row.addSpacing(12)
        filter_row.addWidget(QLabel("Kind"))
        self.kind_filter = QComboBox()
        self.kind_filter.addItem("(all)")
        for k in KIND_COLORS:
            self.kind_filter.addItem(k)
        self.kind_filter.currentTextChanged.connect(self._refresh_rows)
        filter_row.addWidget(self.kind_filter)
        filter_row.addSpacing(12)
        filter_row.addWidget(QLabel("Search"))
        self.search = QLineEdit()
        self.search.setPlaceholderText("MAC, SSID, payload, …")
        self.search.textChanged.connect(self._refresh_rows)
        filter_row.addWidget(self.search, 1)
        export_btn = QPushButton("Export CSV")
        export_btn.clicked.connect(self._export_csv)
        filter_row.addWidget(export_btn)
        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(self.clear)
        filter_row.addWidget(clear_btn)

        # --- table ------------------------------------------------------
        self.table = QTableWidget(0, len(self._COLUMNS))
        self.table.setHorizontalHeaderLabels([c.upper() for c in self._COLUMNS])
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.setAlternatingRowColors(True)
        self.table.setSortingEnabled(True)
        h = self.table.horizontalHeader()
        h.setSectionResizeMode(QHeaderView.ResizeMode.Interactive)
        h.setStretchLastSection(True)
        self.table.setColumnWidth(0, 100)
        self.table.setColumnWidth(1, 150)
        self.table.setColumnWidth(2, 170)
        self.table.setColumnWidth(3, 180)

        # --- empty-state placeholder -----------------------------------
        self.empty = QLabel(
            "No interactions captured yet. Wi-Fi associations, BLE "
            "pairings, USB enumerations and credential captures will "
            "appear here as they happen."
        )
        self.empty.setProperty("secondary", True)
        self.empty.setWordWrap(True)
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.empty.setSizePolicy(QSizePolicy.Policy.Expanding,
                                 QSizePolicy.Policy.Expanding)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addLayout(cards)
        layout.addLayout(filter_row)
        layout.addWidget(self.empty)
        layout.addWidget(self.table)
        self.table.hide()

    def _make_card(self, caption: str, value: str) -> dict[str, QWidget]:
        wrap = QWidget()
        wrap.setObjectName("statTile")
        wrap.setMinimumWidth(180)
        wrap.setSizePolicy(QSizePolicy.Policy.Preferred,
                           QSizePolicy.Policy.Fixed)
        accent = KIND_COLORS.get(caption, theme.PRIMARY)
        wrap.setStyleSheet(
            f"QWidget#statTile {{ background: {theme.SURFACE}; "
            f"border-top: 2px solid {accent}; border-radius: 4px; }}"
        )
        v = QVBoxLayout(wrap)
        v.setContentsMargins(14, 10, 14, 10)
        v.setSpacing(4)
        cap = QLabel(caption.upper())
        cap.setProperty("role", "caption")
        val = QLabel(value)
        val.setProperty("role", "metric")
        v.addWidget(cap)
        v.addWidget(val)
        return {"wrap": wrap, "val": val}

    # --- public API ---------------------------------------------------

    def append(self, ev: dict) -> None:
        """Record a new interaction. ev keys: module, kind, who, detail."""
        row = {
            "time":   time.strftime("%H:%M:%S"),
            "module": str(ev.get("module", "-")),
            "kind":   str(ev.get("kind", "-")),
            "who":    str(ev.get("who", "")),
            "detail": str(ev.get("detail", "")),
        }
        self._rows.append(row)
        # Keep filter dropdowns aware of new modules.
        existing = {self.module_filter.itemText(i)
                    for i in range(self.module_filter.count())}
        if row["module"] not in existing:
            self.module_filter.addItem(row["module"])
        self._update_counter(row["kind"])
        self._maybe_show_table()
        if self._row_passes_filters(row):
            self._insert_row(row)

    def clear(self) -> None:
        self._rows.clear()
        self.table.setRowCount(0)
        for lbl in self._counters.values():
            lbl.setText("0")
        self.empty.show()
        self.table.hide()
        self.cleared.emit()

    # --- internals ----------------------------------------------------

    def _update_counter(self, kind: str) -> None:
        lbl = self._counters.get(kind)
        if lbl is not None:
            try:
                n = int(lbl.text())
            except ValueError:
                n = 0
            lbl.setText(str(n + 1))

    def _maybe_show_table(self) -> None:
        if not self._rows:
            return
        if self.empty.isVisible():
            self.empty.hide()
            self.table.show()

    def _row_passes_filters(self, row: dict[str, str]) -> bool:
        m = self.module_filter.currentText()
        if m != "(all)" and m != row["module"]:
            return False
        k = self.kind_filter.currentText()
        if k != "(all)" and k != row["kind"]:
            return False
        s = self.search.text().strip().lower()
        if s and not any(s in v.lower() for v in row.values()):
            return False
        return True

    def _refresh_rows(self) -> None:
        self.table.setRowCount(0)
        for r in self._rows:
            if self._row_passes_filters(r):
                self._insert_row(r)

    def _insert_row(self, row: dict[str, str]) -> None:
        self.table.setSortingEnabled(False)
        try:
            ridx = self.table.rowCount()
            self.table.insertRow(ridx)
            for cidx, col in enumerate(self._COLUMNS):
                item = QTableWidgetItem(row[col])
                item.setFlags(Qt.ItemFlag.ItemIsSelectable
                              | Qt.ItemFlag.ItemIsEnabled)
                if col == "kind":
                    color = KIND_COLORS.get(row[col], theme.TEXT_DIM)
                    item.setForeground(QBrush(QColor(color)))
                self.table.setItem(ridx, cidx, item)
            # Auto-scroll to newest unless user is sorting.
            self.table.scrollToBottom()
        finally:
            self.table.setSortingEnabled(True)

    def _export_csv(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Export interactions to CSV",
            "thewave32-interactions.csv",
            "CSV (*.csv)",
        )
        if not path:
            return
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(self._COLUMNS))
            w.writeheader()
            w.writerows(self._rows)


__all__ = ["InteractionsPanel", "KIND_COLORS"]
