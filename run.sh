#!/bin/sh
# Fire up PiST for poking around: build if needed, then launch on the display.
#
#   ./run.sh                 opens demo/hello.s
#   ./run.sh path/to/x.s     opens your file instead
#
# Builds only when the binary is missing or a source file is newer, so repeat
# runs are instant. Always uses the native display; if you want the headless
# checks, that is the test suite, not this script.
set -eu
cd "$(dirname "$0")"

BIN=build/pist

if [ ! -x "$BIN" ] || [ -n "$(find src CMakeLists.txt -newer "$BIN" -print -quit 2>/dev/null)" ]; then
    echo "Building PiST..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build --parallel
fi

# The arguments are the file to open; default to the demo so a first run has
# something on screen.
if [ "$#" -eq 0 ]; then
    set -- demo/hello.s
fi

exec env -u QT_QPA_PLATFORM "$BIN" "$@"
