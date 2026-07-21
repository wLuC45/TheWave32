"""Background thread that runs the flash pipeline.

``pipeline.execute_flash`` blocks for ~30 s spawning subprocesses
(esptool, idf tools); doing that on the GUI thread freezes Qt's event
loop and on some platforms triggers an OS-level "application not
responding" kill - exactly the crash the user reported when flashing.
This worker hops the work off the main thread, surfaces progress as
signals, and lets the UI stay responsive.
"""

from __future__ import annotations

import traceback

from PySide6.QtCore import QThread, Signal

from thewave32 import log as _log, pipeline
from thewave32.errors import Tw32Error
from thewave32.manifest import Input
from thewave32.registry import Module

_logger = _log.get("gui.flash")


class FlashWorker(QThread):
    progress = Signal(str)
    finished_ok = Signal()
    finished_err = Signal(str)

    def __init__(
        self,
        mod: Module,
        port: str,
        resolved: list[tuple[Input, object]],
        baud: int = 921600,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._mod = mod
        self._port = port
        self._resolved = resolved
        self._baud = baud

    def run(self) -> None:
        try:
            _logger.info("flash worker start: %s @ %s baud=%d",
                         self._mod.name, self._port, self._baud)
            self.progress.emit(f"detecting chip on {self._port}…")
            pipeline.execute_flash(
                mod=self._mod,
                port=self._port,
                resolved=self._resolved,
                baud=self._baud,
            )
            _logger.info("flash worker done: %s", self._mod.name)
            self.finished_ok.emit()
        except Tw32Error as e:
            _logger.error("flash worker Tw32Error: %s", e)
            self.finished_err.emit(str(e))
        except Exception as e:  # noqa: BLE001 - let the GUI show whatever broke
            _logger.exception("flash worker unexpected failure")
            self.finished_err.emit(f"{type(e).__name__}: {e}\n\n{traceback.format_exc()}")
