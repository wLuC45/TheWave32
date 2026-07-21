"""Custom viewer for wifi-handshake-capture.

Firmware CLI:
  scan                     → list nearby APs (sets `target N` cache)
  target <bssid|N|any>     → pick AP to follow
  chan N                   → manual channel override
  start / stop             → 4-way handshake observation
  stats                    → eapol counters + state

The previous CounterViewer surface had no AP picker, no `target`, no
`chan`. The user couldn't actually steer the capture from the GUI.
"""

from __future__ import annotations

from collections import deque
from typing import Any

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QPushButton, QSizePolicy, QSpinBox, QSplitter,
    QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
)

from thewave32.gui import theme

from ._scan_target import ScanTargetViewer


class WifiHandshakeCaptureViewer(ScanTargetViewer):
    HISTORY = 180
    SCAN_ROW_LABEL = "target"
    SCAN_ROW_COMMAND = "target {idx}"

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._eapol_history: deque[float] = deque(maxlen=self.HISTORY)
        self._last_eapol = 0
        self._build()

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("scan", primary=True,
                                                  optimistic_state="scanning"))
        actions.addWidget(self.make_action_button("start", primary=True,
                                                  optimistic_state="capturing"))
        actions.addWidget(self.make_action_button("stop",
                                                  optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("stats"))
        actions.addSpacing(20)
        any_btn = QPushButton("target any")
        any_btn.setToolTip("Capture handshakes from any AP on the current channel.")
        any_btn.clicked.connect(lambda: self.fire("target any", label="target any"))
        self.register_action_button(any_btn)
        actions.addWidget(any_btn)
        actions.addSpacing(16)
        actions.addWidget(QLabel("channel"))
        self.chan = QSpinBox()
        self.chan.setRange(1, 14)
        self.chan.setValue(6)
        actions.addWidget(self.chan)
        chan_btn = QPushButton("apply ch")
        chan_btn.clicked.connect(
            lambda: self.fire(f"chan {self.chan.value()}",
                              label=f"chan {self.chan.value()}")
        )
        self.register_action_button(chan_btn)
        actions.addWidget(chan_btn)
        actions.addStretch(1)

        # --- AP table (shared kernel) ---------------------------------
        self._build_scan_table()

        # --- stats cards -----------------------------------------------
        cards_row = QHBoxLayout()
        cards_row.setSpacing(12)
        # `eapol_seen` is the EAPOL/s plot below; `running` is on the
        # StatusStrip - only the non-duplicated cards stay.
        self._cards = {
            "handshakes_complete":  self._make_card("4-WAY DONE", "0"),
            "channel":              self._make_card("CHANNEL", "-"),
        }
        for c in self._cards.values():
            cards_row.addWidget(c["wrap"])
        cards_row.addStretch(1)

        # --- per-message progress (M1..M4) ------------------------------
        progress_row = QHBoxLayout()
        progress_row.setSpacing(12)
        self._msg_lbls: dict[str, QLabel] = {}
        for k in ("m1_seen", "m2_seen", "m3_seen", "m4_seen"):
            tag = k.split("_")[0].upper()
            wrap = QWidget()
            wrap.setStyleSheet(
                f"QWidget {{ background: {theme.SURFACE}; "
                f"border-radius: 6px; padding: 8px 14px; min-width: 90px; }}"
            )
            v = QVBoxLayout(wrap)
            v.setContentsMargins(8, 4, 8, 4)
            v.setSpacing(2)
            cap = QLabel(tag)
            cap.setStyleSheet(
                f"color: {theme.TEXT_MUTED}; font-size: 8pt; "
                f"letter-spacing: 1.2px; font-weight: 600; background: transparent;"
            )
            val = QLabel("0")
            val.setStyleSheet(
                f"color: {theme.TEXT}; "
                f"font-size: 14pt; background: transparent;"
            )
            v.addWidget(cap)
            v.addWidget(val)
            self._msg_lbls[k] = val
            progress_row.addWidget(wrap)
        progress_row.addStretch(1)

        # --- EAPOL rate plot --------------------------------------------
        self.plot = pg.PlotWidget()
        self.plot.setMaximumHeight(150)
        self.plot.setLabel("left", "EAPOL/s",
                           **{"color": theme.TEXT_DIM, "font-size": "9pt"})
        self.plot.showGrid(x=False, y=True, alpha=0.15)
        self.plot.setMouseEnabled(x=False, y=False)
        self.plot.hideButtons()
        for ax in (self.plot.getAxis("left"), self.plot.getAxis("bottom")):
            ax.setTextPen(pg.mkPen(theme.TEXT_MUTED))
            ax.setPen(pg.mkPen(theme.BORDER, width=1))
        self._curve = self.plot.plot(pen=pg.mkPen(theme.ACCENT, width=2))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        scans_label = QLabel(
            "Scanned APs (click a row's `target` to follow that BSSID)"
        )
        scans_label.setProperty("heading", True)
        layout.addWidget(scans_label)
        layout.addWidget(self.scans, 2)
        layout.addLayout(cards_row)
        layout.addLayout(progress_row)
        layout.addWidget(self.plot)

        self._poll = QTimer(self)
        self._poll.timeout.connect(lambda: self.send_command.emit("stats"))

    def _make_card(self, caption: str, value: str) -> dict[str, QWidget]:
        wrap = QWidget()
        wrap.setMinimumWidth(160)
        wrap.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        wrap.setStyleSheet(
            f"QWidget {{ background: {theme.SURFACE}; "
            f"border-left: 3px solid {theme.ACCENT}; "
            f"border-top-right-radius: 6px; border-bottom-right-radius: 6px; "
            f"padding: 10px 14px; }}"
        )
        v = QVBoxLayout(wrap)
        v.setContentsMargins(10, 8, 10, 8)
        v.setSpacing(4)
        cap = QLabel(caption)
        cap.setStyleSheet(
            f"color: {theme.TEXT_MUTED}; font-size: 8pt; letter-spacing: 1.4px; "
            f"font-weight: 600; background: transparent;"
        )
        val = QLabel(value)
        val.setStyleSheet(
            f"color: {theme.TEXT}; "
            f"font-size: 22pt; font-weight: 500; background: transparent;"
        )
        v.addWidget(cap)
        v.addWidget(val)
        return {"wrap": wrap, "val": val}

    def set_actions_enabled(self, on: bool) -> None:
        super().set_actions_enabled(on)
        if on:
            self._poll.start(1000)
        else:
            self._poll.stop()

    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        evt = obj.get("event")
        cmd = obj.get("cmd")

        if evt == "ap":
            self._handle_scan_event(obj)
            return

        if cmd == "stats":
            eapol = int(obj.get("eapol_seen", 0))
            self._cards["handshakes_complete"]["val"].setText(
                str(obj.get("handshakes_complete", 0))
            )
            self._cards["channel"]["val"].setText(str(obj.get("channel", "-")))
            running = bool(obj.get("running"))
            for k, lbl in self._msg_lbls.items():
                lbl.setText(str(obj.get(k, 0)))
            if self._status is not None:
                self._status.set_state("capturing" if running else "stopped")
            rate = max(0.0, eapol - self._last_eapol)
            self._last_eapol = eapol
            self._eapol_history.append(rate)
            self._curve.setData(list(self._eapol_history))
            return

    # _handle_scan_event + _make_scan_row_button live in ScanTargetViewer.
