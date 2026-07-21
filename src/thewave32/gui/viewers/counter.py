"""CounterViewer: shows latest stats in a key/value grid + a few sparklines."""

from __future__ import annotations

from collections import deque
from typing import Any, Sequence

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QPushButton, QSizePolicy, QVBoxLayout, QWidget,
)

from thewave32.gui import theme
from thewave32.gui.flow_layout import FlowLayout

from .base import BaseViewer


class CounterViewer(BaseViewer):
    ACTIONS: Sequence[str] = ("start", "stop", "stats")
    STATS_KEYS: Sequence[str] = ()
    SPARKLINE_KEYS: Sequence[str] = ()
    HELP: str = ""
    POLL_STATS_MS: int = 2000

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._values: dict[str, str] = {}
        self._series: dict[str, deque[float]] = {
            k: deque(maxlen=180) for k in self.SPARKLINE_KEYS
        }
        self._build()

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setContentsMargins(0, 0, 0, 0)
        actions.setSpacing(8)
        for cmd in self.ACTIONS:
            primary = cmd == "start"
            opt = None
            if cmd == "start":  opt = "running"
            elif cmd == "stop": opt = "stopped"
            actions.addWidget(self.make_action_button(cmd, primary=primary, optimistic_state=opt))
        actions.addStretch(1)
        self._poll_btn = QPushButton("auto-poll")
        self._poll_btn.setCheckable(True)
        self._poll_btn.setChecked(True)
        self._poll_btn.toggled.connect(self._toggle_polling)
        actions.addWidget(self._poll_btn)

        self._labels: dict[str, QLabel] = {}
        cards_host = QWidget()
        cards = FlowLayout(cards_host, margin=0, hspacing=14, vspacing=12)
        # Tile QSS uses #objectName so the gradient/border doesn't cascade
        # onto inner labels. Top-border-only reads as a Grafana panel
        # (the previous left-flag treatment confused alignment).
        TILE_QSS = (
            f"QWidget#statTile {{ background: {theme.BG}; "
            f"border: 1px solid {theme.TERM_BLUE}; "
            f"border-radius: 0px; }}"
        )
        for key in self.STATS_KEYS:
            cell = QWidget()
            cell.setObjectName("statTile")
            cell.setMinimumWidth(120)
            cell.setMaximumWidth(200)
            cell.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
            cell.setStyleSheet(TILE_QSS)
            v = QVBoxLayout(cell)
            v.setContentsMargins(8, 4, 8, 4)
            v.setSpacing(1)
            cap = QLabel(key.upper().replace("_", " "))
            cap.setProperty("role", "caption")
            val = QLabel("-")
            val.setProperty("role", "metric")
            v.addWidget(cap)
            v.addWidget(val)
            cards.addWidget(cell)
            self._labels[key] = val

        sp = QVBoxLayout()
        sp.setContentsMargins(0, 0, 0, 0)
        sp.setSpacing(10)
        self._curves: dict[str, Any] = {}
        for i, key in enumerate(self.SPARKLINE_KEYS):
            pw = pg.PlotWidget()
            pw.setMinimumHeight(110)
            pw.setMaximumHeight(160)
            pw.showGrid(x=False, y=True, alpha=0.12)
            pw.setMouseEnabled(x=False, y=False)
            pw.hideButtons()
            pw.setLabel("left", key,
                        **{"color": theme.TEXT_DIM, "font-size": "9pt"})
            pw.getAxis("left").setTextPen(pg.mkPen(theme.TEXT_MUTED))
            pw.getAxis("bottom").setTextPen(pg.mkPen(theme.TEXT_MUTED))
            color = theme.CHART_PALETTE[i % len(theme.CHART_PALETTE)]
            self._curves[key] = pw.plot(pen=pg.mkPen(color, width=2))
            sp.addWidget(pw)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        # Inline HELP intentionally NOT rendered here - the per-module
        # "Sobre" button on the module card opens a dialog with the same
        # text (see BaseViewer.show_about_dialog).
        layout.addWidget(cards_host)
        layout.addLayout(sp, 1)

        self._poll = QTimer(self)
        self._poll.timeout.connect(self._tick)
        # Don't start polling until set_actions_enabled(True) - otherwise
        # we'd keep firing `stats` into the void while the module is
        # gated, and the MainWindow has to swallow each one.
        if self._actions_enabled and self._poll_btn.isChecked():
            self._poll.start(self.POLL_STATS_MS)

    def set_actions_enabled(self, on: bool) -> None:
        super().set_actions_enabled(on)
        if on and self._poll_btn.isChecked():
            self._poll.start(self.POLL_STATS_MS)
        else:
            self._poll.stop()

    def _toggle_polling(self, on: bool) -> None:
        if on and self._actions_enabled:
            self._poll.start(self.POLL_STATS_MS)
        else:
            self._poll.stop()

    def _tick(self) -> None:
        if not self._actions_enabled:
            return
        self.send_command.emit("stats")

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        # Reflect the firmware's own `running` flag on the StatusStrip so a
        # board left running across sessions shows up correctly.
        if self._status is not None and obj.get("cmd") == "stats":
            running = obj.get("running")
            if isinstance(running, bool):
                self._status.set_state("running" if running else "stopped")
        if obj.get("cmd") != "stats":
            return
        for key, label in self._labels.items():
            v = obj.get(key)
            if v is None:
                continue
            label.setText(_format_value(v))
            self._values[key] = str(v)
        for key in self.SPARKLINE_KEYS:
            v = obj.get(key)
            if isinstance(v, (int, float)):
                self._series[key].append(float(v))
                curve = self._curves.get(key)
                if curve is not None:
                    curve.setData(list(self._series[key]))


def _format_value(v: Any) -> str:
    if isinstance(v, bool):
        return "yes" if v else "no"
    if isinstance(v, int) and v >= 1000:
        return f"{v:,}".replace(",", " ")
    return str(v)
