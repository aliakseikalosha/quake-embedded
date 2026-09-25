#!/bin/bash
# Run a profiling build (cmake -DPD_PROFILE=ON, usually with -DPD_BENCH=ON) on a USB-connected
# Playdate and fetch its prof.csv.
#
#   scripts/pd-bench.sh <build-dir> <label> <seconds>
#   e.g. scripts/pd-bench.sh build-prof demo1-before 105 && scripts/pd-report.py bench-results/demo1-before.csv
#
# Installs the build as Games/quake_PROF.pdx (your quake_DEVICE.pdx is not touched; the first run
# copies the whole pak, later runs only replace pdex.bin), launches it, waits <seconds>, then
# mounts the data disk again and copies Data/<bundleID>/prof.csv to bench-results/<label>.csv.
# Nothing is deleted from the device; remove Games/quake_PROF.pdx yourself when done.
#
# The Playdate goes to sleep after a few minutes (auto-lock), which disconnects USB and stops
# the run: keep <seconds> short (a timedemo of one demo takes ~80-100 s) or set Auto-lock to Never.
set -e
BUILD=${1:?usage: pd-bench.sh <build-dir> <label> <seconds>}
LABEL=${2:?label}
SECS=${3:?seconds to run}
SDK=${PLAYDATE_SDK_PATH:-$(egrep '^\s*SDKRoot' "$HOME/.Playdate/config" | head -n 1 | cut -c9-)}
PDUTIL="$SDK/bin/pdutil"
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$BUILD/quake_DEVICE.pdx"
VOL=/Volumes/PLAYDATE
GAME=quake_PROF.pdx
[ -d "$SRC" ] || { echo "$SRC not found: build the device target first"; exit 1; }
BUNDLE=$(sed -n 's/^bundleID=//p' "$SRC/pdxinfo")

port() { ls /dev/cu.usbmodemPD* 2>/dev/null | head -n 1 || true; }
wait_port() { for _ in $(seq 1 40); do p=$(port); [ -n "$p" ] && { echo "$p"; return; }; sleep 1; done; }
mount_disk() {
  [ -d "$VOL/Games" ] && return	# the Playdate is already in data-disk mode (no serial port then)
  P=$(wait_port); [ -n "$P" ] || { echo "No Playdate serial port. Wake and unlock it, and connect USB."; exit 2; }
  "$PDUTIL" "$P" datadisk
  for _ in $(seq 1 40); do [ -d "$VOL/Games" ] && return; sleep 1; done
  echo "$VOL did not appear"; exit 3
}

mount_disk
if [ -d "$VOL/Games/$GAME/id1" ]; then
  cp -X "$SRC/pdex.bin" "$VOL/Games/$GAME/pdex.bin"; cp -X "$SRC/pdxinfo" "$VOL/Games/$GAME/pdxinfo"
else
  rm -rf "$VOL/Games/$GAME"; cp -RX "$SRC" "$VOL/Games/$GAME"
fi
rm -f "$VOL/Games/._$GAME" "$VOL/Data/$BUNDLE/prof.csv"
sync; diskutil eject "$VOL" >/dev/null
sleep 3; P=$(wait_port); sleep 3
"$PDUTIL" "$P" run "/Games/$GAME"
echo "running for $SECS s ..."
sleep "$SECS"

mount_disk
mkdir -p "$HERE/bench-results"
cp "$VOL/Data/$BUNDLE/prof.csv" "$HERE/bench-results/$LABEL.csv"
echo "saved bench-results/$LABEL.csv ($(wc -l < "$HERE/bench-results/$LABEL.csv") lines)"
sync; diskutil eject "$VOL" >/dev/null
