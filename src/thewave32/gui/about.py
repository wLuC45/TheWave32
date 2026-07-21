"""About / Home page - the static landing surface for TheWave32.

This is what the user sees before picking a module on the sidebar. It
is intentionally NOT a dashboard: data that already lives elsewhere in
the GUI (the sidebar lists every module, the bottom statusline shows
the connected device) is not duplicated here. The entry screen is just
the brand: logo, project title, a short description of what the
software is and does, and the author at the bottom.

Host callers historically populated ``set_device_info`` and
``set_modules``; both are kept as no-ops for backwards compatibility,
but the values they carried are now surfaced by the statusbar and the
sidebar respectively.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable

import PySide6.QtSvg  # noqa: F401 - register the SVG plugin for QPixmap
from PySide6.QtCore import Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QHBoxLayout, QLabel, QSizePolicy, QVBoxLayout, QWidget


_ASSETS = Path(__file__).parent / "assets"

# Description text. Plain prose, no em-dashes, no bullet lists, no
# marketing flourishes; the goal is to tell a first-time visitor what
# the project IS and what it DOES, in two short paragraphs.
_DESCRIPTION = (
    "TheWave32 is a research and educational platform built around "
    "the ESP32-S3. A Python host (CLI and GUI) drives 24 firmware "
    "modules that exercise the chip's Wi-Fi, BLE, ESP-NOW and "
    "USB-HID radios, with low-level access to monitor-mode frames, "
    "CSI samples and clock-skew measurements."
)
_DESCRIPTION_2 = (
    "Each module runs on the ESP32-S3 itself and streams structured "
    "JSON back over USB. The host parses the stream, surfaces it as "
    "live tables and charts, and can persist it for later analysis. "
    "The intent is to study radio behaviour in a lab setting: frame "
    "capture, protocol fingerprinting, signal analysis."
)


class AboutView(QWidget):
    """Static brand and about panel for the entry screen."""

    # Kept defined so any older import of these constants still works.
    CARD_WIDTH = 0
    LOGO_WIDTH = 0

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("AboutDashboard")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(48, 32, 48, 24)
        outer.setSpacing(18)

        # Vertical breathing room above the logo block.
        outer.addStretch(2)

        # --- big logo --------------------------------------------------
        self.lbl_brand_mark = QLabel()
        self.lbl_brand_mark.setObjectName("AboutBrandMark")
        wave = QPixmap(str(_ASSETS / "TheWave32.svg"))
        if not wave.isNull():
            self.lbl_brand_mark.setPixmap(
                wave.scaledToHeight(240, Qt.TransformationMode.SmoothTransformation)
            )
        self.lbl_brand_mark.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        outer.addWidget(self.lbl_brand_mark, 0, Qt.AlignmentFlag.AlignHCenter)

        # --- project title --------------------------------------------
        self.lbl_brand_title = QLabel("TheWave32")
        self.lbl_brand_title.setObjectName("AboutBrandTitle")
        self.lbl_brand_title.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        outer.addWidget(self.lbl_brand_title)

        # --- description (two short paragraphs) -----------------------
        # Centered wrapped text: Qt's heightForWidth on a QLabel does
        # not play well with AlignHCenter on the parent layout, so each
        # paragraph lives in its own HBox with stretches on the sides.
        # The wrapper sets a fixed text width and lets the QLabel grow
        # vertically to fit the wrapped content.
        self.lbl_about_body_1 = self._make_paragraph(_DESCRIPTION)
        outer.addLayout(self._wrap_centered(self.lbl_about_body_1))

        self.lbl_about_body_2 = self._make_paragraph(_DESCRIPTION_2)
        outer.addLayout(self._wrap_centered(self.lbl_about_body_2))

        # Push the author line down to the bottom.
        outer.addStretch(3)

        # --- author ---------------------------------------------------
        self.lbl_author = QLabel("v0rtex")
        self.lbl_author.setObjectName("AboutAuthor")
        self.lbl_author.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        outer.addWidget(self.lbl_author)

    # --- helpers --------------------------------------------------------

    @staticmethod
    def _make_paragraph(text: str) -> QLabel:
        lbl = QLabel(text)
        lbl.setObjectName("AboutBody")
        lbl.setWordWrap(True)
        lbl.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        lbl.setFixedWidth(680)
        lbl.setSizePolicy(
            QSizePolicy.Policy.Fixed, QSizePolicy.Policy.MinimumExpanding
        )
        return lbl

    @staticmethod
    def _wrap_centered(widget: QLabel) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.addStretch(1)
        row.addWidget(widget)
        row.addStretch(1)
        return row

    # --- host-side hooks: kept as no-ops --------------------------------
    #
    # The entry screen no longer mirrors device or module info; the
    # statusline shows the device and the sidebar lists the modules.
    # Older code paths still call these, so they accept the same
    # arguments and silently do nothing.

    def set_device_info(self, **_kwargs: object) -> None:
        return None

    def set_modules(self, _modules: Iterable) -> None:
        return None


__all__ = ["AboutView"]
