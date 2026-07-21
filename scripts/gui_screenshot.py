#!/usr/bin/env python3
"""Headless screenshot of the TheWave32 GUI for visual iteration.

Builds the real MainWindow under Qt's offscreen platform and grabs it to
a PNG, without opening a window or touching the user's display. Optionally
selects a module so a specific viewer is shown. Use this to look at layout
or theme changes while iterating.

Run with the project venv (see run-gui.sh / .venv):
  QT_QPA_PLATFORM=offscreen .venv/bin/python scripts/gui_screenshot.py \
      --out /tmp/gui.png --width 1380 --height 860 [--slug wifi-clock-skew]

Then open the PNG (an agent can Read it directly).
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication  # noqa: E402
from PySide6.QtCore import QTimer  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/gui.png")
    ap.add_argument("--width", type=int, default=1380)
    ap.add_argument("--height", type=int, default=860)
    ap.add_argument("--modules", default="modules")
    ap.add_argument("--slug", default=None,
                    help="select this module so its viewer is shown")
    ap.add_argument("--delay", type=int, default=1800,
                    help="ms to let the UI settle before grabbing")
    args = ap.parse_args()

    from thewave32.gui import app as gapp, theme

    qapp = QApplication(["thewave32-gui"])
    qapp.setApplicationName("TheWave32")
    theme.apply(qapp)

    win = gapp.MainWindow(Path(args.modules))
    win.resize(args.width, args.height)
    win.show()

    def grab_and_quit() -> None:
        if args.slug:
            # Drive the real sidebar-selection path (module_selected ->
            # _on_module_selected) so the module header card populates, not
            # just the viewer. Guarded against internal renames.
            try:
                win.sidebar.module_selected.emit(args.slug)
            except Exception as exc:  # noqa: BLE001
                print(f"warn: could not select {args.slug!r}: {exc}", file=sys.stderr)
                try:
                    win._install_viewer(args.slug)  # noqa: SLF001
                except Exception:  # noqa: BLE001
                    pass
            QTimer.singleShot(600, _shoot)
        else:
            _shoot()

    def _shoot() -> None:
        pm = win.grab()
        ok = pm.save(args.out, "PNG")
        print(f"saved={ok} out={args.out} size={pm.width()}x{pm.height()}")
        qapp.quit()

    QTimer.singleShot(args.delay, grab_and_quit)
    return qapp.exec()


if __name__ == "__main__":
    raise SystemExit(main())
