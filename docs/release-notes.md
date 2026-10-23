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

## What's new in 0.10.5

### New

- **The entry stop explains itself, and resumes with one click.** Run stops at
  the program's first instruction so you can step through it from the start.
  The emulator panel now says so: a **Program loaded** card appears over the
  last frame instead of leaving a still screen that reads as a hang. Its
  **Continue** button names the current shortcut, and the card is the button —
  clicking it resumes. The button pulses briefly when the card first appears,
  then settles.
- **Every stop names the way out.** The small badge at later stops reads
  "Paused — *key* continues", following the shortcut scheme in use.
- **The hint is there even without the display panel.** A session driven by a
  stock Hatari, or one whose panel is hidden, gets the same message in the
  status bar: the program is loaded and paused, and the Continue key runs it.
- **An entry stop that beats the first frame still explains itself.** The card
  is drawn even when no media frame has arrived yet.

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
- The interface is functional, and still a first design pass rather than a finished one.

Run `pist --diagnose` to see which assembler, linker, emulator and ROMs PiST
can find, and the paths it searched.
