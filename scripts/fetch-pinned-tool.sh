#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fetch a pinned third-party source archive, with a checksum-verified fallback.
#
# vasm and vlink are published on the author's host (sun.hasenbraten.de), which
# goes offline periodically — CI failed on 2026-10-23 with "Couldn't connect to
# server", blocking a release. When that host cannot be reached, the fallback is
# the Internet Archive's raw capture of the same URL (the `id_` form returns the
# stored bytes, not the replay page). The sha256 is checked for both attempts,
# so a fallback cannot change what gets built: it only changes where the
# identical bytes came from.
#
# A primary that *answers* with different bytes is not a transport failure: the
# upstream release changed, the pin needs reviewing, and the script fails
# instead of quietly building the archived old version.
#
# Usage: fetch-pinned-tool.sh <primary-url> <archive-url> <sha256> <output>

set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <primary-url> <archive-url> <sha256> <output>" >&2
    exit 2
fi

primary=$1
archive=$2
sha=$3
out=$4

verify() {
    # sha256sum is coreutils (Linux, MSYS2); macOS ships shasum instead.
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s  %s\n' "$sha" "$out" | sha256sum -c -
    else
        printf '%s  %s\n' "$sha" "$out" | shasum -a 256 -c -
    fi
}

fetch() {
    curl -fsSL --connect-timeout 15 --max-time 180 --retry 2 --retry-delay 2 \
        -o "$out" "$1"
}

if fetch "$primary"; then
    if verify; then
        exit 0
    fi
    # Reachable but different: the author has published a new archive, or
    # something served an error page. Falling back here would silently keep
    # building the old release, which is exactly the alarm the pin exists to
    # raise. Only transport failure takes the fallback.
    echo "primary source downloaded but does not match the pinned sha256: $primary" >&2
    echo "not using the archive copy: a changed upstream must fail loudly" >&2
    exit 1
fi

# A partial file from the failed attempt must not be mistaken for the fallback.
rm -f "$out"
echo "primary source unreachable: $primary" >&2
echo "falling back to the archived copy: $archive" >&2
fetch "$archive"
verify
