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
remote_log="$tmpdir/nvim-remote.log"
cat >"$tmpdir/nvim-remote.sh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$1" >>"$NVIM_REMOTE_LOG"
EOF
chmod +x "$tmpdir/nvim-remote.sh"
export NVIM_REMOTE_LOG="$remote_log"
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
(cd "$tmpdir" && PATH="$tmpdir:$PATH" exec "$binary" "$colorless_model") >"$tmpdir/viewer.log" 2>&1 &
viewer_pid=$!
window_id=$(timeout 20s xdotool search --sync --onlyvisible \
  --name '^OCCT XCAF Face Picker$') || {
  cat "$tmpdir/viewer.log" >&2
  printf 'viewer window did not appear\n' >&2
  exit 1
}
xdotool windowfocus --sync "$window_id"
sleep 1

# Regular clicks report every candidate by type; Shift-left opens its source.
for coordinates in "800 450" "640 360" "950 360"; do
  read -r mouse_x mouse_y <<<"$coordinates"
  xdotool mousemove --window "$window_id" "$mouse_x" "$mouse_y"
  sleep 0.2
  xdotool mousedown 1
  sleep 0.2
  xdotool mouseup 1
  sleep 0.3
done
if [[ -e "$remote_log" ]]; then
  printf 'plain left-click unexpectedly launched nvim-remote\n' >&2
  exit 1
fi
xdotool mousemove --window "$window_id" 800 450
xdotool keydown --window "$window_id" Shift_L
sleep 0.2
xdotool mousedown 1
sleep 0.2
xdotool mouseup 1
sleep 0.2
xdotool keyup --window "$window_id" Shift_L
for attempt in {1..20}; do
  [[ -s "$remote_log" ]] && break
  sleep 0.1
done

selection_output=$(cat "$tmpdir/viewer.log")
case "$selection_output" in
  *'E:'*) ;;
  *) printf '%s\n' "$selection_output" >&2; printf 'edge candidate did not print\n' >&2; exit 1 ;;
esac
case "$selection_output" in
  *'F:'*) ;;
  *) printf '%s\n' "$selection_output" >&2; printf 'face candidate did not print\n' >&2; exit 1 ;;
esac
case "$selection_output" in
  *'V:'*) ;;
  *) printf '%s\n' "$selection_output" >&2; printf 'vertex candidate did not print\n' >&2; exit 1 ;;
esac
case "$selection_output" in
  *colorQuery*) printf '%s\n' "$selection_output" >&2; printf 'unexpected colorQuery output\n' >&2; exit 1 ;;
esac
case "$(cat "$remote_log" 2>/dev/null || true)" in
  *'main.hs:'*) ;;
  *) printf 'shift-left did not invoke nvim-remote with the source location\n' >&2; exit 1 ;;
esac
printf '%s\n' "$selection_output"
printf 'colorless face, vertex, edge, and source navigation passed\n'

