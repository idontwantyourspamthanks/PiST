#!/bin/sh
# Fire up PiST in media-channel mode (docs/PLAN.md §12) so the hatari-pist
# fork drives the display: windowless emulator, frames over TCP, and the
# VirtualBox-style input grab.
#
#   ./mediademo.sh                 opens demo/keyred.s (type a key: screen goes red)
#   ./mediademo.sh path/to/x.s     opens your file instead
#
# Points PiST at the fork in the sibling checkout; pass PIST_HATARI yourself
# to use a different binary. Builds either tree when stale.
set -eu
cd "$(dirname "$0")"

BIN=build/pist
FORK=../hatari-pist
FORKBIN="$FORK/build/src/hatari"
if [ -z "${PIST_HATARI:-}" ]; then
    PIST_HATARI="$PWD/$FORKBIN"
    if [ ! -x "$PIST_HATARI" ]; then
        echo "Building hatari-pist..."
        cmake -S "$FORK" -B "$FORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_DISABLE_FIND_PACKAGE_Readline=ON
        cmake --build "$FORK/build" --parallel
    fi
fi
if [ ! -x "$BIN" ] || [ -n "$(find src CMakeLists.txt -newer "$BIN" -print -quit 2>/dev/null)" ]; then
    echo "Building PiST..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build --parallel
fi

if [ "$#" -eq 0 ]; then
    set -- demo/keyred.s
fi

cat <<'EOF'

Media demo — what to do once it is up:

  1. F5 (Run), then F9 (Continue) when the session stops at the entry
     breakpoint — the grab releases on a stop, so capture only shows
     while the machine is running.
  2. Click the emulator panel: an accent border and "Input captured —
     F12 releases" appear. Keys and mouse now belong to the guest.
  3. Type any letter: the guest wakes from Cconin, echoes it, and the
     background turns red.
  4. F12 releases the grab (or Run > Release Input, or just switch windows).

EOF

exec env -u QT_QPA_PLATFORM PIST_HATARI="$PWD/$FORKBIN" "$BIN" "$@"
