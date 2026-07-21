#!/usr/bin/env bash
# Build a TheWave32 firmware module and copy artefacts into modules/.
#
# Usage: ./scripts/build.sh <module-slug>
#
# Requirements:
#   - ESP-IDF exported (source $IDF_PATH/export.sh).
#   - <module-slug> exists under firmware/.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <module-slug>" >&2
    exit 2
fi

slug="$1"
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
fw_dir="${repo_root}/firmware/${slug}"
out_dir="${repo_root}/modules/${slug}"

if [[ ! -d "${fw_dir}" ]]; then
    echo "no firmware sources at ${fw_dir}" >&2
    exit 1
fi
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH not set. Run: source ${HOME}/ESP32S3/export.sh" >&2
    exit 1
fi

echo "[build] ${slug}"
( cd "${fw_dir}" && idf.py -B build build )

mkdir -p "${out_dir}"
cp "${fw_dir}/build/bootloader/bootloader.bin"            "${out_dir}/bootloader.bin"
cp "${fw_dir}/build/partition_table/partition-table.bin"  "${out_dir}/partition-table.bin"
# Find the app binary; idf.py names it after the project (e.g. wifi_scanner.bin).
app_bin="$(find "${fw_dir}/build" -maxdepth 1 -name '*.bin' \
            ! -name 'bootloader.bin' ! -name 'partition-table.bin' \
            -print -quit)"
if [[ -z "${app_bin}" ]]; then
    echo "could not locate app binary in ${fw_dir}/build" >&2
    exit 1
fi
cp "${app_bin}" "${out_dir}/firmware.bin"

echo "[build] artefacts in ${out_dir}:"
ls -lh "${out_dir}"
