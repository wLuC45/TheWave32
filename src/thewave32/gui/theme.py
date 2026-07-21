"""Centralised dark-grey professional theme - Qt palette + QSS + pyqtgraph.

Identity: clean professional dark grey. A calm, contemporary dark-mode tool
with its OWN window chrome (the app is frameless and ships a custom
titlebar, so the GNOME / Mutter title bar does NOT appear). Mid-dark
neutrals with subtle elevation steps; ONE muted accent (blue); hairline
borders; no multi-colour ANSI panes.
"""

from __future__ import annotations

import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import QApplication


# --- palette ---------------------------------------------------------

# Graphite / slate neutrals. BG is a dark grey (not pure black); SURFACE
# is one step lighter, ELEVATED one more. Borders are a single hairline
# in BORDER (~#363b43), with a slightly brighter BORDER_HI for focus.
BG          = "#1c1f24"
SURFACE     = "#23272d"
ELEVATED    = "#2c3138"
BORDER      = "#363b43"
BORDER_HI   = "#4a525b"

# Neutral text scale. Body text reads comfortably on BG; dim is for
# secondary labels; muted is inactive / metadata.
TEXT        = "#e7e9ec"
TEXT_DIM    = "#a8b0bb"
TEXT_MUTED  = "#6b7480"

# One muted professional ACCENT (blue). Used for focused inputs,
# primary button outline + label, active tab underline, selected sidebar
# rail, live status dot. Never as a large fill.
PRIMARY     = "#5aa9e6"
PRIMARY_DIM = "#3b7cb0"
PRIMARY_HI  = "#7dc1f5"
ACCENT      = PRIMARY
ACCENT_DIM  = PRIMARY_DIM
ACCENT_HI   = PRIMARY_HI

SUCCESS     = "#3fb950"
WARNING     = "#d29922"
DANGER      = "#f85149"

# Legacy ANSI aliases kept so any viewer / panel that still imports
# theme.TERM_* renders against the new palette instead of crashing.
# These are NOT used by the new chrome QSS.
TERM_RED    = DANGER
TERM_GREEN  = SUCCESS
TERM_YELLOW = WARNING
TERM_BLUE   = PRIMARY
TERM_DIM    = BORDER

# Chart palette: leads with the new accent blue, then a small spread of
# harmonised hues for multi-trace plots. Tuned to read against BG.
CHART_PALETTE = [
    PRIMARY,
    "#8be9fd",   # paler cyan
    SUCCESS,
    WARNING,
    DANGER,
    "#bd93f9",   # soft violet
    "#ffb86c",   # soft orange
    "#ff79c6",   # soft pink
]


# --- application bring-up -------------------------------------------


# Monospace stack - kept for DATA surfaces only (tables, log console,
# JSON output, raw bytes). The chrome uses the UI sans stack below.
MONO = ('"Fira Code", "JetBrainsMono Nerd Font", "Hack Nerd Font", '
        '"DejaVu Sans Mono", monospace')

# UI sans stack for chrome (titlebar, buttons, labels, headings).
UI_SANS = ('"Inter", "Cantarell", "Segoe UI", "Helvetica Neue", '
           'system-ui, sans-serif')


def apply(app: QApplication) -> None:
    app.setStyle("Fusion")
    _apply_palette(app)
    app.setStyleSheet(_QSS)
    _configure_pyqtgraph()


