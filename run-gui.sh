#!/usr/bin/env bash
# Launch the TheWave32 GUI from the local .venv.
#
# The GUI needs PySide6 + pyqtgraph + pandas + numpy (and esptool for
# flashing). Those live in ./.venv, created with:
#   python -m venv --system-site-packages .venv && .venv/bin/pip install -e .
#
# By default we pass --no-build so the GUI opens immediately against the
# already-built binaries in modules/. To let the GUI recompile modules
# whose source is newer than the artifact, source the ESP-IDF environment
# first and run this script with --build:
#   source ~/ESP32S3/export.sh && ./run-gui.sh --build
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
gui="$here/.venv/bin/thewave32-gui"

if [[ ! -x "$gui" ]]; then
    echo "no .venv launcher at $gui" >&2
    echo "create it with: python -m venv --system-site-packages .venv && .venv/bin/pip install -e ." >&2
    exit 1
fi

args=(--no-build)
if [[ "${1:-}" == "--build" ]]; then
    args=()          # let the GUI's auto-build run (needs IDF_PATH set)
    shift
fi

exec "$gui" "${args[@]}" "$@"
