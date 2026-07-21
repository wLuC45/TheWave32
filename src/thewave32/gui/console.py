"""Live JSON-Lines console with colour-coded entries + a send box."""

from __future__ import annotations

import json
import logging
import time
from typing import Any

from PySide6.QtCore import Qt, Signal, Slot
from PySide6.QtGui import QColor, QFont, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import (
    QHBoxLayout, QLineEdit, QPlainTextEdit, QPushButton, QVBoxLayout,
    QWidget,
)

from thewave32.gui import theme


# ANSI-style severity colours, pulled from the theme tokens so the Console
# reads like a tmux log pane (dim INFO / DEBUG, yellow WARN, red ERROR).
_LEVEL_COLORS: dict[int, QColor] = {
    logging.DEBUG:    QColor(theme.TEXT_MUTED),
    logging.INFO:     QColor(theme.TEXT_DIM),
    logging.WARNING:  QColor(theme.TERM_YELLOW),
    logging.ERROR:    QColor(theme.TERM_RED),
    logging.CRITICAL: QColor(theme.TERM_RED),
}


class Console(QWidget):
    command_sent = Signal(str)
    # Cross-thread channel: workers (FlashWorker, SerialWorker, the
    # logging bridge) emit here; the GUI thread receives it via Qt's
    # auto-queued connection so QPlainTextEdit is only ever touched on
    # its owning thread.
    _status_message = Signal(str)
    _log_message    = Signal(int, str)   # (levelno, formatted message)

    _MAX_LINES = 4000

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.text = QPlainTextEdit()
        self.text.setReadOnly(True)
        self.text.setFont(QFont("Monospace", 9))
        self.text.setMaximumBlockCount(self._MAX_LINES)
        # Cache the most recent "HH:MM:SS" formatting. At 115 200 baud
        # the JSON-Lines parser hands us hundreds of lines/sec, so the
        # repeated `time.strftime` syscall was measurable.
        self._ts_epoch_sec: int = -1
        self._ts_cached: str = ""
        # The visible ">" prompt symbol and the "lines: N" counter were
        # removed: the prompt added no information (the QLineEdit's
        # placeholder already invites input) and the counter was visual
        # noise. Internal _line_count is kept for the (unused) flush
        # path and to make _on_clear a no-op rather than a crash.
        self._line_count: int = 0
        self._counter_pending: bool = False
        self.input = QLineEdit()
        self.input.setPlaceholderText("type a command (e.g. start, stats, help)…")
        self.input.returnPressed.connect(self._on_send)
        self.btn_send = QPushButton("Send")
        self.btn_send.clicked.connect(self._on_send)
        self.btn_clear = QPushButton("Clear")
        self.btn_clear.clicked.connect(self.text.clear)
        self.btn_clear.clicked.connect(self._on_clear)

        row = QHBoxLayout()
        row.setSpacing(4)
        row.addWidget(self.input, 1)
        row.addWidget(self.btn_send)
        row.addWidget(self.btn_clear)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.text, 1)
        layout.addLayout(row)

        # Auto-queued because the receiver lives on the GUI thread.
        self._status_message.connect(self._on_status_message)
        self._log_message.connect(self._on_log_message)

    def _on_send(self) -> None:
        cmd = self.input.text().strip()
        if not cmd:
            return
        self.input.clear()
        self.append_outbound(cmd)
        self.command_sent.emit(cmd)

    # --- output helpers --------------------------------------------------

    def _now_hms(self) -> str:
        """Cached `HH:MM:SS` - only re-formats once per wall-clock second."""
        sec = int(time.time())
        if sec != self._ts_epoch_sec:
            self._ts_epoch_sec = sec
            self._ts_cached = time.strftime("%H:%M:%S", time.localtime(sec))
        return self._ts_cached

    def append_outbound(self, cmd: str) -> None:
        # User input echoed back; yellow reads as "you typed this".
        self._append(f"> {cmd}", QColor(theme.TERM_YELLOW))

    def append_json(self, obj: dict[str, Any]) -> None:
        line = f"[{self._now_hms()}]  {json.dumps(obj, ensure_ascii=False)}"
        if obj.get("event") == "ready":
            color = QColor(theme.TERM_GREEN)
        elif "event" in obj:
            color = QColor(theme.TERM_BLUE)
        elif obj.get("ok") is True:
            color = QColor(theme.TERM_GREEN)
        elif obj.get("ok") is False:
            color = QColor(theme.TERM_RED)
        else:
            color = QColor(theme.TEXT_DIM)
        self._append(line, color)

    def append_raw(self, data: bytes) -> None:
        try:
            text = data.decode("utf-8", errors="replace").rstrip()
        except Exception:
            text = repr(data)
        if not text:
            return
        self._append(f"[{self._now_hms()}]  ! {text}", QColor(theme.TEXT_MUTED))

    def append_status(self, msg: str) -> None:
        # Safe to call from any thread - signal hops to the GUI thread.
        self._status_message.emit(msg)

    def append_log(self, levelno: int, msg: str) -> None:
        """Severity-aware log line: colour comes from ``_LEVEL_COLORS``.

        Safe to call from any thread - the signal hops to the GUI thread,
        same pattern as ``append_status``.
        """
        self._log_message.emit(int(levelno), msg)

    @Slot(str)
    def _on_status_message(self, msg: str) -> None:
        self._append(f"[{self._now_hms()}]  {msg}", QColor(theme.TERM_YELLOW))

    @Slot(int, str)
    def _on_log_message(self, levelno: int, msg: str) -> None:
        color = _LEVEL_COLORS.get(levelno, QColor(theme.TEXT_DIM))
        self._append(f"[{self._now_hms()}]  {msg}", color)

    def _append(self, line: str, color: QColor) -> None:
        cursor = self.text.textCursor()
        cursor.movePosition(QTextCursor.MoveOperation.End)
        fmt = QTextCharFormat()
        fmt.setForeground(color)
        cursor.insertText(line + "\n", fmt)
        self.text.setTextCursor(cursor)
        self.text.ensureCursorVisible()
        self._line_count += 1

    def _on_clear(self) -> None:
        """Reset the running line count when the user clears the view.
        The visible counter label is gone, but the count is still used
        by any external diagnostics that read ``self._line_count``."""
        self._line_count = 0
