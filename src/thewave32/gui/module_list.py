"""Sidebar - full module slugs with visually distinct category separators.

Previously the prefix was stripped ("wifi-scanner" → "scanner"), which
made it impossible to tell which family a row belonged to once you'd
lost the context of the category header. Now the row text shows the
full slug; the category lives on its own surface tint as a non-selectable
section divider styled by the QSS rule for ``QListWidget::item:disabled``.
"""

from __future__ import annotations

from typing import Iterable

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QFont
from PySide6.QtWidgets import QListWidget, QListWidgetItem

from thewave32.registry import Module


# Modules that need to *associate* to a Wi-Fi AP first (or that operate
# from inside a network) - the user asked for a separate sidebar bucket
# so they don't get mixed in with the offline RF/scan tools.
_WIFI_IN_NETWORK = {
    "net-port-scanner",
}
# BLE modules that only make sense once a host has paired with the
# device (or vice-versa).
_BLE_CONNECTED = {
    "ble-hid-keyboard",
}


def _category(slug: str) -> str:
    if slug.startswith("ble-"):
        return "BLE · Connected" if slug in _BLE_CONNECTED else "BLE · On-Air"
    if slug.startswith("wifi-"):
        return "Wi-Fi · In-Network" if slug in _WIFI_IN_NETWORK else "Wi-Fi · On-Air"
    if slug.startswith("usb-"):
        return "USB · Host"
    if slug.startswith("net-"):
        return "Wi-Fi · In-Network"
    if slug.startswith("espnow-"):
        return "ESP-NOW"
    if slug.startswith("spectrum"):
        return "RF / Spectrum"
    return "Other"


_CATEGORY_ORDER = [
    "Wi-Fi · On-Air",
    "Wi-Fi · In-Network",
    "BLE · On-Air",
    "BLE · Connected",
    "RF / Spectrum",
    "USB · Host",
    "ESP-NOW",
    "Other",
]


class ModuleList(QListWidget):
    """Module browser. Emits ``module_selected(slug)`` on activation."""

    module_selected = Signal(str)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setSelectionMode(QListWidget.SelectionMode.SingleSelection)
        self.setFrameShape(QListWidget.Shape.NoFrame)
        # When the sidebar is squeezed (narrow window) a long slug used to
        # spawn a horizontal scrollbar and clip the row. Suppress the
        # horizontal scrollbar and elide the text instead so rows stay
        # readable at any sidebar width.
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setTextElideMode(Qt.TextElideMode.ElideRight)
        self.setWordWrap(False)
        self.itemActivated.connect(self._emit_selected)
        self.itemClicked.connect(self._emit_selected)

    def populate(self, modules: Iterable[Module]) -> None:
        mods = sorted(modules, key=lambda m: (
            _CATEGORY_ORDER.index(_category(m.name))
            if _category(m.name) in _CATEGORY_ORDER else 99,
            m.name,
        ))
        self.clear()
        cur_cat: str | None = None
        for m in mods:
            cat = _category(m.name)
            if cat != cur_cat:
                cur_cat = cat
                hdr = QListWidgetItem(cat.upper())
                # NoItemFlags makes the row both unselectable and unhoverable;
                # the QSS uses ``:disabled`` to paint it as a section divider.
                hdr.setFlags(Qt.ItemFlag.NoItemFlags)
                hf = QFont()
                hf.setPointSize(8)
                hf.setLetterSpacing(QFont.SpacingType.PercentageSpacing, 130)
                hf.setWeight(QFont.Weight.DemiBold)
                hdr.setFont(hf)
                self.addItem(hdr)
            # Module row: full slug so the category context is never
            # ambiguous if the header scrolls out of view.
            item = QListWidgetItem(m.name)
            item.setData(Qt.ItemDataRole.UserRole, m.name)
            # Full slug (+ version) first so the elided row text is always
            # recoverable on hover, then the description for context.
            item.setToolTip(
                f"<b>{m.name}</b> v{m.version}<br>{m.description}"
            )
            self.addItem(item)

    def filter(self, text: str) -> None:
        """Hide module rows whose slug does not contain ``text`` (case
        insensitive). Category headers are hidden when every module under
        them is filtered out, so the list never shows an orphan section."""
        needle = text.strip().lower()
        # First pass: show/hide module rows; track which categories keep
        # at least one visible module.
        last_header: QListWidgetItem | None = None
        header_has_visible: dict[int, bool] = {}
        header_rows: list[int] = []
        for i in range(self.count()):
            item = self.item(i)
            is_header = item.flags() == Qt.ItemFlag.NoItemFlags
            if is_header:
                last_header = item
                header_rows.append(i)
                header_has_visible[i] = False
                continue
            slug = item.data(Qt.ItemDataRole.UserRole) or ""
            visible = needle in str(slug).lower()
            item.setHidden(not visible)
            if visible and header_rows:
                header_has_visible[header_rows[-1]] = True
        # Second pass: hide empty category headers.
        for row in header_rows:
            self.item(row).setHidden(not header_has_visible.get(row, False))

    def _emit_selected(self, item: QListWidgetItem) -> None:
        slug = item.data(Qt.ItemDataRole.UserRole)
        if slug:
            self.module_selected.emit(slug)
