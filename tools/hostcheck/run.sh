#!/bin/bash
# tools/hostcheck/run.sh [frames]            build and run the lazy-upscale invariants on the scripted scenarios
# HGOLD=file tools/hostcheck/run.sh          write per-frame LCD hashes instead (golden mode)
# HMENU=1 tools/hostcheck/run.sh             press keys through the options menu and check what it does and saves
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=${TREE:-$(cd "$HERE/../.." && pwd)}
OUT=${OUT:-hostcheck}
"$HERE/build.sh"
mkdir -p "$HERE/out/run/id1"
PAK="$TREE/port/boards/playdate/Source/id1/pak0.pak"
[ -f "$PAK" ] || { echo "pak0.pak not found at $PAK"; exit 1; }
ln -sf "$PAK" "$HERE/out/run/id1/pak0.pak"
cd "$HERE/out/run" && "$HERE/out/$OUT" "${1:-150}"
