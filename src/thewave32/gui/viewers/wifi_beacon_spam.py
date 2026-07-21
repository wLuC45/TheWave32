"""Custom viewer for wifi-beacon-spam.

Firmware CLI: add <ssid>, clear, count, interval N, chan N, random N,
clones <base> <N>, start, stop, stats.

The CounterViewer-based UI exposed only start/stop/stats - no way to
add SSIDs, change channel, or pick the spam interval. This rebuilds
the panel as a proper control surface.
"""

from __future__ import annotations

from collections import deque
from typing import Any

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QSizePolicy, QSpinBox,
    QVBoxLayout, QWidget,
)

from thewave32.gui import theme

from .base import BaseViewer


class WifiBeaconSpamViewer(BaseViewer):
    """SSID list manager + chan/interval controls + frames-sent rate plot."""

    HISTORY = 180

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._frames_history: deque[float] = deque(maxlen=self.HISTORY)
        self._last_frames = 0
        self._build()

    def _build(self) -> None:
        # --- run controls -----------------------------------------------
        run = QHBoxLayout()
        run.setContentsMargins(0, 0, 0, 0)
        run.setSpacing(8)
        run.addWidget(self.make_action_button("start", primary=True,
                                              optimistic_state="running"))
        run.addWidget(self.make_action_button("stop",
                                              optimistic_state="stopped"))
        run.addWidget(self.make_action_button("count", "count"))
        run.addWidget(self.make_action_button("clear list", "clear"))
        run.addStretch(1)

        # --- SSID input row ---------------------------------------------
        ssid_row = QHBoxLayout()
        ssid_row.setSpacing(8)
        ssid_row.addWidget(QLabel("add SSID"))
        self.ssid_in = QLineEdit()
        self.ssid_in.setPlaceholderText("FreeWiFi  (Enter to add)")
        self.ssid_in.returnPressed.connect(self._add_ssid)
        ssid_row.addWidget(self.ssid_in, 1)
        add_btn = QPushButton("add")
        add_btn.setProperty("primary", True)
        add_btn.clicked.connect(self._add_ssid)
        self.register_action_button(add_btn)
        ssid_row.addWidget(add_btn)
        ssid_row.addSpacing(16)
        ssid_row.addWidget(QLabel("randomise"))
        self.rnd_n = QSpinBox()
        self.rnd_n.setRange(1, 64)
        self.rnd_n.setValue(8)
        ssid_row.addWidget(self.rnd_n)
        rnd_btn = QPushButton("replace")
        rnd_btn.clicked.connect(
            lambda: self.fire(f"random {self.rnd_n.value()}",
                              label=f"random {self.rnd_n.value()}")
        )
        self.register_action_button(rnd_btn)
        ssid_row.addWidget(rnd_btn)

        # --- clones row -------------------------------------------------
        clones_row = QHBoxLayout()
        clones_row.setSpacing(8)
        clones_row.addWidget(QLabel("clone base"))
        self.clone_base = QLineEdit()
        self.clone_base.setPlaceholderText("Vivo-Fibra (will append _01, _02 …)")
        clones_row.addWidget(self.clone_base, 2)
        self.clone_n = QSpinBox()
        self.clone_n.setRange(2, 64)
        self.clone_n.setValue(8)
        self.clone_n.setPrefix("× ")
        clones_row.addWidget(self.clone_n)
        clone_btn = QPushButton("generate clones")
        clone_btn.clicked.connect(self._make_clones)
        self.register_action_button(clone_btn)
        clones_row.addWidget(clone_btn)

        # --- channel + interval ----------------------------------------
        params = QHBoxLayout()
        params.setSpacing(8)
        params.addWidget(QLabel("channel"))
        self.chan = QSpinBox()
        self.chan.setRange(1, 14)
        self.chan.setValue(6)
        params.addWidget(self.chan)
        chan_btn = QPushButton("apply ch")
        chan_btn.clicked.connect(
            lambda: self.fire(f"chan {self.chan.value()}",
                              label=f"chan {self.chan.value()}")
        )
        self.register_action_button(chan_btn)
        params.addWidget(chan_btn)
        params.addSpacing(16)
        params.addWidget(QLabel("interval"))
        self.interval = QSpinBox()
        self.interval.setRange(50, 2000)
        self.interval.setSingleStep(50)
        self.interval.setSuffix(" ms")
        self.interval.setValue(100)
        params.addWidget(self.interval)
        ival_btn = QPushButton("apply")
        ival_btn.clicked.connect(
            lambda: self.fire(f"interval {self.interval.value()}",
                              label=f"interval {self.interval.value()}ms")
        )
        self.register_action_button(ival_btn)
        params.addWidget(ival_btn)
        params.addSpacing(16)
        params.addWidget(QLabel("hop"))
        hop_on = QPushButton("on")
        hop_on.setToolTip(
            "Rotate beacons across channels 1..13 each cycle so phones "
            "scanning a different channel still pick up the SSIDs. "
            "Without hopping the spam is single-channel and most phones "
            "will see only ~3-5 networks."
        )
        hop_on.clicked.connect(lambda: self.fire("hop on", label="hop on"))
        self.register_action_button(hop_on)
        params.addWidget(hop_on)
        hop_off = QPushButton("off")
        hop_off.clicked.connect(lambda: self.fire("hop off", label="hop off"))
        self.register_action_button(hop_off)
        params.addWidget(hop_off)
        params.addSpacing(16)
        params.addWidget(QLabel("auth"))
        auth_open = QPushButton("open")
        auth_open.clicked.connect(lambda: self.fire("auth open", label="auth open"))
        self.register_action_button(auth_open)
        params.addWidget(auth_open)
        auth_wpa2 = QPushButton("wpa2 (cosmetic)")
        auth_wpa2.setToolTip(
            "Adds the WPA2 RSN IE + Privacy bit to every beacon. "
            "Clients see the SSIDs as 'secured' in their scan UI; no "
            "actual association handshake is implemented (this is a "
            "beacon flooder, not a real AP)."
        )
        auth_wpa2.clicked.connect(lambda: self.fire("auth wpa2", label="auth wpa2"))
        self.register_action_button(auth_wpa2)
        params.addWidget(auth_wpa2)
        params.addStretch(1)

        # --- stats cards -----------------------------------------------
        cards_row = QHBoxLayout()
        cards_row.setSpacing(12)
        # `frames_sent` is the frames/s plot below - not repeated as a card.
        self._cards = {
            "tx_errors":    self._make_card("TX ERRORS", "0"),
            "radio_resets": self._make_card("RADIO RESETS", "0"),
            "ssid_count":   self._make_card("SSIDs", "0"),
            "channel":      self._make_card("CHANNEL", "-"),
            "interval_ms":  self._make_card("INTERVAL", "- ms"),
        }
        for c in self._cards.values():
            cards_row.addWidget(c["wrap"])
        cards_row.addStretch(1)

        # --- rate plot --------------------------------------------------
        self.plot = pg.PlotWidget()
        self.plot.setMinimumHeight(160)
        self.plot.setLabel("left", "frames/s",
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
        layout.addLayout(run)
        layout.addLayout(ssid_row)
        layout.addLayout(clones_row)
        layout.addLayout(params)
        layout.addLayout(cards_row)
        layout.addWidget(self.plot, 1)

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

    def _add_ssid(self) -> None:
        s = self.ssid_in.text().strip()
        if not s:
            return
        self.ssid_in.clear()
        self.fire(f"add {s}", label=f"add {s}")
        self.send_command.emit("count")

    def _make_clones(self) -> None:
        base = self.clone_base.text().strip()
        if not base:
            return
        n = self.clone_n.value()
        self.fire(f"clones {base} {n}", label=f"clones {base}×{n}")
        self.send_command.emit("count")

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        cmd = obj.get("cmd")
        if cmd == "stats":
            frames = int(obj.get("frames_sent", 0))
            self._cards["tx_errors"]["val"].setText(
                str(obj.get("tx_errors", 0))
            )
            self._cards["radio_resets"]["val"].setText(
                str(obj.get("radio_resets", 0))
            )
            self._cards["ssid_count"]["val"].setText(str(obj.get("ssid_count", "-")))
            ch_str = str(obj.get("channel", "-"))
            if obj.get("hopping"):
                ch_str = "hop"
            self._cards["channel"]["val"].setText(ch_str)
            self._cards["interval_ms"]["val"].setText(
                f"{obj.get('interval_ms', '-')} ms"
            )
            running = bool(obj.get("running"))
            if self._status is not None:
                self._status.set_state("running" if running else "stopped")
            rate = max(0.0, frames - self._last_frames)
            self._last_frames = frames
            self._frames_history.append(rate)
            self._curve.setData(list(self._frames_history))
        elif cmd == "count":
            n = obj.get("ssid_count")
            if n is not None:
                self._cards["ssid_count"]["val"].setText(str(n))
