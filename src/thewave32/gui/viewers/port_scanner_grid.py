"""Grid viewer for net-port-scanner: IP × port heatmap."""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (
    QHBoxLayout, QLabel, QLineEdit, QPushButton, QTableWidget, QTableWidgetItem,
    QVBoxLayout,
)

from .base import BaseViewer


class PortScannerGridViewer(BaseViewer):
    """Builds a sparse grid as scan results come in.

    Rows = IPs seen with at least one open port.
    Columns = the configured port list.
    Cells turn green for open, blank otherwise.
    """

    def __init__(self, slug: str, parent=None) -> None:
        super().__init__(slug, parent)
        self._ports: list[int] = []
        self._row_for_ip: dict[str, int] = {}
        self._build()

    def _build(self) -> None:
        actions = QHBoxLayout()
        actions.setSpacing(8)
        actions.addWidget(self.make_action_button("connect", primary=True, optimistic_state="connected"))
        actions.addWidget(self.make_action_button("disconnect", optimistic_state="stopped"))
        actions.addWidget(self.make_action_button("scan auto", "scan auto", optimistic_state="scanning"))
        actions.addWidget(self.make_action_button("stats"))
        actions.addStretch(1)
        actions.addWidget(QLabel("ports:"))
        self.port_edit = QLineEdit()
        self.port_edit.setPlaceholderText("22,80,443,8080")
        actions.addWidget(self.port_edit)
        apply_p = QPushButton("apply")
        apply_p.clicked.connect(self._apply_ports)
        self.register_action_button(apply_p)
        actions.addWidget(apply_p)

        self.status = QLabel("Disconnected.")
        self.table = QTableWidget(0, 1)
        self.table.setHorizontalHeaderLabels(["IP"])
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        layout.addWidget(self.make_status_strip())
        layout.addLayout(actions)
        layout.addWidget(self.status)
        layout.addWidget(self.table, 1)

        self.send_command.emit("ports")

    def _apply_ports(self) -> None:
        text = self.port_edit.text().strip()
        if text:
            self.send_command.emit(f"ports {text}")
            self.send_command.emit("ports")

    def _rebuild_columns(self) -> None:
        self.table.setColumnCount(1 + len(self._ports))
        self.table.setHorizontalHeaderLabels(["IP"] + [str(p) for p in self._ports])

    def on_json(self, obj: dict[str, Any]) -> None:
        super().on_json(obj)
        cmd = obj.get("cmd")
        evt = obj.get("event")
        if cmd == "connect" and obj.get("ok") and self._status is not None:
            self._status.set_state("connected")
        elif cmd == "disconnect" and obj.get("ok") and self._status is not None:
            self._status.set_state("stopped")
        elif cmd == "scan" and obj.get("ok") and self._status is not None:
            self._status.record(
                "scan", True,
                detail=f"hosts={obj.get('hosts_seen','?')} open={obj.get('ports_open','?')}"
            )
        elif evt == "open" and self._status is not None:
            self._status.record("open", detail=f"{obj.get('ip','?')}:{obj.get('port','?')}")
        if cmd == "ports":
            ports = obj.get("ports", [])
            if isinstance(ports, list):
                self._ports = [int(p) for p in ports]
                self._rebuild_columns()
            return
        if cmd == "connect" and obj.get("ok"):
            self.status.setText(
                f"Connected - ip={obj.get('ip')}  gw={obj.get('gw')}  ssid={obj.get('ssid')}"
            )
            return
        if cmd == "disconnect" and obj.get("ok"):
            self.status.setText("Disconnected.")
            return
        # `stats` / `scan` summaries are not restated here: the
        # StatusStrip records the scan result and the grid itself shows
        # hosts (rows) and open ports (green cells).
        if evt == "open":
            ip = obj.get("ip", "")
            port = int(obj.get("port", 0))
            self._mark_open(ip, port)
            return

    def _mark_open(self, ip: str, port: int) -> None:
        if not ip:
            return
        if port not in self._ports:
            self._ports.append(port)
            self._rebuild_columns()
        col = 1 + self._ports.index(port)
        if ip not in self._row_for_ip:
            row = self.table.rowCount()
            self.table.insertRow(row)
            self._row_for_ip[ip] = row
            ip_item = QTableWidgetItem(ip)
            ip_item.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, 0, ip_item)
        row = self._row_for_ip[ip]
        cell = QTableWidgetItem("●")
        cell.setBackground(QBrush(QColor(60, 160, 90)))
        cell.setForeground(QBrush(QColor(255, 255, 255)))
        cell.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
        self.table.setItem(row, col, cell)
