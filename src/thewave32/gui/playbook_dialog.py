"""Modal dialog: pick a playbook, edit it inline, run it.

Layout:
  ┌─────────────────────────┬──────────────────────────────┐
  │ Playbooks list          │ Editor (read-write)          │
  │  - recon (wifi-deauth)  │ # module: wifi-deauth        │
  │  - twin   (evil-twin)   │ # desc: scan + first attack  │
  │  ...                    │ scan                         │
  │                         │ wait scan_done 30            │
  │                         │ attack 1                     │
  │                         │ sleep 30                     │
  │                         │ stop                         │
  ├─────────────────────────┴──────────────────────────────┤
  │ Step log (live)                                        │
  │  ▶ recon: 5 step(s)                                    │
  │  1/5 scan                                              │
  │  2/5 wait scan_done 30                                 │
  ├────────────────────────────────────────────────────────┤
  │             [Run]  [Stop]                  [Close]     │
  └────────────────────────────────────────────────────────┘
"""

from __future__ import annotations

from typing import Callable, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QDialog, QDialogButtonBox, QHBoxLayout, QLabel, QListWidget,
    QListWidgetItem, QPlainTextEdit, QPushButton, QSizePolicy,
    QSplitter, QVBoxLayout, QWidget,
)

from thewave32.gui import theme
from thewave32.gui.playbook import (
    Playbook, PlaybookParseError, PlaybookRunner, discover_playbooks,
)


class PlaybookDialog(QDialog):
    """Non-modal-ish runner. Caller passes a `send_cmd(str)` and a
    JSON source signal so the runner can resolve `wait` steps."""

    def __init__(self, send_cmd: Callable[[str], None],
                 active_module: Optional[str] = None,
                 parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Playbooks")
        self.resize(900, 620)
        self._send = send_cmd
        self._active_module = active_module
        self._runner: Optional[PlaybookRunner] = None
        self._playbooks: list[Playbook] = []
        self._build()
        self._reload_list()

    def _build(self) -> None:
        self.list = QListWidget()
        self.list.setMinimumWidth(220)
        self.list.currentRowChanged.connect(self._on_select)

        self.editor = QPlainTextEdit()
        self.editor.setPlaceholderText(
            "# module: wifi-deauth\n"
            "# desc: short scan + first attack\n"
            "scan\n"
            "wait scan_done 20\n"
            "attack 1\n"
            "sleep 30\n"
            "stop\n"
        )
        self.editor.setStyleSheet(
            f"QPlainTextEdit {{ "
            f"font-size: 10pt; color: {theme.TEXT}; background: {theme.BG}; }}"
        )

        top = QSplitter(Qt.Orientation.Horizontal)
        top.addWidget(self.list)
        top.addWidget(self.editor)
        top.setStretchFactor(0, 0)
        top.setStretchFactor(1, 1)
        top.setSizes([260, 600])

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(800)
        self.log.setStyleSheet(
            f"QPlainTextEdit {{ "
            f"font-size: 9pt; color: {theme.TEXT_DIM}; background: {theme.BG}; }}"
        )
        self.log.setPlaceholderText("Run a playbook to see step-by-step progress here.")

        main = QSplitter(Qt.Orientation.Vertical)
        main.addWidget(top)
        main.addWidget(self.log)
        main.setStretchFactor(0, 3)
        main.setStretchFactor(1, 2)
        main.setSizes([380, 220])

        self.run_btn = QPushButton("Run")
        self.run_btn.setProperty("primary", True)
        self.run_btn.clicked.connect(self._run)
        self.stop_btn = QPushButton("Stop")
        self.stop_btn.setEnabled(False)
        self.stop_btn.clicked.connect(self._stop)
        reload_btn = QPushButton("Reload")
        reload_btn.clicked.connect(self._reload_list)
        close = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        close.rejected.connect(self.reject)

        bottom = QHBoxLayout()
        bottom.addWidget(self.run_btn)
        bottom.addWidget(self.stop_btn)
        bottom.addStretch(1)
        bottom.addWidget(reload_btn)
        bottom.addWidget(close)

        layout = QVBoxLayout(self)
        layout.addWidget(main, 1)
        layout.addLayout(bottom)

        # Mark the primary button so the QSS picks it up.
        self.run_btn.style().unpolish(self.run_btn)
        self.run_btn.style().polish(self.run_btn)

    # --- list management ---------------------------------------------

    def _reload_list(self) -> None:
        self._playbooks = discover_playbooks()
        self.list.clear()
        for pb in self._playbooks:
            label = pb.name
            if pb.module:
                label += f"   ({pb.module})"
            it = QListWidgetItem(label)
            it.setToolTip(pb.description or pb.name)
            # Grey out playbooks scoped to a different module.
            if (self._active_module is not None
                    and pb.module is not None
                    and pb.module != self._active_module):
                it.setFlags(it.flags() & ~Qt.ItemFlag.ItemIsEnabled)
            self.list.addItem(it)
        if self.list.count():
            self.list.setCurrentRow(0)

    def _on_select(self, row: int) -> None:
        if row < 0 or row >= len(self._playbooks):
            self.editor.clear()
            return
        pb = self._playbooks[row]
        # Reconstruct text from the parsed steps (lossy on comments - but
        # we re-read source from disk for fidelity).
        parts = []
        if pb.module:
            parts.append(f"# module: {pb.module}")
        if pb.description:
            parts.append(f"# desc: {pb.description}")
        parts.extend(s.raw for s in pb.steps)
        self.editor.setPlainText("\n".join(parts) + "\n")

    # --- run / stop ---------------------------------------------------

    def _run(self) -> None:
        text = self.editor.toPlainText()
        try:
            pb = Playbook.parse(text, name="(editor)")
        except PlaybookParseError as e:
            self.log.appendPlainText(f"✗ parse error: {e}")
            return
        if not pb.steps:
            self.log.appendPlainText("✗ playbook has no steps")
            return
        if (self._active_module is not None
                and pb.module is not None
                and pb.module != self._active_module):
            self.log.appendPlainText(
                f"✗ this playbook is scoped to {pb.module!r} "
                f"but the active module is {self._active_module!r}"
            )
            return
        self.log.clear()
        self._runner = PlaybookRunner(pb, self._send, parent=self)
        self._runner.log.connect(self.log.appendPlainText)
        self._runner.finished.connect(self._on_finished)
        self.run_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self._runner.start()

    def _stop(self) -> None:
        if self._runner is not None:
            self._runner.stop()

    def _on_finished(self, ok: bool, msg: str) -> None:
        self.log.appendPlainText(("✓ " if ok else "✗ ") + msg)
        self.run_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self._runner = None

    # --- bridge from parent ------------------------------------------

    def feed_json(self, obj: dict) -> None:
        """The parent MainWindow forwards every inbound JSON line here so
        ``wait <event>`` steps can resolve."""
        if self._runner is not None:
            self._runner.feed_json(obj)
