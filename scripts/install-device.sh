#!/bin/bash
# Copy quake_DEVICE.pdx to a USB-connected Playdate and launch it.
# The SDK's pdutil has no "install": mount the data disk, copy into Games/, eject, run.
set -e
cd "$(dirname "$0")/.."
PDUTIL="$PLAYDATE_SDK_PATH/bin/pdutil"
SRC=build-dev/quake_DEVICE.pdx
PDX=$(basename "$SRC")
VOL=/Volumes/PLAYDATE

if [ ! -d "$SRC" ]; then
  echo "$SRC not found. Build the device target first (task: Playdate: build (device))."
  exit 1
fi

find_port() { ls /dev/cu.usbmodemPD* 2>/dev/null | head -n 1; }

PD=$(find_port)
if [ -z "$PD" ]; then
  echo "No Playdate serial port found. Unlock the device and connect it via USB."
  exit 1
fi

echo "Mounting data disk via $PD ..."
"$PDUTIL" "$PD" datadisk

# Always leave the device tidy, including when a step below fails:
# drop the AppleDouble stub macOS creates on FAT and release the data disk.
cleanup() {
  if [ -d "$VOL/Games" ]; then
    rm -f "$VOL/Games/._$PDX" 2>/dev/null || true
    echo "Cleaning up ..."
    sync
    diskutil eject "$VOL" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 30); do [ -d "$VOL/Games" ] && break; sleep 1; done
if [ ! -d "$VOL/Games" ]; then
  echo "$VOL/Games did not appear. If the Playdate is showing 'Data Disk', mount it manually and retry."
  exit 1
fi

echo "Copying $PDX (includes pak0.pak, this takes a while) ..."
# cp, not rsync: macOS rsync fails to mkdir on the FAT data disk.
# The volume can appear before it is writable, so a first attempt may fail
# with "Operation not permitted"; retry a few times.
copied=0
for attempt in 1 2 3 4 5; do
  if rm -rf "$VOL/Games/$PDX" && cp -RX "$SRC" "$VOL/Games/$PDX"; then copied=1; break; fi
  echo "Copy failed (attempt $attempt), retrying ..."
  sleep 3
done
if [ "$copied" != 1 ]; then
  echo "Could not write to $VOL/Games ('Operation not permitted' means macOS is blocking this app)."
  echo "Allow the app running this task (VS Code / Terminal) under System Settings > Privacy & Security >"
  echo "Files and Folders > Removable Volumes (or Full Disk Access), restart it, and rerun. Running the script"
  echo "from Terminal.app instead is a quick way to check."
  exit 1
fi

cleanup            # eject before launching; the game can't start from the disk
trap - EXIT

echo "Launching ..."
for _ in $(seq 1 20); do PD=$(find_port); [ -n "$PD" ] && break; sleep 1; done
[ -n "$PD" ] && "$PDUTIL" "$PD" run "/Games/$PDX" || echo "Copied. Launch it from the Playdate's home screen."