def _apply_palette(app: QApplication) -> None:
    p = QPalette()
    p.setColor(QPalette.ColorRole.Window,           QColor(BG))
    p.setColor(QPalette.ColorRole.WindowText,       QColor(TEXT))
    p.setColor(QPalette.ColorRole.Base,             QColor(SURFACE))
    p.setColor(QPalette.ColorRole.AlternateBase,    QColor(BG))
    p.setColor(QPalette.ColorRole.ToolTipBase,      QColor(ELEVATED))
    p.setColor(QPalette.ColorRole.ToolTipText,      QColor(TEXT))
    p.setColor(QPalette.ColorRole.Text,             QColor(TEXT))
    p.setColor(QPalette.ColorRole.Button,           QColor(SURFACE))
    p.setColor(QPalette.ColorRole.ButtonText,       QColor(TEXT))
    p.setColor(QPalette.ColorRole.Highlight,        QColor(PRIMARY_DIM))
    p.setColor(QPalette.ColorRole.HighlightedText,  QColor(TEXT))
    p.setColor(QPalette.ColorRole.PlaceholderText,  QColor(TEXT_MUTED))
    p.setColor(QPalette.ColorRole.Link,             QColor(PRIMARY))
    p.setColor(QPalette.ColorRole.LinkVisited,      QColor(PRIMARY))
    # Disabled.
    p.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.WindowText, QColor(TEXT_MUTED))
    p.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.ButtonText, QColor(TEXT_MUTED))
    p.setColor(QPalette.ColorGroup.Disabled, QPalette.ColorRole.Text,       QColor(TEXT_MUTED))
    app.setPalette(p)


def _configure_pyqtgraph() -> None:
    pg.setConfigOption("background", BG)
    pg.setConfigOption("foreground", TEXT_DIM)
    pg.setConfigOptions(antialias=True, useOpenGL=False)


# --- stylesheet ------------------------------------------------------


def _chevron(direction: str, colour: str) -> str:
    """Render a small spinbox stepper chevron and return its file path.

    These replace the Fusion native arrows, whose default paint path leaks
    a stray white focus square on a focused QSpinBox. Qt's QSS ``image:``
    loader does not accept ``data:`` URIs, so the glyph is written once to
    a cache file and referenced by absolute path. Colour comes from a theme
    token (light glyph for normal, a muted variant for disabled).
    """
    import hashlib
    import os

    pts = "2,7 6,3 10,7" if direction == "up" else "2,3 6,7 10,3"
    svg = (
        "<svg xmlns='http://www.w3.org/2000/svg' width='12' height='10' "
        "viewBox='0 0 12 10'><polyline points='" + pts + "' fill='none' "
        f"stroke='{colour}' stroke-width='1.6' stroke-linecap='round' "
        "stroke-linejoin='round'/></svg>"
    )
    digest = hashlib.md5(svg.encode("utf-8")).hexdigest()[:10]
    xdg = os.environ.get("XDG_CACHE_HOME")
    base = xdg if xdg else os.path.join(os.path.expanduser("~"), ".cache")
    cache = os.path.join(base, "thewave32", "glyphs")
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, f"chevron-{direction}-{digest}.svg")
    if not os.path.exists(path):
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(svg)
    return path.replace(os.sep, "/")


_CHEVRON_UP        = _chevron("up", TEXT_DIM)
_CHEVRON_DOWN      = _chevron("down", TEXT_DIM)
_CHEVRON_UP_DIS    = _chevron("up", TEXT_MUTED)
_CHEVRON_DOWN_DIS  = _chevron("down", TEXT_MUTED)

