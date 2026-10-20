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

## What's new in 0.10.4

A bugfix release for packaged installs: the demo project that opens on a first
run could not build.

### Fixed

- **The demo now builds from a packaged install.** Packages keep the demos under
  the read-only install prefix (`/usr/lib/pist/share/pist/demo` in the `.deb`),
  while the build writes its listing and program beside the source — so the
  first build failed with `fatal error 14: could not open .../hello.lst for
  output`. The shipped demo tree is now copied to
  `~/.local/share/PiST/demos` on first use, and that writable copy is what the
  welcome view and the Demo action open. Edits to the copy are kept across
  runs.
- **A session already pointed at the installed demo moves to the copy.** A
  read-only demo source stored as the last open file or in Open Recent is
  translated when it is reopened, so upgrading does not leave the failing path
  in place.
- **The copied demo's stop hook is trusted without a prompt.** Only the project
  files PiST copied are recorded; a project later placed in that directory is
  not trusted just by being there.

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
