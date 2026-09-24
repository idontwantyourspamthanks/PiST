<!--
# PiST — release notes

This file is the body of every GitHub release: `.github/workflows/release.yml`
publishes it via `body_path`, so the text is reviewed in-tree and cannot drift
from what the workflow and README actually do. Edit it here, not in the
workflow. Maintainer guidance lives in this comment so it does not ship in the
published body.

The "What's new" section covers only the version being released. Do not leave
older "What's new" sections in the file: the workflow publishes this whole
file, so they would appear on the new release. Previous notes stay in git
history. The download and install text below the changelog stays; it describes
the archives, not a past version.
-->

PiST — an IDE for Atari ST assembly development.

## What's new in 0.8.7

macOS: open the disk image and drag PiST onto the Applications folder. The
previous image also showed the ROM folder and a build-info file, and macOS
refused to launch the app — *PiST is damaged and can't be opened. You should
move it to the Bin* — because the bundle had been changed after it was signed.

### Fixed

- **The macOS disk image is a drag-install.** It contains PiST and a shortcut
  to Applications. The ROM, the assembler, the linker and the notices are
  inside the app, so dragging it across is the whole install. The `.tar.gz` is
  that same app.
- **The macOS app is signed after the bundle is finished.** Deploying Qt
  rewrites the binaries, and the assembler was copied in afterwards, which
  left a signature that did not match the app. That is the failure macOS
  reports as damaged.

Opening a downloaded copy may still ask for approval under *System Settings ▸
Privacy & Security*. The signature lets the system see an intact app;
notarization, which would skip that prompt, needs an Apple Developer ID and
this release does not have one.

### Worth knowing

An AppImage does not add itself to your application menu, and cannot: that
integration is done by AppImageLauncher or `appimaged`. If you want a menu
entry, install the `.deb` or the `.rpm` — that is what they are for, and both
carry the entry, the icons and the metadata a software centre reads.

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
transport uses), built unmodified from a checksum-pinned commit tarball (PiST
never patches it and runs it as a separate process). The binary is conveyed
under GPL version 2: three of the files Hatari compiles in grant version 2
alone, so the "or later" on the rest cannot lift the combined work — which is
also why the GPLv3 GNU Readline is not linked into it and travels in no
archive. The Windows build is the same fork compiled with MSYS2 ucrt64, with
its runtime DLLs beside the exe. The macOS archive does **not** include an
emulator: install one with `brew install hatari` (2.6.1 bottled), or point PiST
at one in Project Settings. Use 2.5 or later — 2.4.1 returns truncated debugger
responses that break source-line debugging, and Ubuntu 24.04 ships exactly
that, so a distribution package is often not recent enough. See
`share/doc/pist/NOTICE` for the full licence details.

### Linux

Nothing else to install: assemble, run and debug straight away.

The AppImage — `PiST-<version>-x86_64.AppImage` — is self-contained: Qt, the
assembler, the linker, the emulator and the ROM are all inside it. The shell
glob below is deliberate: these notes are published verbatim as the release
body, and a literal file name here drifted from the artifact a release actually
shipped once already.

```sh
chmod +x PiST-*-x86_64.AppImage
./PiST-*-x86_64.AppImage your-program.s
```

`.deb` and `.rpm` packages carry the same content for those who prefer the
package manager. They are also the route that integrates with the desktop: a
menu entry, the icons and the AppStream metadata a software centre reads the
name, author and licence from. Everything they bundle lives in a private
`/usr/lib/pist/`, with only `pist` and `pist-mcp` linked into `/usr/bin`, so
none of it can be picked up by other applications — and the bundled Hatari,
vasm and vlink cannot shadow a user's own. An AppImage does not add itself to
the application menu and cannot; that integration is AppImageLauncher's or
`appimaged`'s job, or install a package.

Everything is built on Ubuntu 22.04, so it needs glibc 2.35 or newer (Ubuntu
22.04+, Debian 12+, Fedora 36+, and anything more recent).

### Windows and macOS

Windows: the `.zip` or the `.msi` — both include the emulator, and both carry
the Visual C++ runtime beside the executable, so no redistributable install is
needed. macOS: open the `.dmg` and drag PiST to Applications, or unpack the
`.tar.gz`. Either one still needs `brew install hatari`.

### What works

Write, assemble, run and debug: source-line and conditional breakpoints,
stepping, step-over, registers, a memory viewer, and labelled disassembly, with
the editor following the program counter. Multi-file projects assemble
separately and link with the bundled vlink (add sources in Project Settings).
Projects keep their settings — include paths, defines, target CPU, machine,
ROM, RAM and disk images — in a small JSON file beside the source.

### Known limitations
- **CI exercises the emulator integration on Linux and macOS** (both build the
  pinned Hatari 2.6.1 and the hrdb-main fork from source and run the emulator
  suites against both transports). Windows builds and unit-tests only: the
  official Windows Hatari is a GUI-subsystem binary whose debugger never
  answers over pipes, so the bundled Windows emulator — the same fork, built
  with MSYS2 ucrt64 — is **experimental**; reports from real Windows machines
  are particularly welcome.
- **A user-installed *stock* Hatari on Windows** still cannot pause, change
  breakpoints while running, or swap disks at runtime — Hatari compiles its
  control channel only on POSIX systems. The bundled fork is unaffected (HRDB
  is TCP), and breakpoints and stepping work regardless. See `docs/FUTURE.md`
  for the upstream fix that would close this for stock Hatari.
- The interface is functional rather than polished.

Run `pist --diagnose` to see which assembler, linker, emulator and ROMs PiST
can find, and the paths it searched.
