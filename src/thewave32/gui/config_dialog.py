"""Modal dialog that renders a module's ``[[inputs]]`` for flashing.

Deliberately plain: one block per input - the manifest ``prompt`` as the
label, the field below it, the default pre-filled. Manifest plumbing
(target partition, namespace, dest path) is intentionally *not* shown:
the person flashing only needs to answer the prompt, not know which
partition the value lands in.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFileDialog, QFrame,
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QSpinBox, QVBoxLayout,
    QWidget,
)

from thewave32.gui import theme
from thewave32.manifest import Input


class ConfigDialog(QDialog):
    """Renders one field per ``Input``; ``values()`` returns a flat
    ``key -> str`` dict suitable for ``manifest.resolve_inputs``."""

    def __init__(
        self,
        module_name: str,
        inputs: list[Input],
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"Configure · {module_name}")
        self.setMinimumWidth(460)
        self._widgets: dict[str, tuple[Input, object]] = {}

        root = QVBoxLayout(self)
        root.setContentsMargins(28, 24, 28, 20)
        root.setSpacing(0)

        root.addWidget(self._header(module_name, bool(inputs)))
        root.addSpacing(18)

        if not inputs:
            root.addStretch(1)
        for i, inp in enumerate(inputs):
            if i:
                root.addSpacing(16)
            root.addWidget(self._field_block(inp))

        root.addSpacing(24)
        root.addStretch(1)
        root.addWidget(self._rule())
        root.addSpacing(16)
        root.addWidget(self._buttons())

    # --- chrome --------------------------------------------------------

    def _header(self, module_name: str, has_inputs: bool) -> QWidget:
        box = QWidget()
        v = QVBoxLayout(box)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(3)
        title = QLabel(module_name)
        title.setStyleSheet(
            f"color:{theme.TEXT}; font-size:15pt; font-weight:600; "
            f"background:transparent;"
        )
        sub = QLabel(
            "These values are baked into the flash."
            if has_inputs else
            "This module has no inputs - click Flash to continue."
        )
        sub.setStyleSheet(
            f"color:{theme.TEXT_MUTED}; font-size:9.5pt; background:transparent;"
        )
        v.addWidget(title)
        v.addWidget(sub)
        return box

    def _rule(self) -> QFrame:
        line = QFrame()
        line.setFixedHeight(1)
        line.setStyleSheet(f"background:{theme.BORDER}; border:none;")
        return line

    def _buttons(self) -> QDialogButtonBox:
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        ok = buttons.button(QDialogButtonBox.StandardButton.Ok)
        ok.setText("Confirm")
        ok.setProperty("primary", True)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        return buttons

    # --- per-input block ----------------------------------------------

    def _field_block(self, inp: Input) -> QWidget:
        block = QWidget()
        v = QVBoxLayout(block)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(7)

        field = self._make_field(inp)
        self._widgets[inp.key] = (inp, field)

        # A bool reads best as a single labelled checkbox; everything
        # else gets a label stacked above its field.
        if inp.type == "bool":
            row = QHBoxLayout()
            row.setContentsMargins(0, 0, 0, 0)
            row.setSpacing(10)
            row.addWidget(field)
            row.addWidget(self._caption(inp))
            row.addStretch(1)
            v.addLayout(row)
            return block

        v.addWidget(self._caption(inp))
        v.addWidget(field)
        return block

    def _caption(self, inp: Input) -> QWidget:
        """Prompt text + a subtle required/optional marker."""
        box = QWidget()
        row = QHBoxLayout(box)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(8)
        name = QLabel(inp.prompt)
        name.setStyleSheet(
            f"color:{theme.TEXT_DIM}; font-size:10pt; font-weight:600; "
            f"background:transparent;"
        )
        row.addWidget(name)
        if inp.required:
            tag = QLabel("REQUIRED")
            tag.setStyleSheet(
                f"color:{theme.PRIMARY}; font-size:7.5pt; font-weight:700; "
                f"letter-spacing:0.8px; background:transparent;"
            )
        else:
            tag = QLabel("OPTIONAL")
            tag.setStyleSheet(
                f"color:{theme.TEXT_MUTED}; font-size:7.5pt; font-weight:700; "
                f"letter-spacing:0.8px; background:transparent;"
            )
        row.addWidget(tag)
        row.addStretch(1)
        return box

    # --- field factory -------------------------------------------------

    def _make_field(self, inp: Input):
        if inp.type == "bool":
            cb = QCheckBox()
            if inp.default in (True, "true", "1", 1):
                cb.setChecked(True)
            return cb
        if inp.type == "int":
            sb = QSpinBox()
            sb.setRange(-2_000_000_000, 2_000_000_000)
            if inp.default is not None:
                try:
                    sb.setValue(int(inp.default))
                except (TypeError, ValueError):
                    pass
            return sb
        if inp.type == "choice":
            cb = QComboBox()
            for opt in inp.options or []:
                cb.addItem(opt)
            if inp.default is not None:
                idx = cb.findText(str(inp.default))
                if idx >= 0:
                    cb.setCurrentIndex(idx)
            return cb
        if inp.type == "file":
            container = QWidget()
            row = QHBoxLayout(container)
            row.setContentsMargins(0, 0, 0, 0)
            row.setSpacing(8)
            edit = QLineEdit()
            edit.setPlaceholderText("path to a file…")
            if inp.default is not None:
                edit.setText(str(inp.default))
            btn = QPushButton("Browse…")

            def _browse() -> None:
                p, _ = QFileDialog.getOpenFileName(self, "Pick file", "")
                if p:
                    edit.setText(p)

            btn.clicked.connect(_browse)
            row.addWidget(edit, 1)
            row.addWidget(btn)
            container._edit = edit  # for value extraction
            return container
        # default: string
        edit = QLineEdit()
        if inp.default is not None:
            edit.setText(str(inp.default))
            edit.setPlaceholderText(f"default: {inp.default}")
        return edit

    # --- result extraction ---------------------------------------------

    def values(self) -> dict[str, str]:
        out: dict[str, str] = {}
        for key, (inp, field) in self._widgets.items():
            if inp.type == "bool":
                out[key] = "true" if field.isChecked() else "false"
            elif inp.type == "int":
                out[key] = str(field.value())
            elif inp.type == "choice":
                out[key] = field.currentText()
            elif inp.type == "file":
                out[key] = field._edit.text().strip()
            else:
                out[key] = field.text()
        # Drop empty optional inputs so resolve_inputs falls back to defaults.
        return {k: v for k, v in out.items() if v != ""}
