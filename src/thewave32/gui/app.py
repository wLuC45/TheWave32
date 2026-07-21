"""TheWave32 native GUI - entry point + MainWindow.

Run with ``thewave32-gui`` (or ``python -m thewave32.gui.app``).
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
import traceback
from pathlib import Path
from typing import Optional

import time

from PySide6.QtCore import Qt, QTimer, QSize, QEvent
from PySide6.QtGui import QAction, QFont, QIcon, QPixmap, QMouseEvent
from PySide6.QtWidgets import (
    QApplication, QFrame, QHBoxLayout, QLabel, QLineEdit, QMainWindow,
    QMenu, QMessageBox, QPushButton, QSizeGrip, QSizePolicy, QSplitter,
    QStackedWidget, QStatusBar, QToolButton, QVBoxLayout, QWidget,
)
# Importing QtSvg makes sure the Qt SVG plugin is registered before any
# QPixmap(<svg>) call tries to load it. Without this on minimal PySide6
# installs the brand mark and chevrons silently fall back to a blank
# pixmap (the "icons do not load" report).
import PySide6.QtSvg  # noqa: F401

from thewave32 import compiler, flasher, log as _log, manifest, pipeline, registry
from thewave32.errors import Tw32Error
from thewave32.gui import theme
from thewave32.gui.config_dialog import ConfigDialog
from thewave32.gui.console import Console
from thewave32.gui.flash_worker import FlashWorker
from thewave32.gui.interaction_router import InteractionRouter
from thewave32.gui.log_bridge import install as install_log_bridge
from thewave32.gui.module_list import ModuleList
from thewave32.gui.serial_controller import SerialController
from thewave32.gui.viewers import BaseViewer, get_viewer_class


_logger = _log.get("gui")


class MainWindow(QMainWindow):
    """Single-window app:
       Sidebar - module browser
       Workspace - Console / Viewer tabs
       Serial work happens on a QThread; flashing on a separate QThread.
    """

    def __init__(self, modules_root: Path) -> None:
        super().__init__()
        self.setWindowTitle("TheWave32")
        self.resize(1380, 820)
        # Frameless window + custom titlebar: hide the GNOME / Mutter
        # title decoration so the app owns its top band. The window is
        # NOT translucent - we keep a solid dark-grey canvas behind.
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.Window)
        self.setAttribute(Qt.WA_TranslucentBackground, False)
        # Titlebar drag-state. _drag_active is set on left-press in the
        # titlebar background; mouseMoveEvent then hands the window to
        # the compositor via startSystemMove (X11 / Wayland safe path).
        self._drag_active = False
        self._modules_root = modules_root
        self._modules: list[registry.Module] = []
        self._current_slug: Optional[str] = None
        self._current_module: Optional[registry.Module] = None
        self._current_viewer: Optional[BaseViewer] = None
        self._flash_worker: Optional[FlashWorker] = None
        self._port: Optional[str] = None
        self._pending_inputs: Optional[dict[str, str]] = None
        # Slug of the module currently flashed onto the device. Buttons in
        # any viewer for a *different* slug stay disabled, since clicking
        # `start`/`stop` for an unflashed module would just hit the wrong
        # firmware on the wire.
        self._active_module_slug: Optional[str] = None
        # Serial worker + session log live behind one controller.
        self._serial = SerialController(parent=self)
        self._serial.json_received.connect(self._on_json)
        self._serial.raw_received.connect(self._on_raw)
        self._serial.connection_changed.connect(self._on_connection_changed)
        self._serial.error.connect(lambda m: _logger.error("serial: %s", m))
        self._serial.session_log_changed.connect(self._on_session_log_changed)
        # The playbook dialog (when open) wants every inbound JSON to
        # resolve `wait <event>` steps. Tracked here so `_on_json` can
        # forward without spawning another signal hop.
        self._playbook_dlg = None

        self._build_ui()
        install_log_bridge(self.console)
        self._interactions_router = InteractionRouter(self.interactions.append)
        self._refresh_modules()
        self._try_resolve_port(verbose=False)
        # Persist + restore the gate across sessions: if the device was
        # already flashed in a previous run, unlock that module's
        # buttons immediately so the user does not have to reflash just
        # to send `start`.
        self._restore_active_module_from_state()
        # Auto-open the serial if a port was resolved. Without this,
        # selecting an already-installed module enables its start/stop
        # buttons but _send_cmd silently drops because the serial worker
        # is not running; the user would have to click Reopen first,
        # which is unexpected when the device is clearly attached.
        if self._port is not None:
            self._open_serial()

    # ------------------------------------------------------------------
    # UI
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Top-level UI orchestration. Defers to focused helpers so this
        method itself is just the wiring graph.

        New shape (full redesign):
          +---------------------------------------------------------+
          | HEADER: brand | global actions | status pill           |
          +---------+-----------------------------------------------+
          | SIDEBAR | WORKSPACE (About  OR  module header card +    |
          | search  |            viewer stack + bottom dock for     |
          | + list  |            console / interactions)            |
          +---------+-----------------------------------------------+

        The old top QToolBar + workspace tabs (About/Module/Interactions/
        Console) are gone; everything that fed those widgets still feeds
        the same objects, only re-arranged.
        """
        self._build_actions()
        header = self._build_titlebar()
        side_w = self._build_sidebar()
        workspace = self._build_workspace()

        # --- horizontal splitter: sidebar | workspace ---
        split = QSplitter(Qt.Orientation.Horizontal)
        split.setObjectName("MainSplit")
        split.setHandleWidth(1)
        split.setChildrenCollapsible(False)
        split.addWidget(side_w)
        split.addWidget(workspace)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setCollapsible(0, False)
        split.setCollapsible(1, False)
        # The sidebar only needs room for a slug plus a little breathing
        # space; give the rest of the width to the workspace so the table
        # columns and console get the slack on resize.
        split.setSizes([228, 1152])

        central = QWidget()
        central_layout = QVBoxLayout(central)
        central_layout.setContentsMargins(0, 0, 0, 0)
        central_layout.setSpacing(0)
        central_layout.addWidget(header)
        central_layout.addWidget(split, 1)
        self.setCentralWidget(central)
        self.setMinimumSize(960, 600)
        # The bottom statusline was removed: every value it used to
        # show (port, chip, connection state, active module) is already
        # surfaced on the titlebar (StatusPill + brand). Transient
        # one-shot messages now flow through ``self.console`` instead.
        # Build the legacy widgets (and an off-screen QStatusBar) so
        # the existing setText/showMessage callers do not crash, but
        # do NOT install the bar on the window.
        self._build_status_bar()
        # A QSizeGrip in the bottom-right corner so the frameless
        # window can still be resized without an OS title bar.
        self._install_corner_grip()

    # --- _build_ui helpers --------------------------------------------

    def _build_actions(self) -> None:
        """The global QActions. Kept as QActions (handlers wired here) so
        the existing enable/disable gating is untouched; the header bar
        drives them through buttons that adopt each action."""
        self.act_flash = QAction("Flash", self)
        self.act_flash.triggered.connect(self._on_flash)
        self.act_flash.setEnabled(False)

        self.act_config = QAction("Configure", self)
        self.act_config.triggered.connect(self._on_configure)
        self.act_config.setEnabled(False)

        self.act_reopen = QAction("Reopen", self)
        self.act_reopen.triggered.connect(self._open_serial)

        self.act_stop_serial = QAction("Disconnect", self)
        self.act_stop_serial.triggered.connect(self._stop_serial)

        self.act_rescan = QAction("Rescan port", self)
        self.act_rescan.triggered.connect(lambda: self._try_resolve_port(verbose=True))

        self.act_playbook = QAction("Playbook", self)
        self.act_playbook.setToolTip(
            "Run a multi-step script against the connected device "
            "(scan, wait, attack, stop, etc.)"
        )
        self.act_playbook.triggered.connect(self._open_playbook)

        self.act_power_off = QAction("Power off", self)
        self.act_power_off.setToolTip(
            "Send 'shutdown' - chip enters deep sleep (~10µA). "
            "Press the board's RESET button to bring it back."
        )
        self.act_power_off.triggered.connect(self._on_power_off)

        self.act_logfile = QAction("Log", self)
        self.act_logfile.triggered.connect(self._open_log_file)

        # "Home" returns the workspace to the About card; selecting a
        # module flips it to the module workspace.
        self.act_home = QAction("Home", self)
        self.act_home.setToolTip("Show the About / overview card")
        self.act_home.triggered.connect(self._show_about)

    def _hdr_button(self, action: QAction, *, kind: str = "") -> QToolButton:
        """A titlebar QToolButton that adopts a QAction via setDefaultAction.
        Doing this preserves every existing handler and enable/disable
        gating - the same QAction objects keep driving the same slots,
        only the host widget changes. ``kind`` sets the QSS dynamic
        property (primary / destructive / ghost)."""
        btn = QToolButton()
        btn.setDefaultAction(action)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        if kind:
            btn.setProperty(kind, True)
        btn.setProperty("hdr", True)
        return btn

    def _build_titlebar(self) -> QWidget:
        """Custom titlebar (replaces both the GNOME chrome and the old
        HeaderBar). Composition:
          LEFT   : wave SVG mark + WindowTitle label.
          CENTER : TitleActions row carrying every global QAction as a
                   QToolButton via setDefaultAction (same handlers,
                   same gating).
          RIGHT  : StatusPill + WinMin / WinMax / WinClose.
        The bar is draggable; double-click toggles maximised.
        """
        bar = QWidget()
        bar.setObjectName("TitleBar")
        bar.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        row = QHBoxLayout(bar)
        row.setContentsMargins(10, 4, 6, 4)
        row.setSpacing(8)

        # --- brand: wave SVG mark + window title ----------------------
        brand_wrap = QWidget()
        brand_wrap.setObjectName("BrandWrap")
        brand_wrap.setCursor(Qt.CursorShape.PointingHandCursor)
        bw = QHBoxLayout(brand_wrap)
        bw.setContentsMargins(0, 0, 0, 0)
        bw.setSpacing(8)

        brand_mark = QLabel()
        brand_mark.setObjectName("BrandMark")
        svg_path = Path(__file__).parent / "assets" / "TheWave32.svg"
        pm = QPixmap(str(svg_path))
        if not pm.isNull():
            # Titlebar slot is narrow; height 18 px keeps the detailed
            # logo readable without crowding the title text. Aspect
            # ratio is preserved so the band stays clean.
            pm = pm.scaledToHeight(18, Qt.TransformationMode.SmoothTransformation)
            brand_mark.setPixmap(pm)
        brand_mark.setFixedHeight(20)
        brand_mark.setAlignment(Qt.AlignmentFlag.AlignCenter)
        bw.addWidget(brand_mark)

        title = QLabel("TheWave32")
        title.setObjectName("WindowTitle")
        _tf = QFont()
        _tf.setPointSize(11)
        _tf.setWeight(QFont.Weight.DemiBold)
        title.setFont(_tf)
        title.setMinimumWidth(title.fontMetrics().horizontalAdvance("TheWave32") + 8)
        bw.addWidget(title)

        brand_wrap.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        # Clicking the mark or wordmark is the same as Home (mirrors old
        # behaviour). The press is consumed so it does NOT also start
        # a window drag.
        brand_wrap.mousePressEvent = lambda _e: self._show_about()
        row.addWidget(brand_wrap)

        # The dedicated "Home" button was removed; clicking the brand
        # mark/wordmark (above) already routes to _show_about, which is
        # the same handler.
        row.addStretch(1)

        # --- centre/right: global actions ------------------------------
        actions = QWidget()
        actions.setObjectName("TitleActions")
        ar = QHBoxLayout(actions)
        ar.setContentsMargins(0, 0, 0, 0)
        ar.setSpacing(4)
        ar.addWidget(self._hdr_button(self.act_flash, kind="primary"))
        ar.addWidget(self._hdr_button(self.act_config))
        ar.addWidget(self._hdr_sep())
        ar.addWidget(self._hdr_button(self.act_reopen))
        ar.addWidget(self._hdr_button(self.act_stop_serial))
        ar.addWidget(self._hdr_button(self.act_rescan))
        ar.addWidget(self._hdr_sep())
        ar.addWidget(self._build_overflow_button())
        row.addWidget(actions)

        row.addStretch(1)

        # --- right: status pill + window controls ---------------------
        row.addWidget(self._build_status_pill())
        row.addSpacing(8)

        self.btn_win_min = self._make_winctl("min", "Minimize", self.showMinimized)
        self.btn_win_max = self._make_winctl(
            "max", "Maximize / Restore", self._toggle_maximised,
        )
        self.btn_win_close = self._make_winctl("close", "Close", self.close)

        row.addWidget(self.btn_win_min)
        row.addWidget(self.btn_win_max)
        row.addWidget(self.btn_win_close)

        # Drag + double-click handlers, scoped to the titlebar background
        # (children with their own handlers, e.g. the action buttons and
        # window controls, keep their normal click path).
        bar.mousePressEvent = self._titlebar_press
        bar.mouseMoveEvent = self._titlebar_move
        bar.mouseDoubleClickEvent = self._titlebar_double_click
        self._titlebar = bar
        return bar

    def _make_winctl(self, kind: str, tip: str, handler) -> QToolButton:
        """Build a window-control button (Min / Max / Close). The glyph
        is painted with QPainter so it sits on the button regardless of
        QSS image loading quirks. The button is named (WinMin / WinMax /
        WinClose) so the appearance side can style hover (Close = red)."""
        from PySide6.QtGui import QPainter, QPen, QPixmap as _QPM
        btn = QToolButton()
        obj = {"min": "WinMin", "max": "WinMax", "close": "WinClose"}[kind]
        btn.setObjectName(obj)
        btn.setToolTip(tip)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        # Paint a 24x24 glyph for crisp edges, then let QIcon downscale
        # to the requested 12x12. (Previous code combined p.scale(2,2)
        # with setDevicePixelRatio(2) which doubled the scale and made
        # the glyphs paint outside the 12-logical box: the minimize bar
        # disappeared entirely and the maximize square showed only its
        # top-right corner. The fix: drop both, paint at native 24 px.)
        pm = _QPM(24, 24)
        pm.fill(Qt.GlobalColor.transparent)
        p = QPainter(pm)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        pen = QPen()
        # TEXT token is light grey; use it so the glyph blends with the
        # rest of the chrome text instead of pure white.
        from PySide6.QtGui import QColor as _QC
        pen.setColor(_QC(theme.TEXT))
        pen.setWidthF(2.4)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        p.setPen(pen)
        # All three glyphs live inside the same 4..20 box (8..40 logical)
        # so the trio reads as a coherent set: same width, same vertical
        # extent. Min sits on the same baseline as the bottom edge of
        # the max square and the bottom corners of the close X.
        if kind == "min":
            p.drawLine(4, 18, 20, 18)
        elif kind == "max":
            p.drawRect(4, 4, 16, 16)
        else:  # close
            p.drawLine(4, 4, 20, 20)
            p.drawLine(20, 4, 4, 20)
        p.end()
        btn.setIcon(QIcon(pm))
        btn.setIconSize(QSize(12, 12))
        btn.clicked.connect(handler)
        return btn

    def _toggle_maximised(self) -> None:
        if self.isMaximized():
            self.showNormal()
        else:
            self.showMaximized()

    def _titlebar_press(self, event: QMouseEvent) -> None:
        """Start a system-side window move on left-press in the titlebar
        background. Children with their own handlers (the action toolbar
        buttons, the window controls) receive the press first via Qt's
        event dispatch, so they keep their click path."""
        if event.button() != Qt.MouseButton.LeftButton:
            return
        # Hand the drag to the compositor (X11 / Wayland). Returns False
        # on platforms that do not support it - fall through silently.
        handle = self.windowHandle()
        if handle is not None:
            try:
                handle.startSystemMove()
            except Exception:  # noqa: BLE001
                pass
        event.accept()

    def _titlebar_move(self, event: QMouseEvent) -> None:
        # The actual drag is run by the compositor after startSystemMove;
        # we still consume mouse-move so Qt does not synthesise a hover
        # storm while the drag is in flight.
        event.accept()

    def _titlebar_double_click(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        self._toggle_maximised()
        event.accept()

    def _build_overflow_button(self) -> QToolButton:
        """Header overflow: a single "More" button that drops a QMenu of
        the secondary actions (Playbook, Log, Power off). The menu hosts
        the SAME QAction objects the header buttons would have used, so
        every existing handler and gating rule still fires - only the
        host widget changes."""
        btn = QToolButton()
        btn.setObjectName("HeaderMore")
        btn.setText("More")
        btn.setToolTip("More actions: Playbook, Log, Power off")
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        btn.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        btn.setProperty("hdr", True)
        menu = QMenu(btn)
        menu.addAction(self.act_playbook)
        menu.addAction(self.act_logfile)
        menu.addSeparator()
        menu.addAction(self.act_power_off)
        btn.setMenu(menu)
        return btn

    def _hdr_sep(self) -> QFrame:
        sep = QFrame()
        sep.setObjectName("HeaderSep")
        sep.setFixedWidth(1)
        sep.setFixedHeight(22)
        return sep

    def _build_status_pill(self) -> QWidget:
        """Connection status pill: a state dot, the connected/disconnected
        word, and the resolved port. The frame border (cyan/green/red)
        is driven by a dynamic ``connected`` property flipped by
        ``_on_connection_changed`` so the pill reads at a glance."""
        pill = QFrame()
        pill.setObjectName("StatusPill")
        pill.setProperty("connected", "false")
        box = QHBoxLayout(pill)
        box.setContentsMargins(10, 2, 10, 2)
        box.setSpacing(6)
        self.lbl_conn = QLabel("o disconnected")
        self.lbl_conn.setObjectName("ConnState")
        self.lbl_conn.setStyleSheet(f"color: {theme.TEXT_MUTED}; background: transparent;")
        sep = QLabel("|")
        sep.setStyleSheet(f"color: {theme.BORDER}; background: transparent;")
        self.lbl_port = QLabel("-")
        self.lbl_port.setObjectName("PortLabel")
        self.lbl_port.setStyleSheet(f"color: {theme.TEXT_DIM}; background: transparent;")
        box.addWidget(self.lbl_conn)
        box.addWidget(sep)
        box.addWidget(self.lbl_port)
        # Pin the pill so it stays readable on narrow windows.
        pill.setMinimumWidth(178)
        pill.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        self._status_pill = pill
        return pill

    def _build_sidebar(self) -> QWidget:
        self.sidebar = ModuleList()
        self.sidebar.module_selected.connect(self._on_module_selected)

        wrap = QFrame()
        wrap.setObjectName("Sidebar")
        inner = QVBoxLayout(wrap)
        inner.setContentsMargins(6, 6, 6, 6)
        inner.setSpacing(4)

        header = QLabel("MODULES")
        header.setObjectName("SidebarHeading")
        inner.addWidget(header)

        # Search / filter field over the module list.
        self.module_filter = QLineEdit()
        self.module_filter.setObjectName("ModuleFilter")
        self.module_filter.setPlaceholderText("filter...")
        self.module_filter.setClearButtonEnabled(True)
        self.module_filter.textChanged.connect(self._on_filter_changed)
        inner.addWidget(self.module_filter)

        inner.addWidget(self.sidebar, 1)

        self.module_meta = QLabel("")
        self.module_meta.setWordWrap(True)
        self.module_meta.setProperty("secondary", True)
        self.module_meta.hide()
        inner.addWidget(self.module_meta)

        wrap.setMinimumWidth(200)
        wrap.setMaximumWidth(340)
        return wrap

    def _build_workspace(self) -> QWidget:
        """Workspace stack: About card  OR  the module workspace (header
        card + viewer + collapsible bottom dock for console/interactions)."""
        from thewave32.gui.interactions import InteractionsPanel
        from thewave32.gui.about import AboutView

        # The viewer stack is unchanged - same object fed the same way.
        self.viewer_stack = QStackedWidget()
        self.viewer_stack.setMinimumWidth(540)
        self._viewer_placeholder = QLabel("Select a module to load its viewer.")
        self._viewer_placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._viewer_placeholder.setProperty("secondary", True)
        self.viewer_stack.addWidget(self._viewer_placeholder)

        self.interactions = InteractionsPanel()
        self.console = Console()
        self.console.command_sent.connect(self._send_cmd)
        self.about = AboutView()
        # Seed the About dashboard with whatever device info we have now
        # (refreshed again whenever the port/module changes).
        self.about.set_device_info(port=self._port, chip="esp32s3")

        # --- module header card (name / version / target / desc) ---
        self.module_card = self._build_module_card()

        # --- bottom dock: Console + Interactions as full-width tabs ---
        # The Console is the primary surface; it spans the entire width
        # between the sidebar and the right edge. The Interactions
        # panel rides on a second tab so it stays accessible but does
        # not steal horizontal space from the log stream.
        from PySide6.QtWidgets import QTabWidget
        self.dock_tabs = QTabWidget()
        self.dock_tabs.setObjectName("DockTabs")
        self.dock_tabs.setDocumentMode(True)
        self.dock_tabs.setTabPosition(QTabWidget.TabPosition.North)
        self.dock_tabs.addTab(self.console, "Console")
        self.dock_tabs.addTab(self.interactions, "Interactions")
        # Kept as an alias so external code (and the work_split below)
        # that referenced dock_split still resolves.
        self.dock_split = self.dock_tabs

        # Wrap the viewer stack in a cyan-bordered frame so it reads as
        # the dominant data pane (per the terminal-tile aesthetic).
        viewer_frame = QFrame()
        viewer_frame.setObjectName("ViewerFrame")
        vf_l = QVBoxLayout(viewer_frame)
        vf_l.setContentsMargins(4, 4, 4, 4)
        vf_l.setSpacing(0)
        vf_l.addWidget(self.viewer_stack, 1)
        viewer_frame.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding,
        )

        # A vertical splitter so the user can grow/shrink (or collapse)
        # the bottom dock against the viewer.
        self.work_split = QSplitter(Qt.Orientation.Vertical)
        self.work_split.setObjectName("WorkSplit")
        self.work_split.setHandleWidth(1)
        self.work_split.setChildrenCollapsible(True)
        self.work_split.addWidget(viewer_frame)
        self.work_split.addWidget(self.dock_tabs)
        self.work_split.setStretchFactor(0, 1)
        self.work_split.setStretchFactor(1, 0)
        # Viewer wants the lion's share of the height now that inline
        # help is gone and the module card is one slim row.
        self.work_split.setSizes([640, 200])
        self.dock_tabs.setMinimumHeight(0)
        self.dock_tabs.setMaximumHeight(16777215)

        module_ws = QWidget()
        ws_l = QVBoxLayout(module_ws)
        # Tight surrounding margins - the viewer is the focus, the card
        # is a hairline strip on top.
        ws_l.setContentsMargins(6, 6, 6, 6)
        ws_l.setSpacing(4)
        ws_l.addWidget(self.module_card)
        ws_l.addWidget(self.work_split, 1)

        # About lives on its own page so the landing view is uncluttered.
        about_page = QWidget()
        ab_l = QVBoxLayout(about_page)
        ab_l.setContentsMargins(0, 0, 0, 0)
        ab_l.addWidget(self.about)

        stack = QStackedWidget()
        stack.addWidget(about_page)     # 0
        stack.addWidget(module_ws)      # 1
        self._page_about = 0
        self._page_module = 1
        stack.setCurrentIndex(self._page_about)
        self.workspace_stack = stack
        return stack

    def _build_module_card(self) -> QWidget:
        """Single compact row: <module-name> . v<version> . <target>
        plus a small "Sobre" (About) button on the right that opens the
        per-module dialog. Inline description text is gone - the dialog
        is the single home for that information.

        The frame carries a dynamic Qt property ``state`` (idle / running /
        error) that the appearance agent styles via
        ``QFrame#ModuleCard[state="running"]`` etc. ``_set_card_state``
        flips it; ``_on_json`` drives the transitions.
        """
        card = QFrame()
        card.setObjectName("ModuleCard")
        card.setProperty("state", "idle")
        card.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        row = QHBoxLayout(card)
        row.setContentsMargins(8, 3, 8, 3)
        row.setSpacing(6)

        self.card_name = QLabel("No module selected")
        self.card_name.setObjectName("CardName")
        row.addWidget(self.card_name)

        self.card_sep1 = QLabel(" . ")
        self.card_sep1.setObjectName("CardSep")
        self.card_sep1.hide()
        row.addWidget(self.card_sep1)
        self.card_version = QLabel("")
        self.card_version.setObjectName("CardMeta")
        self.card_version.hide()
        row.addWidget(self.card_version)

        self.card_sep2 = QLabel(" . ")
        self.card_sep2.setObjectName("CardSep")
        self.card_sep2.hide()
        row.addWidget(self.card_sep2)
        self.card_target = QLabel("")
        self.card_target.setObjectName("CardMeta")
        self.card_target.hide()
        row.addWidget(self.card_target)

        # Small badge that lights up when the selected module is the one
        # actually running on the device. Hidden by default and toggled
        # from _refresh_flash_gate; the same condition disables Flash.
        self.card_installed = QLabel("installed")
        self.card_installed.setObjectName("CardInstalled")
        self.card_installed.setToolTip(
            "This module is already running on the device; reflash is "
            "blocked."
        )
        self.card_installed.hide()
        row.addWidget(self.card_installed)

        row.addStretch(1)

        # Kept around (hidden) so any stray reference still resolves -
        # description now lives in the Sobre dialog.
        self.card_desc = QLabel("")
        self.card_desc.setObjectName("CardDesc")
        self.card_desc.hide()

        self.btn_sobre = QPushButton("Sobre")
        self.btn_sobre.setObjectName("SobreBtn")
        self.btn_sobre.setToolTip("Show the module's full description and viewer notes")
        self.btn_sobre.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_sobre.clicked.connect(self._on_sobre)
        self.btn_sobre.setEnabled(False)
        row.addWidget(self.btn_sobre)
        # Remember it so _set_card_state can repolish; QTimer for clearing
        # transient error state back to the prior real state.
        self.card_frame = card
        self._card_real_state = "idle"
        self._card_err_clear_timer = QTimer(self)
        self._card_err_clear_timer.setSingleShot(True)
        self._card_err_clear_timer.timeout.connect(self._clear_card_error)
        return card

    def _set_card_state(self, state: str) -> None:
        """Flip QFrame#ModuleCard's `state` dynamic property and repolish
        so the appearance agent's selectors re-apply. ``state`` is one of
        idle / running / error."""
        card = getattr(self, "card_frame", None)
        if card is None:
            return
        card.setProperty("state", state)
        card.style().unpolish(card)
        card.style().polish(card)

    def _clear_card_error(self) -> None:
        """A transient error flashes then returns to the current real
        state (running or idle)."""
        if getattr(self, "card_frame", None) is None:
            return
        if self.card_frame.property("state") == "error":
            self._set_card_state(self._card_real_state)

    def _on_sobre(self) -> None:
        """Open the Sobre (About) dialog for the active module's viewer."""
        if self._current_viewer is not None:
            try:
                self._current_viewer.show_about_dialog()
            except Exception:  # noqa: BLE001
                _logger.exception("Sobre: failed to open dialog")

    def _build_status_bar(self) -> None:
        """Dense tmux-style one-line statusline at the bottom of the
        window.

        Segments left-to-right: device (port | chip), module (slug v<ver>),
        state (connection), clock (HH:MM:SS, updated every second).

        Each segment lives in its own QLabel with an objectName the
        appearance agent can target (StatusDevice / StatusModule /
        StatusConn / StatusClock). Separator labels use objectName
        StatusSep. The bar height is pinned tight (~20-22 px) so the
        viewer keeps the slack.

        The QStatusBar itself is kept (transient flash/serial messages
        still flow through ``showMessage``) but the persistent content
        is the inner ``StatusBar`` widget we install as a permanent
        corner.
        """
        self.status = QStatusBar()
        # Frameless window has no compositor-side resize edge - enable
        # the built-in QSizeGrip in the status bar's corner so the user
        # can still resize on X11 + Wayland.
        self.status.setSizeGripEnabled(True)
        # Give the statusline enough vertical space that descenders
        # ('p', 'g', 'y') don't clip against the bottom window edge.
        # 9 pt mono on Linux measures ~13-14 px line height, so we need
        # a ~28 px inner box plus the QStatusBar's own frame to clear it.
        self.status.setContentsMargins(6, 0, 6, 0)
        self.status.setFixedHeight(40)

        bar = QWidget()
        bar.setObjectName("StatusBar")
        bar.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        bar.setFixedHeight(38)
        row = QHBoxLayout(bar)
        # 8 px top, 12 px bottom: descenders ('y' in ttyACM0, 'p' in
        # port) need real cushion against the window's bottom edge.
        row.setContentsMargins(10, 8, 10, 12)
        row.setSpacing(6)

        # device segment: <port> | <chip>
        self.lbl_status_device = QLabel("- | esp32s3")
        self.lbl_status_device.setObjectName("StatusDevice")
        row.addWidget(self.lbl_status_device)
        row.addWidget(self._status_sep())

        # module segment: <slug>  v<version>
        self.lbl_status_module = QLabel("-")
        self.lbl_status_module.setObjectName("StatusModule")
        row.addWidget(self.lbl_status_module)
        row.addWidget(self._status_sep())

        # state segment: mirrors the connection state
        self.lbl_status_conn = QLabel("disconnected")
        self.lbl_status_conn.setObjectName("StatusConn")
        row.addWidget(self.lbl_status_conn)
        row.addWidget(self._status_sep())

        # clock segment, updated every second
        self.lbl_status_clock = QLabel("--:--:--")
        self.lbl_status_clock.setObjectName("StatusClock")
        row.addStretch(1)
        row.addWidget(self.lbl_status_clock)

        # The QStatusBar widget is built and the inner StatusBar widget
        # is added to it, but we deliberately do NOT call setStatusBar:
        # the bar is never shown on the window. Old code paths that
        # update lbl_status_device / module / conn / clock therefore
        # keep working (no AttributeError) but the writes go nowhere
        # visible, which is what we want now that the titlebar carries
        # this information.
        self.status.addPermanentWidget(bar, 1)
        # The size grip used to live on the QStatusBar; with the bar
        # off-screen, _install_corner_grip parks a fresh one as an
        # overlay child of MainWindow instead (see __init__).

        # Tick the clock once per wall-clock second. The label is not
        # visible, but tests / code paths that read lbl_status_clock
        # keep getting a fresh value.
        self._clock_timer = QTimer(self)
        self._clock_timer.setInterval(1000)
        self._clock_timer.timeout.connect(self._tick_status_clock)
        self._clock_timer.start()
        self._tick_status_clock()

        # Kept so any stray reference to lbl_module does not crash; it
        # is not shown but mirrors the active module name.
        self.lbl_module = QLabel("-")

    def _install_corner_grip(self) -> None:
        """Place a QSizeGrip in the bottom-right corner so the frameless
        window can be resized. The grip is an overlay child of the
        MainWindow itself (NOT inside a layout) and is re-positioned in
        resizeEvent so it always hugs the corner.
        """
        self._corner_grip = QSizeGrip(self)
        self._corner_grip.setObjectName("CornerGrip")
        self._corner_grip.setFixedSize(16, 16)
        self._corner_grip.raise_()
        self._reposition_corner_grip()

    def _reposition_corner_grip(self) -> None:
        grip = getattr(self, "_corner_grip", None)
        if grip is None:
            return
        grip.move(self.width() - grip.width() - 2,
                  self.height() - grip.height() - 2)
        grip.raise_()

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt signature
        super().resizeEvent(event)
        self._reposition_corner_grip()

    def statusBar(self):  # noqa: N802 - Qt signature
        """Override QMainWindow.statusBar() so any stray caller that
        still asks for the status bar gets the orphan ``self.status``
        instance instead of triggering Qt to auto-create (and SHOW) a
        fresh one at the window bottom. The orphan is not parented to
        the window, so calls like ``showMessage`` are silent no-ops."""
        return self.status

    def _status_sep(self) -> QLabel:
        sep = QLabel(" | ")
        sep.setObjectName("StatusSep")
        return sep

    def _tick_status_clock(self) -> None:
        self.lbl_status_clock.setText(time.strftime("%H:%M:%S"))

    def _on_filter_changed(self, text: str) -> None:
        self.sidebar.filter(text)

    def _show_about(self) -> None:
        self.workspace_stack.setCurrentIndex(self._page_about)

    def _refresh_flash_gate(self) -> None:
        """Disable the Flash action when the currently selected module
        is the one already running on the device.

        The user's rule: do not re-flash what's already there. The
        source of truth for "what's there" is ``_active_module_slug``,
        which is updated either from ``state.json`` (last successful
        flash this host knows about) or from the device's own
        ``event:"ready"`` banner over serial. When the selected slug
        matches, we grey the button out and explain why via tooltip;
        otherwise we leave it on.

        Safe to call any time: it never overrides the disable that the
        flash worker itself sets while a flash is in progress (those
        sites do not call this).
        """
        flash_action = getattr(self, "act_flash", None)
        if flash_action is None:
            return
        active = self._active_module_slug
        current = self._current_slug
        if current is None:
            flash_action.setEnabled(False)
            flash_action.setToolTip("Select a module on the left first")
            if hasattr(self, "card_installed"):
                self.card_installed.hide()
            return
        if active is not None and active == current:
            flash_action.setEnabled(False)
            port = self._port or "the device"
            flash_action.setToolTip(
                f"{active} is already installed on {port}; reflash blocked"
            )
            if hasattr(self, "card_installed"):
                self.card_installed.show()
            return
        if hasattr(self, "card_installed"):
            self.card_installed.hide()
        flash_action.setEnabled(True)
        if active is None:
            flash_action.setToolTip(
                "Flash this module to the device"
            )
        else:
            flash_action.setToolTip(
                f"Flash {current} (currently installed: {active})"
            )

    def _restore_active_module_from_state(self) -> None:
        """Seed `_active_module_slug` from the on-disk state so that a
        module flashed in a prior session keeps its buttons unlocked.
        The next `event:"ready"` from the device will reconcile this if
        the slug actually differs from what's on the chip.
        """
        try:
            from thewave32 import state
            st = state.load(state.default_path())
        except Exception:
            _logger.exception("could not load state.json")
            return
        if not st.current_module:
            return
        self._active_module_slug = st.current_module
        _logger.info("restored active module from state: %s",
                     st.current_module)
        # If the user already has the matching viewer up, unlock it now.
        if (
            self._current_slug == st.current_module
            and self._current_viewer is not None
        ):
            self._current_viewer.set_actions_enabled(True)
        # Carrying over an installed slug across sessions; honour it.
        self._refresh_flash_gate()

    # ------------------------------------------------------------------
    # Verbose log bridge - Python `logging` → Console.append_status
    # ------------------------------------------------------------------

    # log-bridge installation now lives in gui/log_bridge.py.

    # ------------------------------------------------------------------
    # Module discovery
    # ------------------------------------------------------------------

    def _refresh_modules(self) -> None:
        try:
            mods, fails = registry.discover_with_errors(self._modules_root)
        except Exception as e:  # noqa: BLE001
            self._error_box("Failed to load modules", str(e))
            return
        self._modules = mods
        self.sidebar.populate(mods)
        if hasattr(self, "about"):
            self.about.set_modules(mods)
        for f in fails:
            self.console.append_status(f"skipped {f.name}: {f.reason}")

    # ------------------------------------------------------------------
    # Port handling
    # ------------------------------------------------------------------

    def _try_resolve_port(self, *, verbose: bool) -> None:
        try:
            self._port = flasher.resolve_port(None)
        except Tw32Error as e:
            self._port = None
            self.lbl_port.setText("-")
            self._refresh_status_device()
            if verbose:
                _logger.warning("Rescan port: no ESP32-S3 found (%s)", e)
            return
        self.lbl_port.setText(self._port)
        self._refresh_status_device()
        if verbose:
            _logger.info("Rescan port: detected %s", self._port)

    def _refresh_status_device(self) -> None:
        port = self._port or "-"
        chip = "esp32s3"
        if self._current_module is not None:
            chip = str(self._current_module.target) or "esp32s3"
        self.lbl_status_device.setText(f"{port} | {chip}")
        # About dashboard mirrors device info when present.
        if hasattr(self, "about"):
            try:
                self.about.set_device_info(port=self._port, chip=chip)
            except Exception:  # noqa: BLE001
                _logger.exception("about.set_device_info failed")

    # ------------------------------------------------------------------
    # Module switching
    # ------------------------------------------------------------------

    def _on_module_selected(self, slug: str) -> None:
        try:
            mod = registry.get(self._modules_root, slug)
        except Tw32Error as e:
            self._error_box("Module load failed", str(e))
            return
        self._current_slug = slug
        self._current_module = mod
        self.lbl_module.setText(slug)
        # tmux statusline: module slug + version, device chip refresh.
        self.lbl_status_module.setText(f"{slug}  v{mod.version}")
        self._refresh_status_device()
        # Sidebar footer keeps a compact summary.
        self.module_meta.setText(
            f"<span style='color:{theme.TEXT}; font-weight:600;'>{mod.name}</span>"
            f" <span style='color:{theme.TEXT_MUTED};'>v{mod.version}</span>"
        )
        # Compact one-row module card: name . v<ver> . <target>
        self.card_name.setText(mod.name)
        self.card_version.setText(f"v{mod.version}")
        self.card_version.show()
        self.card_sep1.show()
        self.card_target.setText(str(mod.target))
        self.card_target.show()
        self.card_sep2.show()
        # Reset the live indicator: a freshly selected module is idle
        # until the user starts it (or a `ready` event arrives).
        self._card_real_state = "idle"
        self._set_card_state("idle")
        # Description and HELP move into the Sobre dialog.
        self.card_desc.setText(mod.description or "")
        self.btn_sobre.setEnabled(True)
        self.act_flash.setEnabled(True)
        self.act_config.setEnabled(bool(mod.manifest.inputs))
        # If this is the module already on the chip, reflash is pointless
        # and we'd rather block the click than re-erase + re-write flash.
        self._refresh_flash_gate()
        self._install_viewer(slug)
        # Feed the viewer the manifest blurb so its Sobre dialog can
        # show the full module identity.
        if self._current_viewer is not None:
            try:
                self._current_viewer.set_about_info(
                    name=mod.name,
                    version=str(mod.version),
                    target=str(mod.target),
                    description=mod.description or "",
                )
            except Exception:  # noqa: BLE001
                _logger.exception("set_about_info failed")

    def _install_viewer(self, slug: str) -> None:
        cls = get_viewer_class(slug)
        viewer = cls(slug)
        viewer.send_command.connect(self._send_cmd)

        viewer.request_binary_mode = self._serial.set_binary_mode

        # Drop the previous viewer (everything except the placeholder
        # which we keep around so a future "no module" state can re-show
        # it without reconstructing).
        if (
            self._current_viewer is not None
            and self._current_viewer is not self._viewer_placeholder
        ):
            # Stop timers, drop binary mode, detach signals *before*
            # the deferred delete - otherwise a pending poll timer or a
            # stuck binary-mode worker survives the swap.
            self._current_viewer.teardown()
            self.viewer_stack.removeWidget(self._current_viewer)
            self._current_viewer.deleteLater()

        self.viewer_stack.addWidget(viewer)
        self.viewer_stack.setCurrentWidget(viewer)
        self._current_viewer = viewer

        # Buttons gate: only enable if THIS module is the one currently
        # flashed on the device. The user's rule: "flash module X →
        # previous module's buttons block automatically".
        is_active = (slug == self._active_module_slug)
        viewer.set_actions_enabled(is_active)
        if not is_active:
            _logger.info("viewer for %s installed; buttons disabled until flashed", slug)

        # Picking a module jumps to the module workspace - the About
        # page only leads on launch, not once the user starts working.
        self.workspace_stack.setCurrentIndex(self._page_module)

    # ------------------------------------------------------------------
    # Flash flow - runs in FlashWorker (off the GUI thread)
    # ------------------------------------------------------------------

    def _on_configure(self) -> None:
        if self._current_module is None:
            _logger.warning("Configure clicked but no module selected")
            return
        if not self._current_module.manifest.inputs:
            _logger.info("Configure: %s has no inputs to set",
                         self._current_module.name)
            return
        _logger.info("Configure: opening dialog for %s (%d inputs)",
                     self._current_module.name,
                     len(self._current_module.manifest.inputs))
        dlg = ConfigDialog(self._current_module.name, self._current_module.manifest.inputs, self)
        if dlg.exec():
            self._pending_inputs = dlg.values()
            _logger.info("Configure: inputs queued: %s", self._pending_inputs)
        else:
            self._pending_inputs = None
            _logger.info("Configure: cancelled")

    def _on_flash(self) -> None:
        if self._current_module is None:
            return
        if self._port is None:
            self._try_resolve_port(verbose=True)
            if self._port is None:
                self._error_box("No device", "Plug the ESP32-S3 and click Rescan port.")
                return
        if self._flash_worker is not None and self._flash_worker.isRunning():
            self.console.append_status("flash already in progress…")
            return

        raw_inputs: dict[str, str] = self._pending_inputs or {}
        if self._current_module.manifest.inputs and not raw_inputs:
            dlg = ConfigDialog(self._current_module.name,
                               self._current_module.manifest.inputs, self)
            if not dlg.exec():
                return
            raw_inputs = dlg.values()
        self._pending_inputs = None

        try:
            resolved = manifest.resolve_inputs(self._current_module.manifest.inputs, raw_inputs)
        except Tw32Error as e:
            self._error_box("Bad inputs", str(e))
            return

        self._stop_serial()
        self.act_flash.setEnabled(False)
        self.console.append_status(f"flashing {self._current_module.name} → {self._port} …")
        self.console.append_status("flashing - UI stays responsive while esptool runs")

        self._flash_worker = FlashWorker(
            self._current_module, self._port, resolved, baud=921600, parent=self,
        )
        self._flash_worker.progress.connect(self.console.append_status)
        self._flash_worker.finished_ok.connect(self._on_flash_done)
        self._flash_worker.finished_err.connect(self._on_flash_err)
        self._flash_worker.start()

    def _on_flash_done(self) -> None:
        self.act_flash.setEnabled(True)
        self.console.append_status("flash OK")
        self._flash_worker = None
        # Mark this module as the active one and unlock its viewer's
        # action buttons. Switching to any other module in the sidebar
        # later will re-disable those buttons until the user flashes
        # the new module too.
        if self._current_module is not None:
            # Gate everywhere else keys on the folder slug (state.json,
            # the device's `ready` event, _install_viewer). Use the slug
            # here too - not Module.name - so the comparison can't miss.
            self._active_module_slug = self._current_slug
            _logger.info("active module set to %s; buttons unlocked", self._active_module_slug)
            if self._current_viewer is not None:
                self._current_viewer.set_actions_enabled(True)
        # The same slug is now both selected AND installed: gate locks.
        self._refresh_flash_gate()
        self._open_serial()

    def _open_playbook(self) -> None:
        from thewave32.gui.playbook_dialog import PlaybookDialog
        if self._playbook_dlg is not None and self._playbook_dlg.isVisible():
            self._playbook_dlg.raise_()
            self._playbook_dlg.activateWindow()
            return
        self._playbook_dlg = PlaybookDialog(
            send_cmd=self._send_cmd,
            active_module=self._active_module_slug,
            parent=self,
        )
        # Tear down on close so the next open re-discovers playbooks.
        self._playbook_dlg.finished.connect(lambda _r: self._clear_playbook_dlg())
        self._playbook_dlg.show()

    def _clear_playbook_dlg(self) -> None:
        self._playbook_dlg = None

    def _on_power_off(self) -> None:
        """Shut down the connected ESP32 - deep sleep, then close the
        serial worker so we don't keep complaining about disconnects.

        Symptom we hit before: clicking Power off appeared to do nothing
        because the firmware had Wi-Fi/BT still running, which makes
        `esp_deep_sleep_start` hang silently. The shared CLI handler
        now stops the radios first; this handler also waits longer
        before tearing down the serial link so the ack JSON makes it
        back to the host before the FIFO drains.
        """
        if not self._serial.is_open:
            _logger.warning("Power off: no serial worker; reopen first")
            self.console.append_status("Power off: not connected")
            return
        _logger.info("Power off: sending 'shutdown' (deep sleep)")
        self.console.append_status(
            "Power off → deep sleep. Press RESET to wake the board."
        )
        self.console.append_status("Powering down - press RESET to wake.")
        if not self._serial.send("shutdown"):
            _logger.error("Power off: serial write failed")
            return
        # Larger drain window now that the firmware has to stop Wi-Fi /
        # Bluetooth before deep-sleeping.
        QTimer.singleShot(600, self._stop_serial)

    def _on_flash_err(self, msg: str) -> None:
        self.act_flash.setEnabled(True)
        # The flash failed, so whatever was on the chip before is still
        # there; re-evaluate the gate against that previous active slug.
        self._refresh_flash_gate()
        self._flash_worker = None
        # Trim very long tracebacks for the dialog; full text is in the log.
        short = msg if len(msg) < 600 else msg[:600] + "\n… (truncated; see ~/.cache/thewave32/thewave32.log)"
        self._error_box("Flash failed", short)

    # ------------------------------------------------------------------
    # Serial wiring
    # ------------------------------------------------------------------

    def _open_serial(self) -> None:
        if self._port is None:
            self._try_resolve_port(verbose=True)
            if self._port is None:
                _logger.warning("Reopen: no serial port resolved; click Rescan port")
                return
        self._serial.open(self._port, baud=115200)

    def _stop_serial(self) -> None:
        self._serial.close()

    def _send_cmd(self, cmd: str) -> None:
        if not self._serial.is_open:
            # Silent on the visible Console - viewers' auto-poll timers
            # (e.g. CounterViewer ticking "stats" every 2s) would spam
            # this otherwise. Still recorded in the file logger so
            # post-mortem can see the dropped traffic.
            _logger.debug("send dropped (no worker): %r", cmd)
            return
        if not self._serial.send(cmd):
            _logger.warning("send failed: %r", cmd)
            self.console.append_status(f"failed to send: {cmd}")
        else:
            self.console.append_outbound(cmd)

    def _on_session_log_changed(self, path) -> None:
        """SerialController fires this on open() and close(). Surface
        the path in the Console so the user can find the capture."""
        if path is not None:
            _logger.info("Session log: %s", path)
            self.console.append_status(f"Logging session to {path}")

    def _on_json(self, obj: dict) -> None:
        self.console.append_json(obj)
        if self._playbook_dlg is not None:
            self._playbook_dlg.feed_json(obj)
        # Live indicator on the module card. start.ok -> running,
        # stop.ok -> idle, any ok:false -> error (flashes for ~3 s,
        # then returns to the real state).
        cmd = obj.get("cmd")
        ok = obj.get("ok")
        if cmd == "start" and ok is True:
            self._card_real_state = "running"
            self._set_card_state("running")
        elif cmd == "stop" and ok is True:
            self._card_real_state = "idle"
            self._set_card_state("idle")
        elif ok is False:
            self._set_card_state("error")
            self._card_err_clear_timer.start(3000)
        # The firmware emits {"event":"ready","module":"<slug>"} every
        # boot. That's the source of truth for what's actually running
        # on the device - trust it over `state.json` (which can lag if
        # the user reflashed via the IDF CLI directly).
        if obj.get("event") == "ready":
            slug = obj.get("module")
            if isinstance(slug, str) and slug != self._active_module_slug:
                _logger.info("device reports module=%s; unlocking gate", slug)
                self._active_module_slug = slug
                if self._current_slug == slug and self._current_viewer is not None:
                    self._current_viewer.set_actions_enabled(True)
                # The device just told us what's running; if the user
                # currently has that same slug selected, lock the Flash
                # action so they don't reflash the same firmware.
                self._refresh_flash_gate()
        # Funnel real-world interactions into the dedicated tab so the
        # user can audit a session without scanning the firehose Console.
        slug = self._active_module_slug or self._current_slug
        if self._interactions_router.classify(slug, obj):
            self.workspace_stack.setCurrentIndex(self._page_module)
            # The Interactions tab is in the dock area now; surfacing
            # the credit through the Console keeps a permanent record.
            self.console.append_status(
                "Captured credentials - see Interactions tab"
            )
        if self._current_viewer is not None:
            try:
                self._current_viewer.on_json(obj)
            except Exception:
                _logger.exception("viewer error on json")

    # _classify_interaction + _diff_hid_state moved to
    # gui.interaction_router.InteractionRouter - the MainWindow now
    # delegates via self._interactions_router.classify(...).

    def _on_raw(self, data: bytes) -> None:
        # SerialController already recorded this on the session log
        # before re-emitting; this slot only deals with display.
        if self._current_viewer is not None:
            try:
                self._current_viewer.on_raw(data)
                return
            except Exception:
                _logger.exception("viewer error on raw")
        self.console.append_raw(data)

    def _on_connection_changed(self, connected: bool, message: str) -> None:
        self.console.append_status(message)
        if connected:
            self.lbl_conn.setText("* connected")
            self.lbl_conn.setStyleSheet(f"color: {theme.PRIMARY};")
            self.lbl_status_conn.setText("connected")
            self.lbl_status_conn.setStyleSheet(f"color: {theme.PRIMARY};")
        else:
            self.lbl_conn.setText("o disconnected")
            self.lbl_conn.setStyleSheet(f"color: {theme.TEXT_MUTED};")
            self.lbl_status_conn.setText("disconnected")
            self.lbl_status_conn.setStyleSheet(f"color: {theme.TEXT_MUTED};")
        # Flip the pill border colour through the dynamic property so the
        # QSS rule QFrame#StatusPill[connected="true|false"] kicks in.
        pill = getattr(self, "_status_pill", None)
        if pill is not None:
            pill.setProperty("connected", "true" if connected else "false")
            pill.style().unpolish(pill)
            pill.style().polish(pill)

    # ------------------------------------------------------------------
    # Misc
    # ------------------------------------------------------------------

    def _open_log_file(self) -> None:
        """Show a chooser between the rolling diagnostic log (Python
        side) and the per-session capture (firmware JSON stream)."""
        diag = Path.home() / ".cache" / "thewave32" / "thewave32.log"
        sess = self._serial.session_log_path
        from PySide6.QtWidgets import QInputDialog
        choices: list[tuple[str, Path]] = []
        if sess is not None and sess.is_file():
            choices.append((f"Current session: {sess.name}", sess))
        if diag.is_file():
            choices.append(("Rolling diagnostic log", diag))
        # Plus: any older session log in the directory.
        from thewave32.gui.session_log import default_dir
        d = default_dir()
        if d.is_dir():
            recent = sorted(
                (p for p in d.glob("thewave32-*.log") if p.is_file()),
                key=lambda p: p.stat().st_mtime, reverse=True,
            )[:8]
            for p in recent:
                if sess is None or p != sess:
                    choices.append((f"Session: {p.name}", p))
        if not choices:
            QMessageBox.information(self, "Log file",
                                    "No log file produced yet - "
                                    "open the serial port first.")
            return
        labels = [c[0] for c in choices]
        pick, ok = QInputDialog.getItem(
            self, "Open log", "Which log do you want to view?",
            labels, 0, False,
        )
        if not ok:
            return
        path = dict(zip(labels, [c[1] for c in choices]))[pick]
        try:
            with path.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(max(0, path.stat().st_size - 32 * 1024))
                tail = f.read()
        except OSError as e:
            self._error_box("Cannot read log", str(e))
            return
        dlg = QMessageBox(self)
        dlg.setWindowTitle(f"Log tail - {path.name}")
        dlg.setText(f"<pre style='color:{theme.TEXT_DIM};'>"
                    f"{tail[-4000:].replace('<', '&lt;')}</pre>")
        dlg.setDetailedText(f"Full path:\n{path}")
        dlg.exec()

    def _error_box(self, title: str, message: str) -> None:
        QMessageBox.critical(self, title, message)

    def closeEvent(self, e):
        if self._flash_worker is not None and self._flash_worker.isRunning():
            self._flash_worker.wait(5000)
        self._stop_serial()
        super().closeEvent(e)


