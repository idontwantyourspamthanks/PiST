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

## What's new in 0.10.3

A hardening release. It closes a sandbox escape in stop hooks, makes a runaway
hook unable to freeze the IDE, and fixes the errors an independent review of
0.9.1–0.10.2 found in search, crash reporting and the floppy pane.

### Security

- **A stop hook can no longer compile more code or load a precompiled chunk.**
  The engine loaded scripts with `luaL_loadbuffer`, which accepts the binary form
  the interpreter does not verify, and left `load`, `loadfile` and `dofile`
  reachable — so a script could compile one at run time. Chunks now load
  text-only, the three loaders refuse by name, and `string.dump` is gone. This
  matters because a project's hook is code from wherever the project came from.
- **Opening a project now asks before running its hook.** A `.pistproject` can
  name a script anywhere on disk; the first time a project with a hook is
  opened, PiST says what a hook can do (read and write memory and registers) and
  waits for an answer. The answer is remembered per project file. Projects that
  ship with PiST — the demo — are trusted by provenance and are not asked about.

### Fixed

- **A hook that loops around `pcall` can no longer freeze the IDE.** The time
  limit was an ordinary Lua error, so
  `while true do pcall(function() while true do end end) end` caught it and ran
  forever — on the window's own thread. The deadline now carries a token a
  script cannot forge, and the `pcall` it sees re-raises it instead of returning
  it, so no nesting keeps a runaway alive.
- **No more undefined behaviour when a hook raises.** Lua's C build unwinds with
  `longjmp`, which skipped the destructors of the API functions' string objects
  on every script error. Errors now cross into the interpreter after those
  objects are gone.
- **A hook's write is no longer mistaken for someone else's command.** The wait
  after a scripted write took *any* completion, so a toolbar Step or a
  remote-control command arriving mid-hook counted as the answer. It now waits
  for its own command.
- **Find in Files gives the editor's column in CRLF files** — which is most Atari
  ST sources. Every hit after a `\r\n` line was one column too far right.
- **Find in Files no longer stalls before its first result.** The directory tree
  was read in one blocking call; it is now listed within the same time budget as
  the scan.
- **Renaming on a floppy refuses the name it would really write.** The clash
  check compared the typed spelling, so a rename to a name sharing its 8.3 field
  with an existing entry was silently numbered instead of refused.
- **A hook script inside a project is saved relative to the project**, so the
  demo and any committed project no longer carry the saving machine's
  `/home/...` path.
- **A crash report that failed to upload is retried**, as the code always said it
  would be: a parked report is now picked up on the next launch, and the
  unwinder is warmed before the signal handler needs it.

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
