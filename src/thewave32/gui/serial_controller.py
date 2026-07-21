"""SerialController - owns the SerialWorker + SessionLog lifecycle.

Pulls the serial-side plumbing out of MainWindow so:
  * the worker thread + per-session capture file move/start/stop in
    lockstep - open() always pairs them, close() always pairs them
  * inbound JSON and raw bytes are recorded to the session log before
    the controller re-emits them, so consumers (MainWindow, viewers,
    playbooks) can subscribe to one source without each one having to
    remember to log
  * outbound commands routed through .send() are recorded too

The controller is a QObject; emit-and-forget. MainWindow keeps the UI
wiring and tab logic.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from PySide6.QtCore import QObject, Signal

from thewave32 import log as _log
from thewave32.gui.serial_worker import SerialWorker
from thewave32.privacy import PrivacyPolicy
from thewave32.gui.session_log import SessionLog


_logger = _log.get("gui.serial_controller")


class SerialController(QObject):
    """Lifecycle manager around SerialWorker + SessionLog."""

    json_received       = Signal(dict)
    raw_received        = Signal(bytes)
    connection_changed  = Signal(bool, str)
    error               = Signal(str)
    session_log_changed = Signal(object)   # Path | None

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._worker: Optional[SerialWorker] = None
        self._session_log = SessionLog()
        self._port: Optional[str] = None
        self._baud: int = 115200
        # Off by default; opt in via the THEWAVE32_PRIVACY env var (or
        # set_privacy() at runtime). Applied to every inbound event before
        # it is logged or forwarded to the GUI.
        self._privacy = PrivacyPolicy.from_env()
        # The worker exists from open() to close(); _connected tracks
        # whether the *serial port itself* actually opened - that's an
        # async result delivered later via connection_changed.
        self._connected: bool = False

    # --- properties ---------------------------------------------------

    @property
    def is_open(self) -> bool:
        """True only once the worker has actually opened the port -
        not merely between open() and close(). Callers use this to gate
        outbound traffic, so it must not report a port that never
        came up."""
        return self._worker is not None and self._connected

    @property
    def port(self) -> Optional[str]:
        return self._port

    @property
    def session_log_path(self) -> Optional[Path]:
        return self._session_log.path

    @property
    def session_log(self) -> SessionLog:
        return self._session_log

    # --- lifecycle ----------------------------------------------------

    def open(self, port: str, baud: int = 115200) -> bool:
        """Open the worker + start a fresh session log. Returns False on
        SerialWorker construction failure (rare; pyserial would have
        already raised). Callers should resolve the port first via
        ``flasher.resolve_port`` and pass it explicitly."""
        self.close()
        self._port = port
        self._baud = baud
        _logger.info("opening %s @ %d", port, baud)
        try:
            self._worker = SerialWorker(port, baud=baud, parent=self)
        except Exception:
            _logger.exception("SerialWorker construction failed")
            return False
        self._connected = False
        # Consume the coalesced batch rather than per-object signals: the
        # worker->controller hop is cross-thread (queued), so one batch per
        # serial read collapses N queued slot calls into one. Re-emission to
        # the GUI below is same-thread and cheap, so downstream viewers and
        # the playbook dialog keep their per-object json_received contract.
        self._worker.json_batch.connect(self._on_json_batch)
        self._worker.raw_received.connect(self._on_raw_in)
        self._worker.connection_changed.connect(self._on_connection_changed)
        self._worker.error.connect(self.error.emit)
        self._worker.start()
        path = self._session_log.open(port=port)
        self.session_log_changed.emit(path)
        return True

    def close(self) -> None:
        if self._worker is not None:
            _logger.info("closing %s", self._port)
            self._worker.stop()
            # The worker should exit within one read() timeout (~200ms).
            # If it doesn't - e.g. the device was unplugged and pyserial
            # is wedged - terminate rather than abandon a live thread
            # still holding the port handle.
            if not self._worker.wait(2000):
                _logger.warning("serial worker did not exit; terminating")
                self._worker.terminate()
                self._worker.wait(1000)
            self._worker = None
        self._connected = False
        self._session_log.close()
        self.session_log_changed.emit(None)

    def set_binary_mode(self, on: bool) -> None:
        """Toggle the worker's binary-frame mode (used by PCAP-style
        viewers). No-op when the port is closed."""
        if self._worker is not None:
            self._worker.set_binary_mode(on)

    def send(self, cmd: str) -> bool:
        """Send a command; record it on the session log on success.
        Returns True on success, False if the port is closed or the
        underlying write failed."""
        if self._worker is None:
            _logger.debug("send dropped (no worker): %r", cmd)
            return False
        if not self._worker.send(cmd):
            return False
        self._session_log.record_out(cmd)
        return True

    # --- private slots ------------------------------------------------

    def _on_connection_changed(self, connected: bool, message: str) -> None:
        self._connected = connected
        self.connection_changed.emit(connected, message)

    def set_privacy(self, policy: PrivacyPolicy) -> None:
        """Install a privacy policy applied to all future inbound events."""
        self._privacy = policy

    def _on_json_batch(self, batch: list) -> None:
        for obj in batch:
            if self._privacy.active:
                obj = self._privacy.apply(obj)
            self._session_log.record_in(obj)
            self.json_received.emit(obj)

    def _on_raw_in(self, data: bytes) -> None:
        self._session_log.record_raw(data)
        self.raw_received.emit(data)


__all__ = ["SerialController"]
