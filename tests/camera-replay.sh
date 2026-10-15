#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=$(realpath "${1:-"$repo_root/build/OCCT_XCAF_FacePicker"}")
model=$(realpath "${2:-"$repo_root/pulley.step"}")

for dependency in Xvfb xdotool xdpyinfo timeout awk realpath; do
  if ! command -v "$dependency" >/dev/null 2>&1; then
    printf 'missing required command: %s\n' "$dependency" >&2
    exit 1
  fi
done
if [[ ! -x "$binary" ]]; then
  printf 'viewer executable not found or not executable: %s\n' "$binary" >&2
  exit 1
fi
if [[ ! -f "$model" ]]; then
  printf 'STEP model not found: %s\n' "$model" >&2
  exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/occt-camera-replay.XXXXXX")
xvfb_pid=
viewer_pid=
cleanup() {
  if [[ -n "$viewer_pid" ]] && kill -0 "$viewer_pid" 2>/dev/null; then
    kill "$viewer_pid" 2>/dev/null || true
    wait "$viewer_pid" 2>/dev/null || true
  fi
  if [[ -n "$xvfb_pid" ]] && kill -0 "$xvfb_pid" 2>/dev/null; then
    kill "$xvfb_pid" 2>/dev/null || true
    wait "$xvfb_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT

Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp -ac \
  3>"$tmpdir/display" >"$tmpdir/xvfb.log" 2>&1 &
xvfb_pid=$!
for _ in {1..100}; do
  if [[ -s "$tmpdir/display" ]]; then
    break
  fi
  if ! kill -0 "$xvfb_pid" 2>/dev/null; then
    cat "$tmpdir/xvfb.log" >&2
    printf 'Xvfb failed to start\n' >&2
    exit 1
  fi
  sleep 0.1
done
if [[ ! -s "$tmpdir/display" ]]; then
  cat "$tmpdir/xvfb.log" >&2
  printf 'timed out waiting for Xvfb display allocation\n' >&2
  exit 1
fi
export DISPLAY=":$(<"$tmpdir/display")"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
xdpyinfo -display "$DISPLAY" >/dev/null

launch_viewer() {
  local log_file=$1
  shift
  (cd "$tmpdir" && exec "$binary" "$@") >"$log_file" 2>&1 &
  viewer_pid=$!
  window_id=$(timeout 20s xdotool search --sync --onlyvisible \
    --name '^OCCT XCAF Face Picker$') || {
    cat "$log_file" >&2
    printf 'viewer window did not appear\n' >&2
    return 1
  }
  xdotool windowfocus --sync "$window_id"
}

press_key() {
  local key=$1
  xdotool keydown --window "$window_id" "$key"
  sleep 0.2
  xdotool keyup --window "$window_id" "$key"
}

read_camera() {
  local log_file=$1
  local previous_state=${2:-}
  local line
  local state
  for _ in {1..100}; do
    state=
    while IFS= read -r line; do
      if [[ "$line" =~ ^[-+0-9.eE,]+:[-+0-9.eE,]+:[-+0-9.eE,]+$ ]]; then
        state=$line
      fi
    done < "$log_file"
    if [[ -n "$state" && "$state" != "$previous_state" ]]; then
      printf '%s\n' "$state"
      return 0
    fi
    sleep 0.1
  done
  cat "$log_file" >&2
  printf 'viewer did not print a new camera state\n' >&2
  return 1
}

apply_camera_state_cli() {
  local state=$1
  local display_mode=$2
  local log_file="$tmpdir/standalone-$display_mode.log"
  if [[ "$display_mode" == valid ]]; then
    (cd "$tmpdir" && timeout 10s env DISPLAY="$DISPLAY" \
      "$binary" "$state") >"$log_file" 2>&1 || {
      cat "$log_file" >&2
      return 1
    }
  else
    (cd "$tmpdir" && timeout 10s env -u DISPLAY \
      "$binary" "$state") >"$log_file" 2>&1 || {
      cat "$log_file" >&2
      return 1
    }
  fi
}

stop_viewer() {
  xdotool keydown --window "$window_id" Escape
  wait "$viewer_pid"
  viewer_pid=
}

camera_state_close() {
  awk -v expected="$1" -v actual="$2" '
    function abs(value) { return value < 0 ? -value : value }
    BEGIN {
      if (split(expected, expected_groups, ":") != 3 ||
          split(actual, actual_groups, ":") != 3) exit 1
      for (group = 1; group <= 3; ++group) {
        if (split(expected_groups[group], expected_values, ",") != 3 ||
            split(actual_groups[group], actual_values, ",") != 3) exit 1
        for (axis = 1; axis <= 3; ++axis) {
          expected_value = expected_values[axis] + 0
          actual_value = actual_values[axis] + 0
          scale = abs(expected_value)
          if (abs(actual_value) > scale) scale = abs(actual_value)
          if (abs(expected_value - actual_value) > 1e-9 * (1 + scale)) exit 1
        }
      }
    }
  '
}

live_log="$tmpdir/live.log"
launch_viewer "$live_log" "$model"
press_key c
initial_state=$(read_camera "$live_log")
xdotool mousemove --window "$window_id" 640 360
sleep 0.2
xdotool mousedown 3
sleep 0.2
xdotool mousemove --sync --window "$window_id" 760 430
sleep 0.2
xdotool mouseup 3
sleep 0.2
press_key c
played_state=$(read_camera "$live_log" "$initial_state")
if [[ "$initial_state" == "$played_state" ]]; then
  printf 'camera input did not change the camera state\n' >&2
  exit 1
fi
stop_viewer

replay_log="$tmpdir/replay.log"
launch_viewer "$replay_log" "$model" "$played_state"
press_key c
replayed_state=$(read_camera "$replay_log")
if ! camera_state_close "$played_state" "$replayed_state"; then
  printf 'camera replay mismatch\n  recorded: %s\n  replayed: %s\n' \
    "$played_state" "$replayed_state" >&2
  exit 1
fi

apply_camera_state_cli "$initial_state" valid
sleep 0.2
press_key c
standalone_initial=$(read_camera "$replay_log" "$replayed_state")
if ! camera_state_close "$initial_state" "$standalone_initial"; then
  printf 'standalone camera command with DISPLAY failed\n  expected: %s\n  actual: %s\n' \
    "$initial_state" "$standalone_initial" >&2
  exit 1
fi

apply_camera_state_cli "$played_state" unset
sleep 0.2
press_key c
standalone_played=$(read_camera "$replay_log" "$standalone_initial")
if ! camera_state_close "$played_state" "$standalone_played"; then
  printf 'standalone camera command without DISPLAY failed\n  expected: %s\n  actual: %s\n' \
    "$played_state" "$standalone_played" >&2
  exit 1
fi

stop_viewer
printf 'camera replay and standalone state round-trip passed\n'
