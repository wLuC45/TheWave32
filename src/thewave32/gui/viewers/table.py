"""Reusable TableViewer base.

Subclass and set ``EVENT``, ``COLUMNS``, optionally ``KEY_COLUMN`` and
override ``row_for`` to customise the per-row rendering. The table
deduplicates rows by ``KEY_COLUMN`` and updates RSSI/last-seen in
place - perfect for scanner/probe-logger/airtag-finder/etc.
"""

from __future__ import annotations

import time
from typing import Any, Sequence

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QPushButton, QSizePolicy, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget,
)

from ..flow_layout import FlowLayout
from .base import BaseViewer


class TableViewer(BaseViewer):
    EVENT: str = ""           # event name to filter on
    COLUMNS: Sequence[str] = ()
    KEY_COLUMN: str | None = None  # event field used to dedupe (e.g. "bssid")
    ACTIONS: Sequence[str] = ("start", "stop", "stats")
    HELP: str = ""

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._row_index: dict[str, int] = {}
        self._build()

    def _build(self) -> None:
        # Wrapping row: on a narrow window the buttons flow onto a second
        # line instead of overflowing / clipping past the right edge.
        actions_row = QWidget()
        actions_row.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        actions = FlowLayout(actions_row, margin=0, hspacing=6, vspacing=4)
        for cmd in self.ACTIONS:
            primary = cmd == "start"
            opt = None
            if cmd == "start":
                opt = "running"
            elif cmd == "stop":
                opt = "stopped"
            elif cmd == "scan":
                opt = "scanning"
            actions.addWidget(self.make_action_button(cmd, primary=primary, optimistic_state=opt))
        clear = QPushButton("clear")
        clear.clicked.connect(self._clear)
        # Mark as a secondary action; the appearance agent owns the
        # QPushButton[secondary="true"] QSS rule. Re-polish in case the
        # widget was already polished when the property is set.
        clear.setProperty("secondary", "true")
        clear.style().unpolish(clear)
        clear.style().polish(clear)
        actions.addWidget(clear)

        # Subclass hook: additional controls (e.g. filter rows). The
        # default returns None - most table viewers don't need any.
        extras_layout = self._build_extra_controls()

        self.table = QTableWidget(0, len(self.COLUMNS))
        self.table.setHorizontalHeaderLabels(list(self.COLUMNS))
        self.table.setSortingEnabled(True)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.horizontalHeader().setStretchLastSection(True)
        # Let the table consume the freed vertical slack rather than the
        # action row or status strip.
        self.table.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        layout.addWidget(self.make_status_strip())
        layout.addWidget(actions_row)
        if extras_layout is not None:
            layout.addLayout(extras_layout)
        # Inline HELP intentionally NOT rendered here - the per-module
        # "Sobre" button on the module card opens a dialog with the same
        # text (see BaseViewer.show_about_dialog).
        layout.addWidget(self.table, 1)

    def _build_extra_controls(self):
        """Hook for subclasses that need a second control row (e.g. RSSI
        / MAC / name filters). Default: no extras."""
        return None

    def _clear(self) -> None:
        self.table.setRowCount(0)
        self._row_index.clear()

    def row_for(self, obj: dict[str, Any]) -> list[str]:
        """Return ordered cell strings for one row. Default: pull keys
        whose names match the column titles (case-sensitive)."""
        return [str(obj.get(col, "")) for col in self.COLUMNS]

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        if self._status is not None and obj.get("event") == self.EVENT:
            # Live row counter on the strip - visible "data is arriving".
            self._status.record(f"{self.EVENT}",
                                detail=f"rows={self.table.rowCount() + 1}")
        if obj.get("event") != self.EVENT:
            return
        cells = self.row_for(obj)
        key = obj.get(self.KEY_COLUMN) if self.KEY_COLUMN else None
        self.table.setSortingEnabled(False)
        try:
            if key and key in self._row_index:
                row = self._row_index[key]
                for i, val in enumerate(cells):
                    self.table.setItem(row, i, _make_item(val))
            else:
                row = self.table.rowCount()
                self.table.insertRow(row)
                for i, val in enumerate(cells):
                    self.table.setItem(row, i, _make_item(val))
                if key:
                    self._row_index[key] = row
        finally:
            self.table.setSortingEnabled(True)


def _make_item(val: str) -> QTableWidgetItem:
    item = QTableWidgetItem(val)
    item.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
    # Right-align if it parses as int - looks better for RSSI / channels.
    try:
        int(val)
        item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
    except (TypeError, ValueError):
        pass
    return item
