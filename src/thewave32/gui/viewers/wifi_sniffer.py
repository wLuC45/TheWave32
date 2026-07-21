"""Custom viewer for wifi-sniffer - handles the binary PCAP stream."""

from __future__ import annotations

import struct
import time
from collections import deque
from typing import Any

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QPushButton, QSpinBox, QVBoxLayout, QWidget,
)

from thewave32.gui import theme

from .base import BaseViewer

PCAP_MAGIC = 0xA1B2C3D4
PCAP_GLOBAL_LEN = 24
PCAP_REC_HDR_LEN = 16


class WifiSnifferViewer(BaseViewer):
    """The sniffer emits a JSON ack for ``start``, then a PCAP global
    header, then PCAP records until ``stop``. We parse the records
    on-the-fly and plot a frames-per-second line so the user has live
    feedback that capture is happening."""

    HISTORY = 120

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._streaming = False
        self._buf = bytearray()
        self._got_global = False
        self._frames = 0
        self._bytes = 0
        self._last_frames = 0
        self._fps_history: deque[float] = deque(maxlen=self.HISTORY)
        self._build()
        self._tick = QTimer(self)
        self._tick.timeout.connect(self._update_rate)
        self._tick.start(1000)

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("start capture", "start", primary=True, optimistic_state="capturing"))
        self.btn_stop = QPushButton("stop")
        self.btn_stop.clicked.connect(self._stop)
        self.register_action_button(self.btn_stop)
        actions.addWidget(self.btn_stop)
        actions.addWidget(self.make_action_button("stats"))
        actions.addSpacing(20)
        actions.addWidget(QLabel("channel"))
        self.chan = QSpinBox()
        self.chan.setRange(1, 14)
        self.chan.setValue(1)
        actions.addWidget(self.chan)
        ap = QPushButton("apply")
        ap.clicked.connect(lambda: self.send_command.emit(f"chan {self.chan.value()}"))
        actions.addWidget(ap)
        actions.addStretch(1)

        # Stats row. `state` is on the StatusStrip and `frames` is the
        # frames/s plot below - neither is repeated here.
        stats_row = QHBoxLayout()
        stats_row.setSpacing(28)
        self.lbl_bytes   = self._stat("0 B")
        self.lbl_chan    = self._stat("-")
        self.lbl_dropped = self._stat("0")
        stats_row.addWidget(self._cap("bytes",   self.lbl_bytes))
        stats_row.addWidget(self._cap("channel", self.lbl_chan))
        stats_row.addWidget(self._cap("dropped", self.lbl_dropped))
        stats_row.addStretch(1)

        # FPS sparkline.
        self.plot = pg.PlotWidget()
        self.plot.setLabel("left", "frames/s",
                           **{"color": theme.TEXT_DIM, "font-size": "10pt"})
        self.plot.showGrid(x=False, y=True, alpha=0.15)
        self.plot.setMouseEnabled(x=False, y=False)
        self.plot.hideButtons()
        self.plot.getAxis("left").setTextPen(pg.mkPen(theme.TEXT_MUTED))
        self.plot.getAxis("bottom").setTextPen(pg.mkPen(theme.TEXT_MUTED))
        self._curve = self.plot.plot(pen=pg.mkPen(theme.ACCENT, width=2))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addLayout(stats_row)
        layout.addWidget(self.plot, 1)
        layout.addWidget(self._helper())

    def _stat(self, text: str, color: str = theme.TEXT) -> QLabel:
        lbl = QLabel(text)
        lbl.setStyleSheet(f"color: {color}; font-size: 14pt;")
        return lbl

    def _cap(self, caption: str, widget: QWidget) -> QWidget:
        wrap = QWidget()
        v = QVBoxLayout(wrap)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(2)
        cap = QLabel(caption.upper())
        cap.setStyleSheet(f"color: {theme.TEXT_MUTED}; font-size: 8pt; letter-spacing: 1.2px;")
        v.addWidget(cap)
        v.addWidget(widget)
        return wrap

    def _helper(self) -> QLabel:
        msg = QLabel(
            f"<span style='color:{theme.TEXT_MUTED};'>"
            "Captured frames are forwarded as raw PCAP. To save them, pipe the "
            "CLI's serial output through a script - the GUI only counts frames "
            "for live feedback."
            "</span>"
        )
        msg.setWordWrap(True)
        return msg

    def _stop(self) -> None:
        if self._status is not None:
            self._status.record("stop")
            self._status.set_state("stopped")
        self.request_binary_mode(False)
        self._streaming = False
        self.send_command.emit("stop")

    def _update_rate(self) -> None:
        rate = max(0.0, self._frames - self._last_frames)
        self._last_frames = self._frames
        self._fps_history.append(rate)
        self._curve.setData(list(self._fps_history))
        self.lbl_bytes.setText(_human_bytes(self._bytes))

    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        cmd = obj.get("cmd")
        if cmd == "start" and obj.get("ok") is True:
            self._streaming = True
            self._buf.clear()
            self._got_global = False
            self._frames = 0
            self._bytes = 0
            self._last_frames = 0
            self._fps_history.clear()
            if self._status is not None:
                self._status.set_state("capturing")
            self.request_binary_mode(True)
            return
        if cmd == "stop" and obj.get("ok") is True:
            self._streaming = False
            self.request_binary_mode(False)
            return
        if cmd == "stats":
            self.lbl_chan.setText(str(obj.get("channel", "-")))
            self.lbl_dropped.setText(str(obj.get("dropped", 0)))

    def on_raw(self, data: bytes) -> None:
        if not self._streaming:
            return
        self._buf.extend(data)
        if not self._got_global:
            if len(self._buf) < PCAP_GLOBAL_LEN:
                return
            magic = struct.unpack_from("<I", self._buf, 0)[0]
            if magic != PCAP_MAGIC:
                # Fell out of sync - drop one byte and retry.
                del self._buf[:1]
                return
            del self._buf[:PCAP_GLOBAL_LEN]
            self._got_global = True
        # Drain records.
        while len(self._buf) >= PCAP_REC_HDR_LEN:
            ts_sec, ts_us, incl_len, orig_len = struct.unpack_from("<IIII", self._buf, 0)
            if incl_len > 65535:        # corrupt
                del self._buf[:1]
                continue
            total = PCAP_REC_HDR_LEN + incl_len
            if len(self._buf) < total:
                return
            del self._buf[:total]
            self._frames += 1
            self._bytes += total


def _human_bytes(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if isinstance(n, float) else f"{n} {unit}"
        n /= 1024.0
    return f"{n}"
