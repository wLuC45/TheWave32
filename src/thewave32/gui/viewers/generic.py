"""Fallback viewer: tree + last 200 events. Works for any module."""

from __future__ import annotations

import json
from collections import deque
from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QPlainTextEdit, QPushButton, QSplitter, QTreeWidget,
    QTreeWidgetItem, QVBoxLayout, QWidget,
)

from .base import BaseViewer


class GenericViewer(BaseViewer):
    """Fallback for any module without a custom viewer.

    Layout:
      * Left  - recent JSON events (compact log).
      * Right - quick-action buttons (start/stop/stats/help) + last
                JSON object rendered as a tree.
    """

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._recent: deque[dict] = deque(maxlen=200)
        self._build()

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("start", primary=True, optimistic_state="running"))
        actions.addWidget(self.make_action_button("stop", optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("stats"))
        actions.addWidget(self.make_action_button("help"))
        actions.addStretch(1)

        # Left: rolling event log.
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setFont(QFont("Monospace", 9))
        self.log.setMaximumBlockCount(400)

        # Right: latest JSON as tree.
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Key", "Value"])
        self.tree.setColumnWidth(0, 200)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(self.log)
        split.addWidget(self.tree)
        split.setSizes([1, 1])

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addWidget(split, 1)

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        self._recent.append(obj)
        kind = obj.get("event") or obj.get("cmd") or "?"
        line = f"{kind}: {json.dumps({k: v for k, v in obj.items() if k not in ('event', 'cmd')}, ensure_ascii=False)}"
        self.log.appendPlainText(line)
        # Refresh the tree only for non-event payloads (e.g. stats).
        if obj.get("cmd") or "stats" in obj.get("event", ""):
            self._render_tree(obj)

    def _render_tree(self, obj: Any, parent: QTreeWidgetItem | None = None) -> None:
        if parent is None:
            self.tree.clear()
            top = QTreeWidgetItem(["root", ""])
            self.tree.addTopLevelItem(top)
            parent = top
        if isinstance(obj, dict):
            for k, v in obj.items():
                child = QTreeWidgetItem([str(k), "" if isinstance(v, (dict, list)) else str(v)])
                parent.addChild(child)
                if isinstance(v, (dict, list)):
                    self._render_tree(v, child)
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                child = QTreeWidgetItem([f"[{i}]", "" if isinstance(v, (dict, list)) else str(v)])
                parent.addChild(child)
                if isinstance(v, (dict, list)):
                    self._render_tree(v, child)
        self.tree.expandAll()
