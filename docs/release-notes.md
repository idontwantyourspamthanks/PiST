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

## What's new in 0.8.3

A correction release, published shortly after 0.8.2 and superseding it: CI found
two defects in what 0.8.2 shipped, both are fixed here, and the release workflow
now runs the test suite before it packages anything, so a red build cannot be
published again. 0.8.2 keeps its own page and changelog — the large maintenance
release this one corrects.

**If you downloaded 0.8.2, update:** its archives carry the first bug below.

### Fixed

- **A failed project save no longer destroys the project.** Saving writes through
  a temporary file that is renamed into place, so a write that cannot reach the
  disk — a full disk, a quota, an I/O error — was meant to leave the file you
  had. In the Qt these archives bundle (6.8.1) it did not: `QSaveFile::commit()`
  does not notice the flush inside it failing, so it renamed the empty temporary
  file over the project and reported success. The save now flushes explicitly and
  checks the device error before committing, and cancels the temporary file when
  either is wrong. Qt 6.10 checks this inside `commit()`; 6.8.1, which every
  archive ships, does not — so the bug was in the published binaries, not only in
  a source build against an older Qt.
- **A timed-out debugger command's tail can no longer leak into the next
  command's response.** When the transport gives up on a command after 10 s it
  swallows the prompt that command still owes it, and drains stderr first so the
  dead command's last output is not attributed to whatever runs next. That drain
  waited 20 ms — enough for a tail already sitting in the pipe, not for one a
  slower machine had not written yet, which is how it surfaced on macOS. The
  owed-prompt path now waits 200 ms, and nothing user-visible is held up by it:
  the command was reported failed ten seconds earlier. The completion path every
  step, continue and register read goes through keeps the short window.

### Tests, packaging and docs

- **The release workflow tests before it packages.** Linux runs the full suite
  against the emulator it is about to bundle; macOS and Windows run the unit
  suites (a Windows emulator session never reaches the entry stop, so the
  integration suites stay with CI's Windows leg). Tagging a commit whose CI was
  red — or, as happened here, still running — used to publish anyway.
- Three Windows-only test defects fixed. The fake `git` the discovery test plants
  recorded its working directory with a separator its own checker parses
  differently: cmd.exe needs `^|` and was writing `&`, so every invocation looked
  like it ran outside the project. The gutter-repaint check asserted a glyph on a
  runner with no fonts installed at all, and now asserts the font-independent
  breakpoint dot there, skipping the glyph check with the reason named. And a
  Latin-1 file name in the floppy test was spelled as a raw `0xE9` byte inside
  `QStringLiteral`, whose meaning in that position is compiler-defined; it is now
  `QString::fromLatin1`, the same decode the product applies to the field.
- The generated third-party notices state the GPLv2 cap on the bundled emulator
  instead of upstream's "or later", and the install text names the AppImage a
  release actually ships (`PiST-x86_64.AppImage`).

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

`PiST-x86_64.AppImage` is self-contained — Qt, the assembler, the
linker, the emulator and the ROM are all inside it:

```sh
chmod +x PiST-x86_64.AppImage
./PiST-x86_64.AppImage your-program.s
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
