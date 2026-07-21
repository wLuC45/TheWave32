"""CSI heatmap viewer (subcarrier × time, magnitude)."""

from __future__ import annotations

import struct
from typing import Any

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox, QVBoxLayout,
)

from thewave32.gui import theme

from .base import BaseViewer

CSI_MAGIC = b"TCSI"
HEADER_LEN = 20
MAX_SUBCARRIERS = 192
TIME_WINDOW = 240


class CSIHeatmapViewer(BaseViewer):
    """Reassembles the binary CSI stream emitted after ``start`` and
    plots a scrolling magnitude heatmap (subcarrier index × time)."""

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._buf = bytearray()
        self._image = np.zeros((MAX_SUBCARRIERS, TIME_WINDOW), dtype=np.float32)
        self._n_subc = 0
        self._sample_count = 0
        self._streaming = False
        self._build()

    # ------------------------------------------------------------------

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        self.btn_start = QPushButton("start")
        self.btn_start.setProperty("primary", True)
        self.btn_start.clicked.connect(self._start_stream)
        self.btn_stop = QPushButton("stop")
        self.btn_stop.clicked.connect(self._stop_stream)
        self.register_action_button(self.btn_start)
        self.register_action_button(self.btn_stop)
        actions.addWidget(self.btn_start)
        actions.addWidget(self.btn_stop)
        actions.addWidget(self.make_action_button("stats"))
        actions.addSpacing(20)
        actions.addWidget(QLabel("chan"))
        self.chan = QSpinBox()
        self.chan.setRange(1, 14)
        self.chan.setValue(6)
        actions.addWidget(self.chan)
        ap_chan = QPushButton("apply")
        ap_chan.clicked.connect(lambda: self.send_command.emit(f"chan {self.chan.value()}"))
        actions.addWidget(ap_chan)
        actions.addSpacing(20)
        actions.addWidget(QLabel("filter MAC"))
        self.filter_mac = QLineEdit()
        self.filter_mac.setPlaceholderText("aa:bb:cc:dd:ee:ff or 'any'")
        actions.addWidget(self.filter_mac, 1)
        ap_f = QPushButton("apply")
        ap_f.clicked.connect(self._apply_filter)
        self.register_action_button(ap_f)
        actions.addWidget(ap_f)
        actions.addSpacing(20)
        actions.addWidget(QLabel("RSSI ≥"))
        self.rssi_min = QSpinBox()
        self.rssi_min.setRange(-127, 0)
        self.rssi_min.setSuffix(" dBm")
        self.rssi_min.setValue(-127)
        actions.addWidget(self.rssi_min)
        ap_r = QPushButton("apply")
        ap_r.clicked.connect(
            lambda: self.send_command.emit(f"rssi_min {self.rssi_min.value()}")
        )
        self.register_action_button(ap_r)
        actions.addWidget(ap_r)
        actions.addSpacing(20)
        actions.addWidget(QLabel("rate"))
        self.rate_limit = QSpinBox()
        self.rate_limit.setRange(0, 1000)
        self.rate_limit.setSuffix(" Hz")
        self.rate_limit.setValue(0)
        self.rate_limit.setToolTip("0 = no limit")
        actions.addWidget(self.rate_limit)
        ap_rate = QPushButton("apply")
        ap_rate.clicked.connect(
            lambda: self.send_command.emit(f"rate_limit {self.rate_limit.value()}")
        )
        self.register_action_button(ap_rate)
        actions.addWidget(ap_rate)

        self._info = QLabel("Click <b>start</b> to begin streaming CSI.")
        self._info.setProperty("secondary", True)

        # Image display configured for natural orientation:
        # X = time (newest at right), Y = subcarrier index (0 at bottom).
        self.image_view = pg.ImageView(view=pg.PlotItem())
        self.image_view.ui.histogram.hide()
        self.image_view.ui.roiBtn.hide()
        self.image_view.ui.menuBtn.hide()
        plot = self.image_view.getView()
        plot.setLabel("left", "Subcarrier",
                      **{"color": theme.TEXT_DIM, "font-size": "10pt"})
        plot.setLabel("bottom", "Time (newer →)",
                      **{"color": theme.TEXT_DIM, "font-size": "10pt"})
        plot.invertY(False)        # subcarrier 0 at bottom
        plot.setMouseEnabled(x=False, y=False)
        plot.hideButtons()
        # Inferno-ish colour map (low = near-black, high = warm).
        cmap = pg.ColorMap(
            pos=np.array([0.0, 0.25, 0.55, 0.8, 1.0]),
            color=np.array([
                [10, 10, 12, 255],
                [60, 30, 90, 255],
                [167, 139, 250, 255],   # accent (violet)
                [240, 200, 100, 255],
                [255, 245, 200, 255],
            ], dtype=np.uint8),
        )
        self.image_view.setColorMap(cmap)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addWidget(self._info)
        layout.addWidget(self.image_view, 1)

    def _apply_filter(self) -> None:
        mac = self.filter_mac.text().strip() or "any"
        self.send_command.emit(f"filter {mac}")

    def _start_stream(self) -> None:
        # Send `start` first; flip to binary mode only AFTER the ack comes
        # back (handled in on_json) so we don't swallow the ack itself.
        if self._status is not None:
            self._status.record("start")
        self.send_command.emit("start")

    def _stop_stream(self) -> None:
        # Flip OUT of binary mode immediately so the stop ack reaches the
        # console as parsed JSON. Optimistic state update.
        if self._status is not None:
            self._status.record("stop")
            self._status.set_state("stopped")
        self.request_binary_mode(False)
        self._streaming = False
        self._buf.clear()
        self.send_command.emit("stop")

    # ------------------------------------------------------------------
    # Data plumbing
    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        cmd = obj.get("cmd")
        if cmd == "start" and obj.get("ok") is True:
            self._streaming = True
            self.request_binary_mode(True)
            if self._status is not None:
                self._status.set_state("streaming")
            # Usage hint only - run state is on the StatusStrip.
            self._info.setText(
                "Use the Console tab to send commands while the heatmap streams."
            )
            return
        if cmd == "start" and obj.get("ok") is False:
            self._info.setText(f"start failed: {obj.get('err','?')}")
            return
        if cmd == "stop" and obj.get("ok") is True:
            self._streaming = False
            self.request_binary_mode(False)
            return
        if cmd == "stats":
            self._info.setText(
                f"samples_emitted={obj.get('samples_emitted','?')} · "
                f"out_of_band={obj.get('out_of_band','?')} · "
                f"chan={obj.get('channel','?')} · "
                f"streaming={self._streaming}"
            )

    def on_raw(self, data: bytes) -> None:
        self._buf.extend(data)
        if len(self._buf) > 1 << 20:
            del self._buf[: len(self._buf) // 2]
        while True:
            idx = self._buf.find(CSI_MAGIC)
            if idx < 0:
                if len(self._buf) > 4:
                    del self._buf[: len(self._buf) - 3]
                return
            if idx > 0:
                del self._buf[:idx]
            if len(self._buf) < HEADER_LEN:
                return
            try:
                magic, ts_ms, *_mac, _rssi, _chan, _sig, _bw, length = \
                    struct.unpack_from("<I I 6B b B B B H", self._buf, 0)
            except struct.error:
                return
            if magic != int.from_bytes(CSI_MAGIC, "little"):
                del self._buf[:1]
                continue
            total = HEADER_LEN + length
            if length == 0 or length > MAX_SUBCARRIERS * 2:
                del self._buf[:1]
                continue
            if len(self._buf) < total:
                return
            payload = bytes(self._buf[HEADER_LEN:total])
            del self._buf[:total]
            self._consume_sample(payload, length)
            self._sample_count += 1
            if self._sample_count % 6 == 0:
                self._refresh_image()

    def _consume_sample(self, payload: bytes, length: int) -> None:
        arr = np.frombuffer(payload, dtype=np.int8)
        n_pairs = length // 2
        if n_pairs == 0:
            return
        i = arr[1:n_pairs * 2:2].astype(np.float32)
        q = arr[0:n_pairs * 2:2].astype(np.float32)
        mag = np.sqrt(i * i + q * q)
        if n_pairs > MAX_SUBCARRIERS:
            mag = mag[:MAX_SUBCARRIERS]
            n_pairs = MAX_SUBCARRIERS
        self._n_subc = max(self._n_subc, n_pairs)
        # Roll columns left by one and write the new sample on the right.
        self._image[:, :-1] = self._image[:, 1:]
        self._image[:n_pairs, -1] = mag
        if n_pairs < self._image.shape[0]:
            self._image[n_pairs:, -1] = 0

    def _refresh_image(self) -> None:
        sub = self._image[: self._n_subc, :]
        if sub.size == 0:
            return
        # `sub` is shape (n_subc, time). pyqtgraph's setImage expects
        # (time, subcarrier) so the last axis becomes Y. With invertY(False)
        # subcarrier 0 ends up at the bottom - natural orientation.
        self.image_view.setImage(
            sub.T,
            autoLevels=False,
            levels=(0, max(60.0, float(sub.max()))),
            autoRange=False,
        )
