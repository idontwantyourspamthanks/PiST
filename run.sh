#!/bin/sh
# Fire up PiST for poking around: build if needed, then launch on the display.
#
#   ./run.sh                 opens demo/hello.s
#   ./run.sh path/to/x.s     opens your file instead
#
# Sessions go down the media path (docs/PLAN.md §12): the hatari-pist fork
# runs windowless and PiST renders the frames, plays the sound and owns the
# input grab. Points PiST at the fork in the sibling checkout; pass
# PIST_HATARI yourself to use a different binary. Builds only when the binary
# is missing or a source file is newer, so repeat runs are instant.
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
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
fi

# The arguments are the file to open; default to the demo so a first run has
# something on screen.
if [ "$#" -eq 0 ]; then
    set -- demo/hello.s
fi

exec env -u QT_QPA_PLATFORM PIST_HATARI="$PIST_HATARI" "$BIN" "$@"
