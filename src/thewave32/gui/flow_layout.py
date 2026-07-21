"""Qt-style flow layout - wraps children to as many rows as needed.

Adapted from Qt's official ``layouts/flowlayout`` example (LGPL). Used
by viewers that have a variable-width set of stat cards: when the
window is wide, more cards fit per row; when narrow, they wrap.
"""

from __future__ import annotations

from PySide6.QtCore import QPoint, QRect, QSize, Qt
from PySide6.QtWidgets import QLayout, QSizePolicy, QStyle


class FlowLayout(QLayout):
    def __init__(self, parent=None, margin: int = 0, hspacing: int = 12, vspacing: int = 12):
        super().__init__(parent)
        if parent is not None:
            self.setContentsMargins(margin, margin, margin, margin)
        self._hspacing = hspacing
        self._vspacing = vspacing
        self._items = []

    def addItem(self, item):
        self._items.append(item)

    def horizontalSpacing(self) -> int:
        if self._hspacing >= 0:
            return self._hspacing
        return self._smart_spacing(QStyle.PixelMetric.PM_LayoutHorizontalSpacing)

    def verticalSpacing(self) -> int:
        if self._vspacing >= 0:
            return self._vspacing
        return self._smart_spacing(QStyle.PixelMetric.PM_LayoutVerticalSpacing)

    def count(self) -> int:
        return len(self._items)

    def itemAt(self, index):
        if 0 <= index < len(self._items):
            return self._items[index]
        return None

    def takeAt(self, index):
        if 0 <= index < len(self._items):
            return self._items.pop(index)
        return None

    def expandingDirections(self):
        return Qt.Orientations()

    def hasHeightForWidth(self) -> bool:
        return True

    def heightForWidth(self, width: int) -> int:
        return self._do_layout(QRect(0, 0, width, 0), True)

    def setGeometry(self, rect: QRect):
        super().setGeometry(rect)
        self._do_layout(rect, False)

    def sizeHint(self) -> QSize:
        return self.minimumSize()

    def minimumSize(self) -> QSize:
        size = QSize()
        for item in self._items:
            size = size.expandedTo(item.minimumSize())
        m = self.contentsMargins()
        size += QSize(m.left() + m.right(), m.top() + m.bottom())
        return size

    def _do_layout(self, rect: QRect, test_only: bool) -> int:
        m = self.contentsMargins()
        effective = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom())
        x = effective.x()
        y = effective.y()
        line_height = 0
        for item in self._items:
            wid = item.widget()
            space_x = self.horizontalSpacing()
            if space_x == -1 and wid is not None:
                space_x = wid.style().layoutSpacing(
                    QSizePolicy.ControlType.PushButton,
                    QSizePolicy.ControlType.PushButton,
                    Qt.Orientation.Horizontal,
                )
            space_y = self.verticalSpacing()
            if space_y == -1 and wid is not None:
                space_y = wid.style().layoutSpacing(
                    QSizePolicy.ControlType.PushButton,
                    QSizePolicy.ControlType.PushButton,
                    Qt.Orientation.Vertical,
                )
            next_x = x + item.sizeHint().width() + space_x
            if next_x - space_x > effective.right() and line_height > 0:
                x = effective.x()
                y = y + line_height + space_y
                next_x = x + item.sizeHint().width() + space_x
                line_height = 0
            if not test_only:
                item.setGeometry(QRect(QPoint(x, y), item.sizeHint()))
            x = next_x
            line_height = max(line_height, item.sizeHint().height())
        return y + line_height - rect.y() + m.bottom()

    def _smart_spacing(self, pm: QStyle.PixelMetric) -> int:
        parent = self.parent()
        if parent is None:
            return -1
        if parent.isWidgetType():
            return parent.style().pixelMetric(pm, None, parent)
        return parent.spacing()
