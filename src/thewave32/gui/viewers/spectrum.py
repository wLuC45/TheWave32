"""Scrolling waterfall heatmap for spectrum-analyzer.

One view, no duplication: a pyqtgraph ImageItem fed from a pandas
rolling window of the last N sweeps. Colour intensity is signal
strength, time flows left to right, channels stack top to bottom. The
newest sweep is the rightmost column, so the live state and the recent
history are both read off the same chart.

`pandas` drives the rolling window because DataFrame slicing and index
alignment are cleaner than juggling raw numpy rings.
"""

from __future__ import annotations

import json
import time
from typing import Any

import numpy as np
import pandas as pd
import pyqtgraph as pg
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QPushButton, QSpinBox, QVBoxLayout,
)

from thewave32.gui import theme

from .base import BaseViewer

CH_MIN, CH_MAX = 1, 13
N_CH = CH_MAX - CH_MIN + 1
RSSI_FLOOR = -100
WATERFALL_DEPTH = 240  # ~2 minutes at one sweep / 500 ms


class SpectrumViewer(BaseViewer):
    """2.4 GHz channel-occupancy waterfall, updated on every
    ``event:"sweep"``. Warmer colour means a stronger signal; the
    rightmost column is the latest sweep."""

    # EMA factor: 0.0 = freeze, 1.0 = no smoothing. 0.35 follows fast
    # transitions but kills single-sweep jitter.
    EMA_ALPHA = 0.35

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._avgs = np.full(N_CH, RSSI_FLOOR, dtype=np.float32)
        self._first_sweep = True
        # Rolling window: rows = sweep timestamps, columns = channel ids.
        self._history: pd.DataFrame = pd.DataFrame(
            columns=list(range(CH_MIN, CH_MAX + 1)),
            dtype=np.float32,
        )
        self._build()

    # ------------------------------------------------------------------

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("start", primary=True, optimistic_state="running"))
        actions.addWidget(self.make_action_button("stop", optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("stats"))
        # Group separator: between the start/stop/stats cluster and the
        # first spinbox group. Each group below uses an inner addSpacing(8)
        # between the spinbox and its `apply` button so the apply doesn't
        # collide with the spinbox stepper arrows.
        actions.addSpacing(16)
        actions.addWidget(QLabel("dwell"))
        self.dwell = QSpinBox()
        self.dwell.setRange(5, 5000)
        self.dwell.setSuffix(" ms")
        self.dwell.setValue(50)
        actions.addWidget(self.dwell)
        actions.addSpacing(8)
        apply_dw = QPushButton("apply")
        apply_dw.clicked.connect(lambda: self.send_command.emit(f"dwell {self.dwell.value()}"))
        actions.addWidget(apply_dw)
        actions.addSpacing(16)
        actions.addWidget(QLabel("range"))
        self.range_lo = QSpinBox(); self.range_lo.setRange(1, 13); self.range_lo.setValue(1)
        self.range_hi = QSpinBox(); self.range_hi.setRange(1, 13); self.range_hi.setValue(13)
        actions.addWidget(self.range_lo)
        actions.addWidget(QLabel("→"))
        actions.addWidget(self.range_hi)
        actions.addSpacing(8)
        ap_range = QPushButton("apply")
        ap_range.clicked.connect(
            lambda: self.send_command.emit(
                f"range {self.range_lo.value()} {self.range_hi.value()}"
            )
        )
        self.register_action_button(ap_range)
        actions.addWidget(ap_range)
        actions.addSpacing(16)
        actions.addWidget(QLabel("floor"))
        self.floor = QSpinBox()
        self.floor.setRange(-127, 0); self.floor.setSuffix(" dBm"); self.floor.setValue(-127)
        actions.addWidget(self.floor)
        actions.addSpacing(8)
        ap_floor = QPushButton("apply")
        ap_floor.clicked.connect(
            lambda: self.send_command.emit(f"floor {self.floor.value()}")
        )
        self.register_action_button(ap_floor)
        actions.addWidget(ap_floor)
        actions.addStretch(1)

        # --- waterfall (pandas DataFrame -> pyqtgraph ImageItem) -------
        self._waterfall_pw = pg.PlotWidget()
        self._waterfall_pw.setMouseEnabled(x=False, y=False)
        self._waterfall_pw.hideButtons()
        self._waterfall_pw.setLabel(
            "left", "Channel",
            **{"color": theme.TEXT_DIM, "font-size": "10pt"},
        )
        self._waterfall_pw.setLabel(
            "bottom", "Sweeps (newer →)",
            **{"color": theme.TEXT_DIM, "font-size": "10pt"},
        )
        for ax in (self._waterfall_pw.getAxis("left"),
                   self._waterfall_pw.getAxis("bottom")):
            ax.setPen(pg.mkPen(theme.BORDER, width=1))
            ax.setTextPen(pg.mkPen(theme.TEXT_MUTED))
        # Y-axis ticks are channel ids.
        self._waterfall_pw.getAxis("left").setTicks(
            [[(i - CH_MIN, str(i)) for i in range(CH_MIN, CH_MAX + 1)]]
        )
        self._heatmap = pg.ImageItem(axisOrder="row-major")
        # Inferno-ish gradient: dark for weak, warm glow for strong.
        cmap = pg.ColorMap(
            pos=[0.0, 0.25, 0.55, 0.8, 1.0],
            color=[
                QColor("#0a0a0a"),
                QColor("#3b0764"),
                QColor("#a21caf"),
                QColor("#ef4444"),
                QColor("#facc15"),
            ],
        )
        self._heatmap.setLookupTable(cmap.getLookupTable(0.0, 1.0, 256))
        self._heatmap.setLevels((float(RSSI_FLOOR), -20.0))
        self._waterfall_pw.addItem(self._heatmap)

        # Inline legend removed - colour-scale / data-flow notes now
        # live in the "Sobre" dialog.

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addWidget(self._waterfall_pw, 1)

    # ------------------------------------------------------------------

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        if obj.get("event") != "sweep":
            return
        if self._status is not None:
            # Visual confirmation that data is arriving even if the RF
            # is quiet and the colours barely move.
            self._status.set_state("running")
            self._status.record("sweep", detail=f"dwell={obj.get('dwell_ms','?')}ms")
        a = self.EMA_ALPHA
        for entry in obj.get("ch", []):
            try:
                d = json.loads(entry) if isinstance(entry, str) else entry
                idx = int(d.get("ch", 0)) - CH_MIN
                if 0 <= idx < N_CH:
                    new_avg = float(d.get("avg", RSSI_FLOOR))
                    if self._first_sweep:
                        self._avgs[idx] = new_avg
                    else:
                        # EMA: y_t = a*x + (1-a)*y_{t-1}
                        self._avgs[idx] = a * new_avg + (1 - a) * self._avgs[idx]
            except (TypeError, ValueError, json.JSONDecodeError):
                continue
        self._first_sweep = False
        # Rolling window: append the new sweep, trim the head.
        ts = pd.Timestamp(time.time(), unit="s")
        self._history.loc[ts] = self._avgs.astype(np.float32)
        if len(self._history) > WATERFALL_DEPTH:
            self._history = self._history.iloc[-WATERFALL_DEPTH:]
        # Image rows = channels, cols = time; transpose so newer is right.
        img = self._history.to_numpy().T
        self._heatmap.setImage(img, autoLevels=False)
