#!/usr/bin/env bash
# Sweep build + flash + JSON-coherence validate across every module.
#
# Output: one line per module to stdout and to /tmp/sweep_json.log;
# a final RESULT line tallies pass/fail. Designed to run unattended
# in the background.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-/dev/ttyACM0}"
RUN="${RUN:-5}"
LOG="/tmp/sweep_json.log"
DETAIL_DIR="/tmp/sweep_json_detail"

mkdir -p "$DETAIL_DIR"
: >"$LOG"

PY="${VENV_PY:-$(dirname "$(which esptool.py 2>/dev/null || echo python)")}"

# Source IDF env once.
# shellcheck source=/dev/null
source "$HOME/ESP32S3/export.sh" >/dev/null 2>&1

pass=0
fail=0
mods=()
while IFS= read -r d; do
    mods+=("$(basename "$d")")
done < <(find "$REPO/modules" -mindepth 1 -maxdepth 1 -type d \
            -exec test -f {}/module.toml \; -print | sort)

echo "sweep starts $(date -Is)  modules=${#mods[@]}" | tee -a "$LOG"

for slug in "${mods[@]}"; do
    detail="$DETAIL_DIR/$slug.log"
    : >"$detail"
    step="build"
    if ( cd "$REPO" && ./scripts/build.sh "$slug" ) >"$detail" 2>&1; then
        step="flash"
        if ( cd "$REPO/firmware/$slug" && idf.py -p "$PORT" flash ) \
                >>"$detail" 2>&1; then
            step="json"
            if "$PY" "$REPO/scripts/json_coherence.py" "$slug" \
                    --port "$PORT" --run "$RUN" >>"$detail" 2>&1; then
                summary="PASS"
                pass=$((pass+1))
            else
                summary="FAIL(json)"
                fail=$((fail+1))
            fi
        else
            summary="FAIL(flash)"
            fail=$((fail+1))
        fi
    else
        summary="FAIL(build)"
        fail=$((fail+1))
    fi
    # Pull the RESULT line + a one-line histogram if present
    one_line_hist=$(grep -m1 "^info: event histogram" "$detail" | sed 's/^info: //')
    printf "%-26s %-12s %s\n" "$slug" "$summary" "$one_line_hist" \
        | tee -a "$LOG"
done

printf "\nsweep done %s  pass=%d fail=%d total=%d\n" \
    "$(date -Is)" "$pass" "$fail" "${#mods[@]}" | tee -a "$LOG"
exit $(( fail > 0 ? 1 : 0 ))
