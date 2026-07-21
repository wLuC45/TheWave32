"""ScanTargetViewer - shared kernel for viewers with a scan-results
table and a per-row "pick this AP" button.

wifi-deauth, wifi-bssid-clone, and wifi-handshake-capture all surface
the same pattern: receive `event:"ap"` lines, dedupe by `idx`, render
a 6-column table (#, SSID, BSSID, ch, RSSI, action). Subclasses
override the per-row button label and the command it sends.

Subclasses are expected to:

  * call ``self._build_scan_table(...)`` from their ``_build``
  * call ``self._handle_scan_event(obj)`` from ``on_json`` whenever
    ``obj.get("event") == "ap"``
  * leave ``self._row_for_idx`` and ``self._row_action_label``
    unshadowed
"""

from __future__ import annotations

from typing import Any, Optional

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QPushButton, QTableWidget, QTableWidgetItem,
)

from .base import BaseViewer


class ScanTargetViewer(BaseViewer):
    """Mixin-shaped base. Concrete subclasses override the two
    constants (label + command format) and call the helpers below."""

    #: Label shown on the per-row button.
    SCAN_ROW_LABEL: str = "target"

    #: Format string sent over serial when the user clicks a row's
    #: button. ``{idx}`` is replaced with the AP's scan index.
    SCAN_ROW_COMMAND: str = "target {idx}"

    #: Optional optimistic StatusStrip state set when the row button
    #: fires (e.g. "attacking" for wifi-deauth).
    SCAN_ROW_STATE: Optional[str] = None

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._row_for_idx: dict[int, int] = {}

    # --- table construction -----------------------------------------

    def _build_scan_table(self) -> QTableWidget:
        t = QTableWidget(0, 6)
        t.setHorizontalHeaderLabels(["#", "SSID", "BSSID", "ch", "RSSI", ""])
        t.horizontalHeader().setStretchLastSection(False)
        t.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        t.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        t.setColumnWidth(0, 40)
        t.setColumnWidth(2, 160)
        t.setColumnWidth(3, 50)
        t.setColumnWidth(4, 70)
        t.setColumnWidth(5, 110)
        self.scans = t
        return t

    # --- event handling ---------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        """Prune the scan table when a fresh scan reports fewer APs than
        are currently shown. The firmware re-emits `ap` rows on every
        stats poll, so rows are updated in place - but a *second*, shorter
        scan would otherwise leave the previous run's extra rows stale.
        Subclasses call ``super().on_json(obj)`` already."""
        super().on_json(obj)
        if obj.get("cmd") == "scan" and obj.get("ok"):
            try:
                self._prune_scan_rows(int(obj.get("count", 0)))
            except (TypeError, ValueError):
                pass

    def _prune_scan_rows(self, max_idx: int) -> None:
        """Drop rows whose scan index is above `max_idx` (the AP count
        of the latest scan) and rebuild the idx→row map, since removing
        rows shifts every row below it."""
        stale = [idx for idx in self._row_for_idx if idx > max_idx]
        if not stale:
            return
        for idx in sorted(stale, reverse=True):
            self.scans.removeRow(self._row_for_idx[idx])
        self._row_for_idx = {}
        for row in range(self.scans.rowCount()):
            cell = self.scans.item(row, 0)
            if cell is not None and cell.text().isdigit():
                self._row_for_idx[int(cell.text())] = row

    def _handle_scan_event(self, obj: dict[str, Any]) -> None:
        idx = int(obj.get("idx", 0))
        if idx <= 0:
            return
        is_new_row = idx not in self._row_for_idx
        if is_new_row:
            row = self.scans.rowCount()
            self.scans.insertRow(row)
            self._row_for_idx[idx] = row
        else:
            row = self._row_for_idx[idx]
        cells = [
            (str(idx), Qt.AlignmentFlag.AlignCenter),
            (str(obj.get("ssid", "")), None),
            (str(obj.get("bssid", "")), None),
            (str(obj.get("channel", "")), Qt.AlignmentFlag.AlignCenter),
            (str(obj.get("rssi", "")),
             Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter),
        ]
        for col, (val, align) in enumerate(cells):
            existing = self.scans.item(row, col)
            if existing is not None and existing.text() == val:
                continue
            it = QTableWidgetItem(val)
            it.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            if align is not None:
                it.setTextAlignment(align)
            self.scans.setItem(row, col, it)
        # Button is keyed by idx (stable per row); only create on insert
        # to avoid widget churn + lambda re-binding on every stats poll.
        if is_new_row:
            self.scans.setCellWidget(row, 5, self._make_scan_row_button(idx))

    def _make_scan_row_button(self, idx: int) -> QPushButton:
        btn = QPushButton(self.SCAN_ROW_LABEL)
        btn.setProperty("primary", True)
        self.register_action_button(btn)
        cmd = self.SCAN_ROW_COMMAND.format(idx=idx)
        label = f"{self.SCAN_ROW_LABEL} #{idx}"
        state = self.SCAN_ROW_STATE
        btn.clicked.connect(
            lambda _checked=False, c=cmd, s=state, lbl=label:
                self.fire(c, optimistic_state=s, label=lbl)
        )
        return btn


__all__ = ["ScanTargetViewer"]
