"""Python logging + Qt diagnostic stream → on-screen Console.

Two pieces wired together:

  * a ``logging.Handler`` attached to ``thewave32.*`` that emits each
    record into the supplied :class:`Console` via its thread-safe
    signal channel. Survives cross-thread emits (FlashWorker /
    SerialWorker live on QThreads).
  * ``qInstallMessageHandler`` so Qt's own diagnostic stream
    (qDebug / qWarning / qCritical) shows up in the same panel,
    namespaced under ``thewave32.qt``. Guarded against re-entrance
    because a Console widget update can in turn produce a qWarning
    during teardown.

Calling :func:`install` twice is safe - the previous Console handler
is removed first and a fresh one attached.
"""

from __future__ import annotations

import logging
import sys
import threading
import traceback

from PySide6.QtCore import QtMsgType, qInstallMessageHandler

from thewave32 import log as _log


_GUI_LOGGER = _log.get("gui")
_QT_LOGGER  = _log.get("qt")
_QT_LEVELS: dict[QtMsgType, int] = {
    QtMsgType.QtDebugMsg:    logging.DEBUG,
    QtMsgType.QtInfoMsg:     logging.INFO,
    QtMsgType.QtWarningMsg:  logging.WARNING,
    QtMsgType.QtCriticalMsg: logging.ERROR,
    QtMsgType.QtFatalMsg:    logging.CRITICAL,
}
_qt_recursing = threading.local()


class _ConsoleHandler(logging.Handler):
    """Routes formatted log records to a :class:`Console` widget. The
    Console handles cross-thread safety internally (Qt auto-queued
    signal); from this side the call is a fire-and-forget."""

    def __init__(self, console) -> None:
        super().__init__()
        self._console = console

    def emit(self, record: logging.LogRecord) -> None:
        try:
            msg = self.format(record)
        except Exception:
            msg = record.getMessage()
        # Route through the severity-aware path so DEBUG / INFO render dim,
        # WARNING yellow, ERROR red - like a real terminal log pane.
        self._console.append_log(record.levelno, f"[{record.levelname}] {msg}")


def install(console) -> None:
    """Attach the Console handler to the project logger and install the
    Qt message handler. Idempotent."""
    handler = _ConsoleHandler(console)
    handler.setLevel(logging.DEBUG)
    handler.setFormatter(logging.Formatter("%(name)s - %(message)s"))

    root = logging.getLogger("thewave32")
    root.setLevel(logging.DEBUG)
    for h in list(root.handlers):
        if isinstance(h, _ConsoleHandler):
            root.removeHandler(h)
    root.addHandler(handler)

    def _qt_msg_handler(mtype, ctx, msg):
        if getattr(_qt_recursing, "flag", False):
            return
        _qt_recursing.flag = True
        try:
            _QT_LOGGER.log(_QT_LEVELS.get(mtype, logging.INFO), "%s", msg)
        finally:
            _qt_recursing.flag = False

    qInstallMessageHandler(_qt_msg_handler)

    prev_hook = sys.excepthook

    def _excepthook(exc_type, exc_val, tb):
        tb_text = "".join(traceback.format_exception(exc_type, exc_val, tb))
        _GUI_LOGGER.error("uncaught exception: %s", tb_text)
        prev_hook(exc_type, exc_val, tb)

    sys.excepthook = _excepthook
    _GUI_LOGGER.info("log bridge installed - module logs now stream into the Console")


__all__ = ["install"]
