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

## What's new in 0.9.1

0.9.1 is the first stable release of the emulator-panel line. It carries what
the 0.9.0-alpha pre-release introduced — the emulator now runs inside PiST's
own panel: Hatari runs windowless and streams its screen, sound, keyboard and
mouse over a local channel, PiST paints the frames, plays the audio, and takes
input with a VirtualBox-style grab (click the emulator panel to capture the
keyboard and mouse, press F12 to release). The old embedding (X11 window
reparenting on Linux, window adoption on Windows) is gone. All three archives
bundle the fork that provides the channel — the macOS app carrying the fork's
Homebrew dylib closure inside the bundle — so the panel works everywhere out
of the box; only a *source* build supplies its own Hatari, and a stock one has
no media channel, so there the emulator opens in its own window. On top of
that, this release adds:

### Changed

- **A document is a file.** The untitled buffer is gone. File ▸ New File opens
  a dialog — name, directory and one of four templates (blank, GEMDOS hello,
  keyboard/input, image include) — validates as you type, and creates the file
  on disk before opening it; the sidebar's New File… runs the same dialog
  rooted where you right-clicked. With no document open, the central area is a
  welcome view: recent files, the demo projects on a first run, Open/New, and
  toolchain and ROM readiness with a Setup… button. File ▸ Close File (Ctrl+W)
  and Close All reach the last tab. A file renamed on disk is followed; a file
  deleted on disk either closes an unmodified tab or offers Close tab / Save a
  copy… / Cancel, instead of being discovered on the next save. Build and Run
  can no longer silently assemble a different buffer than the one you are
  looking at.
- **The caret is yours until you hand it over.** Run used to move the caret,
  the view and the focus to the program's first line, and every later stop did
  the same — including a breakpoint firing while you were typing. The session
  now marks where the machine is and leaves the caret alone; the editor follows
  the program counter only while you have given it the caret — via Run ▸ Follow
  Execution, a click on the session chip, or the ▶ the gutter paints on the
  marker line. Any other caret movement releases it.
- **Debug and Release builds have their own assembler arguments.** Debug is
  `-quiet -devpac -opt-x+` — Devpac compatibility (which disables
  optimisations) plus the DRI/GST symbol table the debugger reads; Release is
  `-quiet -devpac -nosym`, the same build with the table stripped. Both are
  editable in Project Settings, each with a reset. The Build toolbar button is
  a split button (Build / Build Release) and the Build menu gains Build
  Release. A project that only carried the old single argument list migrates it
  into the Debug list, so a source that assembled under plain Motorola syntax
  now assembles in Devpac compatibility mode; the project file schema is
  version 2.
- **The remote-control protocol can produce a release build**, with the
  `buildrelease` verb (also `pist_build_release` in the MCP shim).

### Fixed

- **The IDE no longer spins at 100% CPU before showing a window under a
  comma-decimal locale** (is_IS, de_DE, …). The menu logo's SVG outline was
  parsed with a locale-following number reader, so a token such as `.001`
  consumed nothing and the parser never advanced. Numbers are now scanned by
  hand and converted in the C locale, and a malformed path cannot loop either.
- **A file deletion is reported once, not twice.** Qt raises the deletion
  twice on Windows, which would have shown the rescue prompt twice for one
  file going away.

### Worth knowing

An AppImage does not add itself to your application menu, and cannot: that
integration is done by AppImageLauncher or `appimaged`. If you want a menu
entry, install the `.deb` or the `.rpm` — that is what they are for, and both
carry the entry, the icons and the metadata a software centre reads.

Every archive contains PiST, the `vasmm68k_mot` assembler, the `vlink` linker,
an EmuTOS ROM and the bundled emulator, so all three run and debug with
nothing else installed.

**Assembler and linker:** vasm and vlink are not free software. They are
redistributed unmodified and for non-commercial use, which their licences
permit; see `share/doc/pist/NOTICE`. PiST never patches them.

**ROM:** EmuTOS 1.4 (GPLv2), a free TOS replacement. Original Atari TOS images
are copyrighted and are not included — supply your own in Project Settings if
you need one.

**Emulator:** every archive bundles the **hatari-pist** fork of Hatari
(upstream 2.6.1 plus the HRDB remote debugger and the media channel that drives
PiST's embedded display), built from a checksum-pinned commit tarball and run as
a separate process. The binary is conveyed under GPL version 2: three of the
files Hatari compiles in grant version 2 alone, so the "or later" on the rest
cannot lift the combined work — which is also why the GPLv3 GNU Readline is not
linked into it and travels in no archive. The Windows build is the same fork
compiled with MSYS2 ucrt64, with its runtime DLLs beside the exe; the macOS app
carries the fork's Homebrew dylib closure (SDL2, libpng, SDL3 and its X11
neighbours) inside the bundle, rewired to `@rpath`. A *source* build supplies
its own emulator: install Hatari 2.5 or later with your package manager
(`brew install hatari` on macOS, 2.6.1 bottled), or point PiST at one in
Project Settings — but a stock Hatari has no media channel, so the emulator
opens in its own window rather than PiST's panel. Use 2.5 or later — 2.4.1
returns truncated debugger responses that break source-line debugging, and
Ubuntu 24.04 ships exactly that, so a distribution package is often not recent
enough. See `share/doc/pist/NOTICE` for the full licence details.

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
`.tar.gz` — the app carries the emulator and its libraries, so there is nothing
else to install.

### What works

Write, assemble, run and debug: source-line and conditional breakpoints,
stepping, step-over, registers, a memory viewer, and labelled disassembly, with
the execution point marked in the editor and your caret left where you put it.
Multi-file projects assemble separately and link with the bundled vlink (add
sources in Project Settings).
Projects keep their settings — include paths, defines, target CPU, machine,
ROM, RAM and disk images — in a small JSON file beside the source.

The emulator's display, sound and input live in PiST's own panel (with the
bundled emulator): click the panel to grab the keyboard and mouse, F12 to
release.

### Known limitations
- **CI exercises the emulator integration on Linux and macOS** (both build the
  hatari-pist fork from source and run the emulator suites against both
  transports). Windows builds and unit-tests only: the official Windows Hatari
  is a GUI-subsystem binary whose debugger never answers over pipes, so the
  bundled Windows emulator — the same fork, built with MSYS2 ucrt64 — is
  **experimental**; reports from real Windows machines are particularly
  welcome.
- **A user-installed *stock* Hatari** (a source build supplies its own; every
  archive bundles the fork) has no media channel, so PiST shows the emulator in
  a separate window instead of the panel; on Windows it also cannot pause,
  change breakpoints while running, or swap disks at runtime, because Hatari
  compiles its control channel only on POSIX systems. The bundled fork is
  unaffected, and breakpoints and stepping work regardless.
- The interface is functional rather than polished.

Run `pist --diagnose` to see which assembler, linker, emulator and ROMs PiST
can find, and the paths it searched.