_QSS = f"""
* {{
    font-family: {UI_SANS};
    font-size: 9.5pt;
}}

QMainWindow, QWidget {{
    background-color: {BG};
    color: {TEXT};
}}

/* --- legacy QToolBar fallback (kept for any non-titlebar use) ------ */
QMainWindow > QToolBar {{
    background: {SURFACE};
    border: none;
    border-bottom: 1px solid {BORDER};
    padding: 4px 8px;
    spacing: 4px;
    min-height: 28px;
}}
QToolBar::separator {{
    background: {BORDER}; width: 1px; margin: 6px 4px;
}}
QToolBar QToolButton {{
    background: transparent;
    color: {TEXT_DIM};
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 4px 9px;
    font-family: {UI_SANS};
}}
QToolBar QToolButton:hover {{
    background: {ELEVATED};
    color: {TEXT};
}}
QToolBar QToolButton:pressed {{
    background: {ELEVATED};
    border-color: {BORDER_HI};
}}
QToolBar QToolButton:disabled {{
    color: {TEXT_MUTED};
    border-color: transparent;
}}

/* --- buttons -------------------------------------------------------- */
QPushButton {{
    background: {SURFACE};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 5px;
    padding: 5px 11px;
    min-height: 16px;
    font-family: {UI_SANS};
}}
QPushButton:hover {{
    background: {ELEVATED};
    border-color: {BORDER_HI};
}}
QPushButton:pressed {{
    background: {ELEVATED};
    border-color: {PRIMARY};
}}
QPushButton:focus {{
    border-color: {PRIMARY};
    outline: none;
}}
QPushButton:checked {{
    background: {ELEVATED};
    border-color: {PRIMARY};
    color: {PRIMARY_HI};
}}
QPushButton:disabled {{
    color: {TEXT_MUTED};
    background: {SURFACE};
    border-color: {BORDER};
}}
QPushButton[primary="true"] {{
    background: {SURFACE};
    color: {PRIMARY_HI};
    border: 1px solid {PRIMARY};
    font-weight: 600;
}}
QPushButton[primary="true"]:hover {{
    background: {ELEVATED};
    border-color: {PRIMARY_HI};
}}
QPushButton[primary="true"]:disabled {{
    color: {TEXT_MUTED};
    border-color: {BORDER};
    background: {SURFACE};
}}
QPushButton[secondary="true"] {{
    background: transparent;
    color: {TEXT_DIM};
    border: 1px solid transparent;
}}
QPushButton[secondary="true"]:hover {{
    background: {ELEVATED};
    color: {TEXT};
    border-color: {BORDER};
}}
QPushButton[destructive="true"] {{
    background: {SURFACE};
    color: {DANGER};
    border: 1px solid {BORDER};
}}
QPushButton[destructive="true"]:hover {{
    background: {DANGER};
    color: {TEXT};
    border-color: {DANGER};
}}

/* --- list (sidebar fallback) --------------------------------------- */
QListWidget {{
    background: {BG};
    border: none;
    outline: none;
    padding: 0;
    font-family: {UI_SANS};
}}
QListWidget::item {{
    padding: 5px 10px 5px 10px;
    color: {TEXT_DIM};
    border-left: 2px solid transparent;
    border-radius: 0;
}}
QListWidget::item:hover {{
    background: {ELEVATED};
    color: {TEXT};
}}
QListWidget::item:selected {{
    background: {ELEVATED};
    color: {TEXT};
    border-left-color: {PRIMARY};
}}
QListWidget::item:disabled {{
    background: transparent;
    color: {TEXT_MUTED};
    padding: 8px 10px 2px 10px;
    border-left: none;
}}

/* --- text / inputs -------------------------------------------------- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background: {SURFACE};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 4px 8px;
    selection-background-color: {PRIMARY_DIM};
    selection-color: {TEXT};
    outline: none;
    font-family: {UI_SANS};
}}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover {{
    border-color: {BORDER_HI};
}}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{
    border-color: {PRIMARY};
}}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {{
    color: {TEXT_MUTED};
    background: {SURFACE};
    border-color: {BORDER};
}}
QSpinBox, QDoubleSpinBox {{
    padding-right: 22px;
    min-width: 80px;
}}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {{
    subcontrol-origin: border;
    background: transparent;
    width: 18px;
    border-left: 1px solid {BORDER};
    outline: none;
}}
QSpinBox::up-button, QDoubleSpinBox::up-button {{
    subcontrol-position: top right;
    border-bottom: 1px solid {BORDER};
}}
QSpinBox::down-button, QDoubleSpinBox::down-button {{
    subcontrol-position: bottom right;
}}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {{
    background: {ELEVATED};
    border-color: {BORDER_HI};
}}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {{
    image: url("{_CHEVRON_UP}");
    width: 12px; height: 10px;
}}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {{
    image: url("{_CHEVRON_DOWN}");
    width: 12px; height: 10px;
}}
QSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off,
QDoubleSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:off {{
    image: url("{_CHEVRON_UP_DIS}");
}}
QSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off,
QDoubleSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:off {{
    image: url("{_CHEVRON_DOWN_DIS}");
}}
QPlainTextEdit, QTextEdit {{
    background: {SURFACE};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    selection-background-color: {PRIMARY_DIM};
    selection-color: {TEXT};
    padding: 6px;
    font-family: {MONO};
    font-size: 9pt;
}}

/* --- table ---------------------------------------------------------- */
QTableWidget, QTreeWidget {{
    background: {SURFACE};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    gridline-color: {BORDER};
    selection-background-color: {ELEVATED};
    selection-color: {PRIMARY_HI};
    alternate-background-color: {BG};
    font-family: {MONO};
}}
QTableWidget::item, QTreeWidget::item {{
    border: none;
    padding: 2px 6px;
    font-family: {MONO};
}}
QTableWidget::item:selected, QTreeWidget::item:selected {{
    background: {ELEVATED};
    color: {PRIMARY_HI};
}}
QHeaderView::section {{
    background: {ELEVATED};
    color: {TEXT_DIM};
    border: none;
    border-bottom: 1px solid {BORDER};
    border-right: 1px solid {BORDER};
    padding: 4px 8px;
    font-family: {MONO};
    font-weight: 600;
    font-size: 8.5pt;
    letter-spacing: 0.8px;
    text-align: left;
    text-transform: uppercase;
}}
QHeaderView::section:last {{ border-right: none; }}
QHeaderView {{ background: {ELEVATED}; }}
QTableCornerButton::section {{ background: {ELEVATED}; border: none; border-bottom: 1px solid {BORDER}; }}

/* --- tabs (generic) ------------------------------------------------- */
QTabWidget::pane {{
    border: 1px solid {BORDER};
    border-radius: 4px;
    background: {SURFACE};
}}
QTabBar::tab {{
    background: transparent;
    color: {TEXT_MUTED};
    padding: 5px 12px;
    border: none;
    border-bottom: 2px solid transparent;
    font-family: {UI_SANS};
    font-size: 9pt;
}}
QTabBar::tab:hover {{
    color: {TEXT};
}}
QTabBar::tab:selected {{
    color: {TEXT};
    border-bottom-color: {PRIMARY};
}}

/* --- status bar ----------------------------------------------------- */
QStatusBar {{
    background: {SURFACE};
    color: {TEXT_MUTED};
    border-top: 1px solid {BORDER};
    /* The inner QWidget#StatusBar owns all visual space and paints its
     * own top hairline; zero the wrapper's margins/padding so descenders
     * are not clipped by the QStatusBar's default insets. */
    padding: 0;
    margin: 0;
}}
QStatusBar::item {{ border: none; }}
/* Size grip glyph - the frameless window relies on this to be the
   user's visible resize affordance. Keep it subtle but findable. */
QSizeGrip {{
    background: transparent;
    width: 14px;
    height: 14px;
}}

/* --- splitter ------------------------------------------------------- */
QSplitter::handle {{
    background: {BORDER};
}}
QSplitter::handle:horizontal {{ width: 1px; }}
QSplitter::handle:vertical   {{ height: 1px; }}

/* --- scrollbars ----------------------------------------------------- */
QScrollBar:vertical, QScrollBar:horizontal {{
    background: transparent;
    border: none;
    margin: 0;
}}
QScrollBar:vertical   {{ width: 8px; }}
QScrollBar:horizontal {{ height: 8px; }}
QScrollBar::handle {{
    background: {BORDER};
    border-radius: 4px;
    min-height: 24px;
    min-width: 24px;
}}
QScrollBar::handle:hover {{ background: {BORDER_HI}; }}
QScrollBar::add-line, QScrollBar::sub-line {{
    background: transparent;
    border: none;
    height: 0;
    width: 0;
}}
QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}

/* --- checkbox / radio ---------------------------------------------- */
QCheckBox, QRadioButton {{ color: {TEXT_DIM}; spacing: 6px; }}
QCheckBox::indicator, QRadioButton::indicator {{
    width: 12px; height: 12px;
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 3px;
}}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {{
    border-color: {PRIMARY};
}}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {{
    background: {PRIMARY};
    border-color: {PRIMARY};
}}
QRadioButton::indicator {{ border-radius: 6px; }}

QLabel {{ color: {TEXT}; background: transparent; }}
QLabel[secondary="true"] {{ color: {TEXT_MUTED}; }}
QLabel[heading="true"] {{
    color: {TEXT};
    font-weight: 600;
    font-size: 10.5pt;
    letter-spacing: 0.2px;
}}
QLabel[role="metric"] {{
    color: {TEXT};
    font-size: 18pt;
    font-weight: 500;
    letter-spacing: -0.5px;
    background: transparent;
}}
QLabel[role="caption"] {{
    color: {TEXT_MUTED};
    font-size: 7.5pt;
    letter-spacing: 1.4px;
    font-weight: 600;
    background: transparent;
    text-transform: uppercase;
}}
QLabel[role="metric-sm"] {{
    color: {TEXT};
    font-size: 11pt;
    font-weight: 500;
    background: transparent;
}}

QToolTip {{
    background: {ELEVATED};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 5px 8px;
}}

/* ==================================================================
   Custom titlebar (frameless window) - the only top band.
   ================================================================== */
QWidget#TitleBar {{
    background: {ELEVATED};
    border: none;
    border-bottom: 1px solid {BORDER};
    min-height: 38px;
    max-height: 38px;
}}
QWidget#TitleActions {{
    background: transparent;
}}
QLabel#WindowTitle {{
    color: {TEXT};
    font-family: {UI_SANS};
    font-size: 10.5pt;
    font-weight: 600;
    letter-spacing: 0.2px;
    background: transparent;
    padding-left: 4px;
}}
QWidget#BrandWrap, QLabel#BrandMark {{
    background: transparent;
}}

/* Header-bar action buttons (property hdr="true"). Flat, transparent
   default, ELEVATED-lighter on hover, accent outline for primary. */
QToolButton[hdr="true"] {{
    background: transparent;
    color: {TEXT_DIM};
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 4px 10px;
    min-height: 16px;
    font-family: {UI_SANS};
    font-weight: 500;
}}
QToolButton[hdr="true"]:hover {{
    background: {SURFACE};
    color: {TEXT};
}}
QToolButton[hdr="true"]:pressed {{
    background: {SURFACE};
    border-color: {BORDER_HI};
}}
QToolButton[hdr="true"]:disabled {{
    color: {TEXT_MUTED};
    background: transparent;
    border-color: transparent;
}}
QToolButton[hdr="true"][primary="true"] {{
    background: transparent;
    color: {PRIMARY_HI};
    border: 1px solid {PRIMARY};
    font-weight: 600;
}}
QToolButton[hdr="true"][primary="true"]:hover {{
    background: {SURFACE};
    border-color: {PRIMARY_HI};
}}
QToolButton[hdr="true"][primary="true"]:disabled {{
    color: {TEXT_MUTED};
    border-color: {BORDER};
}}
QToolButton[hdr="true"][destructive="true"] {{
    color: {DANGER};
    border-color: transparent;
}}
QToolButton[hdr="true"][destructive="true"]:hover {{
    background: {DANGER};
    color: {TEXT};
}}
QToolButton[hdr="true"][ghost="true"] {{
    color: {TEXT_MUTED};
    border-color: transparent;
}}
QToolButton[hdr="true"][ghost="true"]:hover {{
    color: {TEXT};
    background: {SURFACE};
}}
QToolButton#HeaderMore::menu-indicator {{
    image: none;
    width: 0;
    height: 0;
}}
QToolButton#HeaderMore {{
    padding-right: 10px;
}}

/* Window-control buttons (right side of the titlebar). Icons are
   painted in Python (_make_winctl); the QSS only handles fill + hover. */
QToolButton#WinMin, QToolButton#WinMax, QToolButton#WinClose {{
    background: transparent;
    border: none;
    border-radius: 4px;
    min-width: 30px;
    max-width: 30px;
    min-height: 26px;
    padding: 0;
}}
QToolButton#WinMin:hover, QToolButton#WinMax:hover {{
    background: {SURFACE};
}}
QToolButton#WinClose:hover {{
    background: {DANGER};
}}
QToolButton#WinMin:pressed, QToolButton#WinMax:pressed {{
    background: {BG};
}}

/* --- connection status pill (on the titlebar) --------------------- */
QFrame#StatusPill {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 12px;
    padding: 0 4px;
}}
QFrame#StatusPill[connected="true"] {{
    border-color: {PRIMARY};
}}
QFrame#StatusPill[connected="false"] {{
    border-color: {BORDER};
}}
QLabel#ConnState, QLabel#PortLabel {{
    font-family: {UI_SANS};
    font-size: 9pt;
    font-weight: 500;
    background: transparent;
    color: {TEXT_DIM};
}}

/* --- sidebar ------------------------------------------------------- */
QFrame#Sidebar {{
    background: {BG};
    border: none;
    border-right: 1px solid {BORDER};
}}
QLabel#SidebarHeading {{
    color: {TEXT_MUTED};
    font-family: {UI_SANS};
    font-size: 8pt;
    font-weight: 700;
    letter-spacing: 1.6px;
    padding: 4px 4px 2px 6px;
    background: transparent;
}}
QLineEdit#ModuleFilter {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 4px 8px;
    color: {TEXT};
    font-family: {UI_SANS};
}}
QLineEdit#ModuleFilter:focus {{
    border-color: {PRIMARY};
}}
QFrame#Sidebar QListWidget {{
    background: transparent;
    font-family: {UI_SANS};
}}
QFrame#Sidebar QListWidget::item {{
    border-radius: 4px;
    border-left: 2px solid transparent;
    margin: 1px 2px;
    padding: 5px 8px 5px 10px;
    color: {TEXT_DIM};
}}
QFrame#Sidebar QListWidget::item:hover {{
    background: {ELEVATED};
    color: {TEXT};
}}
QFrame#Sidebar QListWidget::item:selected {{
    background: {ELEVATED};
    color: {TEXT};
    border-left: 2px solid {PRIMARY};
}}
QFrame#Sidebar QListWidget::item:disabled {{
    background: transparent;
    color: {TEXT_MUTED};
    font-family: {UI_SANS};
    font-size: 8pt;
    font-weight: 700;
    letter-spacing: 1.4px;
    border-radius: 0;
    border-left: 2px solid transparent;
    margin: 6px 0 1px 0;
    padding: 2px 8px 2px 10px;
    text-transform: uppercase;
}}

/* --- module header card (single hairline; state via accent dot) ---- */
QFrame#ModuleCard {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 5px;
}}
QFrame#ModuleCard[state="idle"] {{
    border: 1px solid {BORDER};
}}
QFrame#ModuleCard[state="running"] {{
    border: 1px solid {PRIMARY};
}}
QFrame#ModuleCard[state="error"] {{
    border: 1px solid {DANGER};
}}
QLabel#CardName {{
    color: {TEXT};
    font-family: {UI_SANS};
    font-size: 10.5pt;
    font-weight: 600;
    letter-spacing: 0.2px;
    background: transparent;
}}
QLabel#CardSep {{
    color: {TEXT_MUTED};
    font-size: 10pt;
    background: transparent;
}}
QLabel#CardMeta {{
    color: {TEXT_DIM};
    font-family: {UI_SANS};
    font-size: 9.5pt;
    background: transparent;
}}
QLabel#CardBadge {{
    color: {TEXT_DIM};
    background: {ELEVATED};
    border: 1px solid {BORDER};
    border-radius: 3px;
    padding: 1px 5px;
    font-size: 8pt;
    font-weight: 600;
    letter-spacing: 0.6px;
}}
/* "installed" pill: lights up next to the module title when the
 * selected module is the one already running on the device. The same
 * condition disables the Flash button (see _refresh_flash_gate). */
QLabel#CardInstalled {{
    color: {PRIMARY_HI};
    background: rgba(90, 169, 230, 0.12);
    border: 1px solid {PRIMARY_DIM};
    border-radius: 3px;
    padding: 1px 7px;
    margin-left: 4px;
    font-family: {UI_SANS};
    font-size: 8pt;
    font-weight: 600;
    letter-spacing: 0.8px;
    text-transform: uppercase;
}}
QLabel#CardDesc {{
    color: {TEXT_DIM};
    font-size: 9.5pt;
    background: transparent;
}}
QPushButton#SobreBtn {{
    background: {ELEVATED};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 3px 10px;
    min-height: 14px;
    font-weight: 600;
    font-family: {UI_SANS};
}}
QPushButton#SobreBtn:hover {{
    background: {SURFACE};
    border-color: {PRIMARY};
    color: {PRIMARY_HI};
}}

/* --- viewer frame + bottom dock ----------------------------------- */
QFrame#ViewerFrame {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 5px;
}}
QTabWidget#DockTabs::pane {{
    border: 1px solid {BORDER};
    border-radius: 4px;
    background: {SURFACE};
    top: 0px;
}}
QTabWidget#DockTabs QTabBar {{
    background: transparent;
}}
QTabWidget#DockTabs QTabBar::tab {{
    background: transparent;
    color: {TEXT_MUTED};
    padding: 4px 12px;
    border: none;
    border-bottom: 2px solid transparent;
    font-family: {UI_SANS};
    font-weight: 500;
    font-size: 9pt;
}}
QTabWidget#DockTabs QTabBar::tab:hover {{
    color: {TEXT_DIM};
}}
QTabWidget#DockTabs QTabBar::tab:selected {{
    color: {TEXT};
    border-bottom: 2px solid {PRIMARY};
}}

/* --- viewer status strip ------------------------------------------ */
QWidget#ViewerStatusStrip {{
    background: {SURFACE};
    border: none;
    border-bottom: 1px solid {BORDER};
}}

/* --- splitters in the new layout ----------------------------------- */
QSplitter#MainSplit::handle, QSplitter#WorkSplit::handle,
QSplitter#DockSplit::handle {{
    background: {BORDER};
}}
QSplitter#MainSplit::handle:hover, QSplitter#WorkSplit::handle:hover,
QSplitter#DockSplit::handle:hover {{
    background: {PRIMARY};
}}
QSplitter#DockSplit::handle:horizontal {{ width: 1px; }}
QSplitter#DockSplit::handle:vertical   {{ height: 1px; }}

/* ==================================================================
   Statusline at the bottom of the window. Calm grey, hairline above.
   ================================================================== */
QWidget#StatusBar {{
    background: {SURFACE};
    border: none;
    border-top: 1px solid {BORDER};
    /* 9pt mono ~= 12-14 px line height; the previous 22 px box with 2 px
     * vertical padding clipped descenders ('p','g','y') against the
     * bottom border. Give the box real breathing room top and bottom. */
    min-height: 38px;
    max-height: 38px;
    padding: 8px 12px 12px 12px;
}}
QLabel#StatusDevice {{
    color: {TEXT_DIM};
    font-family: {MONO};
    font-size: 9pt;
    background: transparent;
    padding: 0 4px;
}}
QLabel#StatusModule {{
    color: {TEXT};
    font-family: {MONO};
    font-size: 9pt;
    font-weight: 600;
    background: transparent;
    padding: 0 4px;
}}
QLabel#StatusConn {{
    color: {TEXT_DIM};
    font-family: {MONO};
    font-size: 9pt;
    background: transparent;
    padding: 0 4px;
}}
QLabel#StatusClock {{
    color: {TEXT_MUTED};
    font-family: {MONO};
    font-size: 9pt;
    background: transparent;
    padding: 0 4px;
}}
QLabel#StatusSep {{
    color: {TEXT_MUTED};
    font-family: {MONO};
    font-size: 9pt;
    background: transparent;
    padding: 0 2px;
}}

/* --- console prompt + counter ------------------------------------- */
QLabel#PromptSymbol {{
    color: {PRIMARY};
    font-family: {MONO};
    font-size: 9pt;
    font-weight: 700;
    background: transparent;
    padding: 0 4px 0 6px;
}}
QLabel#ConsoleCounter {{
    color: {TEXT_MUTED};
    font-family: {MONO};
    font-size: 8.5pt;
    background: transparent;
    padding: 0 6px;
}}

/* --- About / Home dashboard --------------------------------------- */
QWidget#AboutPage {{
    background: {BG};
}}
QLabel#AboutHeader {{
    color: {TEXT};
    font-family: {UI_SANS};
    font-size: 11pt;
    font-weight: 700;
    letter-spacing: 0.2px;
    background: transparent;
}}
/* Entry-screen brand block: SVG mark + wordmark, centered. */
QWidget#AboutBrandRow {{
    background: transparent;
}}
QLabel#AboutBrandMark {{
    background: transparent;
    padding: 0;
}}
QLabel#AboutBrandTitle {{
    color: {TEXT};
    font-family: {UI_SANS};
    font-size: 28pt;
    font-weight: 600;
    letter-spacing: 0.5px;
    background: transparent;
    padding: 4px 0 12px 0;
}}
QLabel#AboutBody {{
    color: {TEXT_DIM};
    font-family: {UI_SANS};
    font-size: 11pt;
    line-height: 150%;
    background: transparent;
    padding: 0 16px;
}}
QLabel#AboutAuthor {{
    color: {TEXT_MUTED};
    font-family: {UI_SANS};
    font-size: 9pt;
    letter-spacing: 1.5px;
    background: transparent;
    padding-top: 8px;
}}
QTableWidget#AboutModules {{
    background: {SURFACE};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    gridline-color: {BORDER};
    selection-background-color: {ELEVATED};
    selection-color: {PRIMARY_HI};
    alternate-background-color: {BG};
    font-family: {MONO};
    font-size: 9pt;
}}
QTableWidget#AboutModules::item {{
    border: none;
    padding: 2px 6px;
}}
QTableWidget#AboutModules::item:selected {{
    background: {ELEVATED};
    color: {PRIMARY_HI};
}}
QTableWidget#AboutModules QHeaderView::section {{
    background: {ELEVATED};
    color: {TEXT_DIM};
    border: none;
    border-bottom: 1px solid {BORDER};
    border-right: 1px solid {BORDER};
    padding: 4px 8px;
    font-family: {MONO};
    font-weight: 600;
    font-size: 8.5pt;
    letter-spacing: 0.8px;
    text-align: left;
    text-transform: uppercase;
}}
QTableWidget#AboutModules QHeaderView::section:last {{ border-right: none; }}
QLabel#AboutHint {{
    color: {TEXT_MUTED};
    font-family: {UI_SANS};
    font-size: 9pt;
    font-style: italic;
    background: transparent;
    padding: 8px 0;
}}
"""
