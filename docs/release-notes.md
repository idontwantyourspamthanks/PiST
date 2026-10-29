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

## What's new in 0.10.6

### Fixed

- **macOS: a failing emulator no longer hangs the app.** SDL's default
  assertion handler on macOS is a modal dialog; inside a background emulator
  nobody could see it, so a probe or session just blocked forever. Capability
  probes now run with no SDL window and `SDL_ASSERT=abort`, sessions set
  `SDL_ASSERT=abort`, and media sessions (which have no SDL window of their
  own) use the dummy video driver — a failure is reported, not waited on.
- **A crashed emulator is reported with the right number.** On Unix the
  message said "killed by signal -11" instead of signal 11; on Windows the
  exit code is a crash code, not a signal, and now says so.
- **Stop-hook scripts could swallow an error budget.** `pcall`/`xpcall`
  and Lua's `longjmp` paths no longer escape the runaway-script budget.

### Improved

- **MainWindow is split** into focused translation units (chrome, documents,
  image, project, build/run, debug, reference) — no behaviour change, faster
  compiles, easier navigation.
- **Hook and demo trust tightened.** Demo provenance is recorded and
  source-checkout demos are trusted; refused hook settings are preserved
  rather than silently dropped.

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
