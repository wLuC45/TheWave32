"""Native PySide6 GUI for TheWave32.

The GUI is a thin shell over the same primitives the CLI uses
(``flasher``, ``pipeline``, ``registry``, ``manifest``); the only thing it
adds is a real-time serial reader thread, a tabbed workspace, and a
collection of pyqtgraph-based viewers that render module-specific
visualisations as data streams in.

Entry point: ``thewave32.gui.app.main``.
"""
