<!--
# PiST — release notes

This file is the body of every GitHub release: `.github/workflows/release.yml`
publishes it via `body_path`, so the text is reviewed in-tree and cannot drift
from what the workflow and README actually do. Edit it here, not in the
workflow. Maintainer guidance lives in this comment so it does not ship in the
published body.
-->

PiST — an IDE for Atari ST assembly development.

Every archive contains PiST, the `vasmm68k_mot` assembler, the `vlink` linker,
and an EmuTOS ROM. The Linux AppImage and the Windows archive additionally
contain the Hatari emulator, so on Linux and Windows they run and debug with
nothing else installed.

**Assembler and linker:** vasm and vlink are not free software. They are
redistributed unmodified and for non-commercial use, which their licences
permit; see `share/doc/pist/NOTICE`. PiST never patches them.

**ROM:** EmuTOS 1.4 (GPLv2), a free TOS replacement. Original Atari TOS images
are copyrighted and are not included — supply your own in Project Settings if
you need one.

**Emulator:** the Linux AppImage and the Windows archive bundle the hrdb-main
fork of Hatari (upstream 2.6.1 plus the remote-debug listener PiST's HRDB
transport uses), GPL-2.0-or-later, built unmodified from a checksum-pinned
commit tarball (PiST never patches it and runs it as a separate process). The
Windows build is the same fork compiled with MSYS2 ucrt64, with its runtime
DLLs beside the exe. The macOS archive does **not** include an emulator:
install one with `brew install hatari` (2.6.1 bottled), or point PiST at one in
Project Settings. Use 2.5 or later — 2.4.1 returns truncated debugger responses
that break source-line debugging, and Ubuntu 24.04 ships exactly that, so a
distribution package is often not recent enough. See `share/doc/pist/NOTICE`
for the full licence details, including the GNU Readline that the bundled Linux
build links.

### Linux

Nothing else to install: assemble, run and debug straight away.

`pist-*-linux-x86_64.AppImage` is self-contained — Qt, the assembler, the
linker, the emulator and the ROM are all inside it:

```sh
chmod +x pist-*-linux-x86_64.AppImage
./pist-*-linux-x86_64.AppImage your-program.s
```

`.deb` and `.rpm` packages carry the same content for those who prefer the
package manager. Everything is built on Ubuntu 22.04, so it needs glibc 2.35 or
newer (Ubuntu 22.04+, Debian 12+, Fedora 36+, and anything more recent).

### Windows and macOS

Windows: the `.zip` or the `.msi` — both include the emulator. macOS: the
`.dmg` or the `.tar.gz`, plus `brew install hatari`.

### What works

Write, assemble, run and debug: source-line and conditional breakpoints,
stepping, step-over, registers, a memory viewer, and labelled disassembly, with
the editor following the program counter. Multi-file projects assemble
separately and link with the bundled vlink (add sources in Project Settings).
Projects keep their settings — include paths, defines, target CPU, machine,
ROM, RAM and disk images — in a small JSON file beside the source.

### Known limitations

- **CI exercises the emulator integration on all three platforms** — Linux and
  macOS build the pinned Hatari 2.6.1 and the hrdb-main fork from source;
  Windows runs the native transport against the official stock 2.6.1 binary and
  HRDB against an MSYS2 fork build. Reports from real machines remain welcome
  all the same.
- **A user-installed *stock* Hatari on Windows** still cannot pause, change
  breakpoints while running, or swap disks at runtime — Hatari compiles its
  control channel only on POSIX systems. The bundled fork is unaffected (HRDB
  is TCP), and breakpoints and stepping work regardless. See `docs/FUTURE.md`
  for the upstream fix that would close this for stock Hatari.
- The interface is functional rather than polished.

Run `pist --diagnose` to see which assembler, linker, emulator and ROMs PiST
can find, and the paths it searched.