# ----------------------------------------------------------------------
# Entry point
# ----------------------------------------------------------------------


def main(modules_root: Path | None = None, argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="thewave32-gui")
    parser.add_argument("--modules", default="modules")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args(argv)

    try:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (AttributeError, ValueError):
        pass

    log_path = _log.setup(verbose=args.debug)
    _logger.info("gui invoked: modules=%s", args.modules)

    chosen_root = modules_root if modules_root is not None else Path(args.modules)

    if not args.no_build:
        try:
            results = compiler.ensure_fresh(compiler.repo_root_from_modules(chosen_root))
            for r in (r for r in results if not r.ok):
                _logger.warning("build failed: %s - %s", r.slug, r.reason)
        except Exception as e:  # noqa: BLE001
            _logger.exception("auto-build aborted: %s", e)

    app = QApplication(sys.argv if argv is None else argv[:1])
    app.setApplicationName("TheWave32")
    theme.apply(app)

    # Funnel Qt's own diagnostic stream (qWarning, paint engine moans,
    # accessibility chatter) through the same logger so it lands in the
    # Console alongside our own messages. Guarded against re-entrance
    # because the bridge handler talks to widgets that may emit qWarning.
    from PySide6.QtCore import QtMsgType, qInstallMessageHandler
    import threading as _threading
    _qt_logger = _log.get("qt")
    _qt_levels = {
        QtMsgType.QtDebugMsg: logging.DEBUG,
        QtMsgType.QtInfoMsg: logging.INFO,
        QtMsgType.QtWarningMsg: logging.WARNING,
        QtMsgType.QtCriticalMsg: logging.ERROR,
        QtMsgType.QtFatalMsg: logging.CRITICAL,
    }
    _qt_recursing = _threading.local()

    def _qt_msg_handler(mtype, ctx, msg):
        if getattr(_qt_recursing, "flag", False):
            return
        _qt_recursing.flag = True
        try:
            _qt_logger.log(_qt_levels.get(mtype, logging.INFO), "%s", msg)
        finally:
            _qt_recursing.flag = False

    qInstallMessageHandler(_qt_msg_handler)

    win = MainWindow(chosen_root)
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
