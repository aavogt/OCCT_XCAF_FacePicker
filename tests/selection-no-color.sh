#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=$(realpath "${1:-"$repo_root/build/OCCT_XCAF_FacePicker"}")
for dependency in awk realpath timeout xdotool; do
  if ! command -v "$dependency" >/dev/null 2>&1; then
    printf 'missing required command: %s\n' "$dependency" >&2
    exit 1
  fi
done
if [[ ! -x "$binary" ]]; then
  printf 'viewer binary is not executable: %s\n' "$binary" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/occt-selection-no-color.XXXXXX")
viewer_pid=
cleanup() {
  if [[ -n "$viewer_pid" ]] && kill -0 "$viewer_pid" 2>/dev/null; then
    kill "$viewer_pid" 2>/dev/null || true
    wait "$viewer_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT

colorless_model="$tmpdir/colorless.step"
awk '
  /= MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION\(/ {
    print "ENDSEC;"
    print "END-ISO-10303-21;"
    found = 1
    exit
  }
  { print }
  END { if (!found) exit 1 }
' "$repo_root/cubetest.step" >"$colorless_model"
if awk '/COLOUR_RGB|STYLED_ITEM/ { found = 1 } END { exit !found }' \
  "$colorless_model"; then
  printf 'generated STEP fixture still contains color styling\n' >&2
  exit 1
fi

export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
(cd "$tmpdir" && exec "$binary" "$colorless_model") >"$tmpdir/viewer.log" 2>&1 &
viewer_pid=$!
window_id=$(timeout 20s xdotool search --sync --onlyvisible \
  --name '^OCCT XCAF Face Picker$') || {
  cat "$tmpdir/viewer.log" >&2
  printf 'viewer window did not appear\n' >&2
  exit 1
}
xdotool windowfocus --sync "$window_id"
sleep 1

# Click a face, a vertex, and an edge; the uncolored model must still print.
xdotool mousemove --window "$window_id" 800 450
sleep 0.2
xdotool mousedown 1
sleep 0.2
xdotool mouseup 1
sleep 0.3
xdotool mousemove --window "$window_id" 640 360
xdotool keydown --window "$window_id" Shift_L
sleep 0.2
xdotool mousedown 1
sleep 0.2
xdotool mouseup 1
sleep 0.2
xdotool keyup --window "$window_id" Shift_L
sleep 0.3
xdotool mousemove --window "$window_id" 950 360
xdotool keydown --window "$window_id" Control_L
sleep 0.2
xdotool mousedown 1
sleep 0.2
xdotool mouseup 1
sleep 0.2
xdotool keyup --window "$window_id" Control_L
sleep 0.3

selection_output=$(cat "$tmpdir/viewer.log")
case "$selection_output" in
  *'main.hs:'*) ;;
  *) printf '%s\n' "$selection_output" >&2; printf 'face selection did not print\n' >&2; exit 1 ;;
esac
case "$selection_output" in
  *':v'[0-9]*' snap:'*) ;;
  *) printf '%s\n' "$selection_output" >&2; printf 'vertex query did not print\n' >&2; exit 1 ;;
esac
case "$selection_output" in
  *' edge:'[0-9]*' length:'*) ;;
  *) printf '%s\n' "$selection_output" >&2; printf 'edge query did not print\n' >&2; exit 1 ;;
esac
case "$selection_output" in
  *colorQuery*) printf '%s\n' "$selection_output" >&2; printf 'unexpected colorQuery output\n' >&2; exit 1 ;;
esac
printf '%s\n' "$selection_output"
printf 'colorless face, vertex, and edge selections passed\n'
