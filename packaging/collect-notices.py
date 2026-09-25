#!/usr/bin/env python3
"""Assemble the third-party licence notices for a staged release bundle.

The set shipped is derived from what the bundle actually CONTAINS — the
binaries present, the libraries the bundled Hatari links, and every shared
library staged into the bundle — matched against the components whose licences
oblige us to ship their text. It is not a static list, so a build-host change
cannot silently alter what we distribute without altering the notices. That
has already happened once in reverse: whether Hatari links Capstone depends on
whether the build host had it, and a static list either ships a BSD text for a
library that is absent or misses the text for one that is present.

Used by the release workflow on the staged tree before archiving — and on the
AppDir before linuxdeploy consumes it (a second linuxdeploy pass is not an
option: re-running the qt plugin on a populated AppDir demands QML tooling a
fresh AppDir does not need). Qt is therefore attested with --expect-qt there,
and the archive verification asserts the texts are inside the final artifact.
Also runnable by hand:

    python3 packaging/collect-notices.py --root AppDir/usr --qt-version 6.8.1 \
        --vasm-tarball vasm.tar.gz --expect-qt

`--root` is the prefix that contains bin/ and share/ (the archive staging dir,
or AppDir/usr for the AppImage). Licence texts land in
<root>/share/doc/pist/licenses/ and a manifest in <root>/share/doc/pist/
THIRD-PARTY.txt.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

NOTICES_DIR = Path(__file__).resolve().parent / "notices"

# File-name substrings of a library -> (component, [licence texts], licence
# name). These are the libraries whose licences oblige us to ship their text
# when they travel inside the archive. Which of them Hatari links — and which
# the deploy step consequently bundles — is a property of the build host, not
# of Hatari's source, so it is discovered per artifact. Each text names its own
# copyright holder: SDL2's licence file is not zlib's. Windows names differ
# from Unix ones (SDL2.dll, zlib1.dll), so each component carries every name
# its library goes by.
KNOWN_LIBS = {
    ("libSDL2", "SDL2.dll"): ("SDL2", ["SDL2-LICENSE.txt"], "zlib licence"),
    ("libpng",): ("libpng", ["libpng.txt"], "libpng licence"),
    ("libcapstone", "capstone.dll"): ("Capstone", ["BSD-3-Clause-Capstone.txt"], "BSD 3-clause"),
    ("libz.", "zlib1"): ("zlib", ["zlib.txt"], "zlib licence"),
    # The MinGW runtime DLLs an MSYS2-built emulator carries. GCC's runtime
    # libraries are GPL v3 with the Runtime Library Exception, and the
    # exception text is what permits shipping them beside a GPL v2 program.
    ("libgcc_s", "libstdc++", "libwinpthread"): (
        "GCC runtime (libgcc/libstdc++/libwinpthread)",
        ["GPL-3.0.txt", "GCC-Runtime-Exception.txt"],
        "GPL v3 or later, with the GCC Runtime Library Exception"),
}


def fail(message):
    print(f"collect-notices: ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def find_binary(root, names):
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.name in names:
            return path
    return None


def linked_libraries(binary, platform):
    """The library names a binary links, via ldd (Linux) or otool (macOS)."""
    if platform == "linux":
        out = subprocess.run(["ldd", str(binary)], capture_output=True, text=True)
        if out.returncode != 0:
            fail(f"ldd failed on {binary}: {out.stdout}{out.stderr}")
        return [line.split()[0] for line in out.stdout.splitlines() if "=>" in line]
    if platform == "macos":
        out = subprocess.run(["otool", "-L", str(binary)], capture_output=True, text=True)
        if out.returncode != 0:
            fail(f"otool failed on {binary}: {out.stdout}{out.stderr}")
        return [line.strip().split()[0] for line in out.stdout.splitlines()[1:]]
    # Windows has neither tool in the packaging shell, and no Hatari is bundled
    # there, so there is nothing to inspect — which the caller asserts.
    fail(f"cannot inspect linked libraries on platform {platform!r}")


def bundled_library_names(root):
    """File names of every shared library staged in the bundle."""
    names = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        name = path.name
        if ".so" in name or name.endswith((".dylib", ".dll")):
            names.append(name)
    return names


def manual_licence_text(tarball, texi_suffix, header):
    """Extract the Legal section from a texinfo manual in the pinned tarball.

    vasm and vlink ship no standalone licence file; their redistribution terms
    are the 'Legal' section of the manual. They must travel with the binary,
    and extracting them from the pinned source means the text cannot drift
    from what was actually built.
    """
    # List-then-extract-exact: GNU tar needs --wildcards for patterns while
    # bsdtar patterns implicitly and rejects the flag, so patterns are avoided.
    members = subprocess.run(["tar", "-tzf", str(tarball)], capture_output=True, text=True)
    if members.returncode != 0:
        fail(f"could not list {tarball}: {members.stderr}")
    member = next((m for m in members.stdout.splitlines() if m.endswith(texi_suffix)), None)
    if not member:
        fail(f"{tarball} contains no {texi_suffix}; the licence terms must ship with the binary")
    out = subprocess.run(["tar", "-xOf", str(tarball), member],
                         capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout:
        fail(f"could not read {member} from {tarball}")
    match = re.search(r"@section Legal\n(.*?)\n@section ", out.stdout, re.DOTALL)
    if not match:
        fail(f"{member} has no Legal section; the licence terms must ship with the binary")
    body = match.group(1)
    # texinfo markup the reader does not need.
    body = re.sub(r"@code\{([^}]*)\}", r"\1", body)
    body = re.sub(r"@emph\{([^}]*)\}", r"\1", body)
    return header + body.strip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", required=True, type=Path,
                        help="staging prefix containing bin/ and share/")
    parser.add_argument("--platform", required=True, choices=["linux", "macos", "windows"])
    parser.add_argument("--qt-version", required=True,
                        help="Qt version the bundle deploys, for the LGPL source offer")
    parser.add_argument("--vasm-tarball", type=Path,
                        help="the pinned vasm source tarball (its licence section ships)")
    parser.add_argument("--vlink-tarball", type=Path,
                        help="the pinned vlink source tarball (its licence section ships)")
    parser.add_argument("--expect-qt", action="store_true",
                        help="Qt is deployed AFTER this script runs (the AppDir case): "
                             "ship its notice without the marker check. The archive "
                             "verification then asserts the text is present in the "
                             "final artifact, so a broken deploy still fails loudly.")
    parser.add_argument("--libretro-commit",
                        help="full lowercase sha of the pist-libretro commit the "
                             "in-process core was built from. The copy "
                             "into the app happens after this script, so the "
                             "packaging job names the commit before the file is "
                             "inside --root. Required when that library is already "
                             "in the tree.")
    args = parser.parse_args()

    root = args.root
    if not root.is_dir():
        fail(f"staging prefix {root} does not exist")
    doc = root / "share" / "doc" / "pist"
    licenses = doc / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)

    components = []  # (name, licence name, [licence texts], note)

    # PiST itself is always present.
    components.append(("PiST", "GPL v2 or later", [], "see LICENSE"))

    # Qt: detected by the deployed runtime's presence where it has been
    # deployed, or attested with --expect-qt where the deployer runs later —
    # linuxdeploy/macdeployqt/windeployqt each place it differently. The LGPLv3
    # incorporates the GPLv3 by reference, so both texts must travel.
    qt_component = (f"Qt {args.qt_version} (dynamically linked)",
                    "LGPL v3 (incorporating GPL v3)",
                    ["LGPL-3.0.txt", "GPL-3.0.txt"],
                    "source: https://download.qt.io/official_releases/qt/ "
                    f"(version {args.qt_version}) — the LGPL relink and "
                    "source-offer obligations are met by dynamic linking "
                    "plus this offer")
    qt_markers = list(root.rglob("libQt6Core.so*")) + list(root.rglob("Qt6Core.dll")) \
        + list(root.rglob("QtCore.framework"))
    if qt_markers or args.expect_qt:
        components.append(qt_component)
    else:
        fail("no Qt runtime found in the bundle — the deploy step did not run?")

    # vasm: present on every platform. Its licence terms must travel with it,
    # and they live only in the source tarball's manual.
    if find_binary(root, {"vasmm68k_mot", "vasmm68k_mot.exe"}):
        if not args.vasm_tarball:
            fail("vasm is bundled but --vasm-tarball was not given: "
                 "its licence terms cannot be shipped without it")
        (licenses / "vasm-LICENCE.txt").write_text(manual_licence_text(
            args.vasm_tarball, "/doc/vasm.texi",
            "vasm licence terms (from the Legal section of doc/vasm.texi,\n"
            "in the pinned upstream source tarball this binary was built from)\n\n"))
        components.append(("vasm (vasmm68k_mot)", "non-free; redistribution unmodified, "
                           "non-commercial use", ["vasm-LICENCE.txt"],
                           "source: http://sun.hasenbraten.de/vasm/ (pinned tarball)"))

    # vlink: same author-hosted terms as vasm, same extraction route; the
    # manual lives at the tarball's top level rather than in doc/.
    if find_binary(root, {"vlink", "vlink.exe"}):
        if not args.vlink_tarball:
            fail("vlink is bundled but --vlink-tarball was not given: "
                 "its licence terms cannot be shipped without it")
        (licenses / "vlink-LICENCE.txt").write_text(manual_licence_text(
            args.vlink_tarball, "/vlink.texi",
            "vlink licence terms (from the Legal section of vlink.texi,\n"
            "in the pinned upstream source tarball this binary was built from)\n\n"))
        components.append(("vlink", "non-free; redistribution unmodified, "
                           "non-commercial use", ["vlink-LICENCE.txt"],
                           "source: http://sun.hasenbraten.de/vlink/ (pinned tarball)"))

    # EmuTOS: the default ROM.
    if list(root.rglob("etos1024k.img")):
        components.append(("EmuTOS 1.4 (etos1024k.img)", "GPL v2", ["GPL-2.0.txt"],
                           "source: https://emutos.sourceforge.net/"))

    # Hatari: bundled in the Linux AppImage and the Windows archive. Linked
    # libraries can be inspected on Linux/macOS only; on Windows the KNOWN_LIBS
    # matching below relies on the DLLs staged beside the emulator, which the
    # packaging job copies from the MSYS2 sysroot the emulator was built against.
    hatari = find_binary(root, {"hatari", "hatari.exe", "Hatari"})

    # The known-licence libraries that travel with the artifact, found two
    # ways: the bundled Hatari's direct links (which the deployer will bundle),
    # and every shared library file already staged. Both matter: pre-deploy,
    # only the first sees SDL2; post-deploy, only the second sees transitively
    # bundled copies.
    found_libs = set()
    if hatari:
        # The bundled emulator may be the hrdb-main fork rather than upstream.
        # The licence is the same, but the source offer must name what was
        # actually built — the pinned fork commit, not the upstream tarball.
        # Detected by the same content probe PiST uses: the fork's listener
        # banner string, which upstream never contains.
        #
        # "GPL v2", not "or later": Hatari is mostly licensed v2-or-later, but
        # three of the files it compiles into every binary grant version 2
        # alone, so the combined work this project conveys is capped at v2 —
        # which is also why no readline travels with it (NOTICE, PLAN §10).
        if b"Remote Debug Listening on port" in hatari.read_bytes():
            components.append(("Hatari (hrdb-main fork: upstream 2.6.1 + remote-debug "
                               "listener)", "GPL v2 as conveyed (upstream v2 or later)",
                               ["GPL-2.0.txt"],
                               "source: https://github.com/tattlemuss/hatari — pinned "
                               "commit 21aa4cb76783eb1b141b917fa1976c9c01331d66"))
        else:
            components.append(("Hatari 2.6.1", "GPL v2 as conveyed (upstream v2 or later)",
                               ["GPL-2.0.txt"],
                               "source: https://www.hatari-emu.org/ (pinned tarball)"))
        if args.platform != "windows":
            found_libs.update(linked_libraries(hatari, args.platform))

    # The macOS app ships hatari_libretro.dylib, not a Hatari executable, and
    # the copy into the bundle happens after this script. --libretro-commit
    # is how that archive's source offer names the pist-libretro commit.
    libretro = find_binary(root, {"hatari_libretro.dylib", "hatari_libretro.so",
                                  "hatari_libretro.dll"})
    if libretro and not args.libretro_commit:
        fail("hatari_libretro is in the bundle but --libretro-commit was not given: "
             "the source offer has to name the commit it was built from")
    if args.libretro_commit:
        commit = args.libretro_commit.strip()
        if len(commit) != 40 or any(c not in "0123456789abcdef" for c in commit):
            fail("--libretro-commit must be the full lowercase sha of the pist-libretro commit")
        components.append(("Hatari (pist-libretro: in-process core hatari_libretro)",
                           "GPL v2 as conveyed (upstream v2 or later)",
                           ["GPL-2.0.txt"],
                           "source: https://github.com/idontwantyourspamthanks/hatari "
                           "— pinned commit " + commit))
        if libretro and args.platform != "windows":
            found_libs.update(linked_libraries(libretro, args.platform))
    found_libs.update(bundled_library_names(root))

    known_found = []
    for needles, (name, texts, licence) in KNOWN_LIBS.items():
        if any(needle in lib for needle in needles for lib in found_libs):
            known_found.append(name)
            components.append((f"{name} (bundled with the archive)", licence, texts, ""))
    all_needles = [needle for needles in KNOWN_LIBS for needle in needles]
    for lib in sorted(found_libs):
        if not any(needle in lib for needle in all_needles):
            print(f"collect-notices: note: {lib} present, no licence text required")

    # Copy the licence texts for the components actually present.
    for name, licence, texts, note in components:
        for text in texts:
            if (licenses / text).exists():
                continue
            src = NOTICES_DIR / text
            if not src.is_file():
                fail(f"licence text {text} is needed for {name} "
                     f"but missing from {NOTICES_DIR}")
            (licenses / text).write_bytes(src.read_bytes())

    lines = ["PiST — third-party components shipped in this archive",
             f"platform: {args.platform}",
             "",
             "Generated by packaging/collect-notices.py from the archive's",
             "contents: the binaries present, the libraries the bundled Hatari",
             "links, and every staged shared library. The authoritative",
             "descriptions live in NOTICE.",
             ""]
    for name, licence, texts, note in components:
        lines.append(f"{name}")
        lines.append(f"    licence: {licence}")
        for text in texts:
            lines.append(f"    licence text: licenses/{text}")
        if note:
            lines.append(f"    {note}")
        lines.append("")
    (doc / "THIRD-PARTY.txt").write_text("\n".join(lines))

    print(f"collect-notices: {doc} now carries notices for: "
          + ", ".join(c[0].split(" (")[0] for c in components))


if __name__ == "__main__":
    main()
