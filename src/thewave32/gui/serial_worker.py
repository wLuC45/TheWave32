"""Background thread that reads JSON-Lines from the device.

Lives in a ``QThread`` so the UI stays responsive while serial I/O
blocks. Emits ``json_received`` for parsed objects, ``raw_received``
for non-JSON bytes (binary streams or ROM bootloader chatter), and
``connection_changed`` for state transitions. Writing is forwarded
straight to the underlying ``serial.Serial`` handle - pyserial's
internal lock makes concurrent read/write safe.
"""

from __future__ import annotations

import json
import threading

import serial
from PySide6.QtCore import QThread, Signal

from thewave32 import log as _log

_logger = _log.get("gui.serial")


class SerialWorker(QThread):
    json_received = Signal(dict)
    # Coalesced variant: every object parsed from one serial read is shipped
    # in a single queued signal. At ~90 events/s a per-object signal floods
    # the GUI event loop (one queued slot call each); one batch per read
    # collapses that to a single cross-thread dispatch, so the consumer can
    # repaint once per batch instead of once per line.
    json_batch = Signal(list)
    raw_received = Signal(bytes)
    connection_changed = Signal(bool, str)
    error = Signal(str)

    def __init__(self, port: str, baud: int = 115200, parent=None) -> None:
        super().__init__(parent)
        self._port = port
        self._baud = baud
        self._stop = False
        self._buf = bytearray()
        self._binary_mode = False
        # Set by set_binary_mode() on the GUI thread; consumed by run()
        # on the worker thread. Clearing self._buf directly from the GUI
        # thread would race the worker's extend()/find()/del - so we
        # only raise a flag here and let run() drop the buffer itself.
        self._buf_reset = False
        self._ser: serial.Serial | None = None
        self._tx_lock = threading.Lock()

    # --- control ---------------------------------------------------------

    def stop(self) -> None:
        self._stop = True

    def set_binary_mode(self, on: bool) -> None:
        # Called from the GUI thread. `bool` assignment is atomic in
        # CPython; the buffer is dropped by run() at a safe point - see
        # the _buf_reset flag.
        self._binary_mode = on
        self._buf_reset = True

    def send(self, cmd: str) -> bool:
        ser = self._ser
        if ser is None or not ser.is_open:
            _logger.warning("send dropped (port closed): %r", cmd)
            return False
        try:
            with self._tx_lock:
                ser.write((cmd + "\n").encode("utf-8"))
                ser.flush()
            _logger.debug("→ %s", cmd)
            return True
        except serial.SerialException as e:
            _logger.error("write failed (%r): %s", cmd, e)
            self.error.emit(f"write failed: {e}")
            return False

    # --- thread body -----------------------------------------------------

    def run(self) -> None:
        _logger.info("opening %s @ %d baud", self._port, self._baud)
        try:
            self._ser = serial.Serial(self._port, self._baud, timeout=0.2)
        except serial.SerialException as e:
            _logger.error("open failed (%s): %s", self._port, e)
            self.connection_changed.emit(False, f"open failed: {e}")
            return
        _logger.info("connected to %s", self._port)
        self.connection_changed.emit(True, f"connected to {self._port}")
        try:
            while not self._stop:
                # A mode toggle on the GUI thread asked us to discard
                # any half-assembled line - do it here, on our thread.
                if self._buf_reset:
                    self._buf_reset = False
                    self._buf.clear()
                try:
                    chunk = self._ser.read(512)
                except serial.SerialException as e:
                    self.error.emit(f"read failed: {e}")
                    break
                if not chunk:
                    continue
                if self._binary_mode:
                    self.raw_received.emit(bytes(chunk))
                    continue
                # Track where the last `\n` search left off so a long
                # line without a newline doesn't make us re-scan from
                # byte 0 on every chunk arriving at 115 200 baud.
                scan_from = len(self._buf)
                self._buf.extend(chunk)
                if len(self._buf) > 65536:
                    drop = 32768
                    del self._buf[:drop]
                    scan_from = max(0, scan_from - drop)
                batch: list[dict] = []
                while True:
                    nl = self._buf.find(b"\n", scan_from)
                    if nl < 0:
                        break
                    line = bytes(self._buf[:nl]).strip()
                    del self._buf[: nl + 1]
                    scan_from = 0   # buffer shifted; resume from start
                    if not line:
                        continue
                    try:
                        obj = json.loads(line.decode("utf-8", errors="replace"))
                    except json.JSONDecodeError:
                        self.raw_received.emit(line + b"\n")
                        continue
                    if isinstance(obj, dict):
                        # Per-object signal kept for any single-event
                        # consumers/tests; the batch carries the same
                        # objects for the GUI's coalesced dispatch.
                        self.json_received.emit(obj)
                        batch.append(obj)
                if batch:
                    self.json_batch.emit(batch)
        finally:
            try:
                if self._ser is not None:
                    self._ser.close()
            finally:
                self._ser = None
                _logger.info("disconnected from %s", self._port)
                self.connection_changed.emit(False, "disconnected")
