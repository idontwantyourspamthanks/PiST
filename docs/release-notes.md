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

## What's new in 0.8.2

A maintenance release, and a big one: no new features, 149 files touched —
19,781 insertions against 5,060 deletions. A full code-quality review of the IDE
was run to ground and every finding it produced is fixed here — the correctness
bugs you could hit, the structure that let them happen, and the tidying that
makes the next change cheap.

### Fixed

- **Saving a source no longer rewrites it.** The editor records the file's own
  line-ending style and encoding on load and writes them back: a CRLF file stays
  CRLF, a Latin-1 file stays Latin-1 (promoted to UTF-8 only if you type a
  character Latin-1 cannot hold), and a byte-order mark survives. Loading and
  saving an unedited file is byte-identical, so a source copied out of a floppy
  image stops changing shape on save. A save also creates missing parent
  directories, which is what extracting a file to a new folder needs.
- **Git acts on the project you opened.** The repository root is discovered with
  `git rev-parse --show-toplevel` from the project directory instead of being
  assumed to be PiST's own working directory — the stray `index.lock` that could
  turn up in a PiST checkout was that bug.
- **A commit is the index, and PiST never rewrites it.** The ticked rows are
  staged and the index is then committed with no pathspec, so a path already
  staged but left unchecked — or a conflicted path — is refused by name (include
  it, unstage it, or resolve it) rather than swept in or committed with its
  conflict markers still in it.
- **A damaged floppy image fails instead of quietly truncating.** A FAT12 cluster
  chain that loops back on itself, links to a cluster outside the image, or ends
  short of the size its directory entry declares is now reported. A short read
  used to be indistinguishable from a short file, so writing the image back
  carried the truncation into the replacement and renamed that over your own —
  destroying the orphaned clusters a repair would need. Listing a disk stays
  best-effort: a damaged folder is shown unexpanded rather than blanking the
  panel.
- **The debugger can no longer answer a command with the previous command's
  output.** On the native transport a timed-out command leaves a prompt owed;
  that prompt is now swallowed *and* its response tail drained before the next
  command goes out, and a fresh session starts from clean framing state instead
  of inheriting the last one's counters. On HRDB the queue is held while a reply
  is owed, so a reply that never arrives cannot shift every later answer by one.
  Both backends also point their stderr drain at the stderr channel — it was
  inspecting the always-empty stdout buffer and burning its full 20 ms timeout on
  every response, which was latency you could feel while stepping.
- **Disassembly of a hex-looking mnemonic is whole again.** `dbcc`, `dbf` and
  `abcd` were absorbed into the byte column of a disassembly line, so the pane
  rendered the instruction without its mnemonic and step-over had no mnemonic to
  read.
- **Two same-named objects in a multi-file project no longer share one address.**
  `dir1/util.o` and `dir2/util.o` were a single module to the line map, which put
  both at one address and could arm a breakpoint in the wrong file. An ambiguous
  base name now resolves nothing and says so in the build log, naming both paths.
- **Find's hit counter admits its cap:** past 2000 matches it reads
  "N of more than 2000" instead of counting on.
- **`info video` reports only what Hatari printed** — the hardware summary no
  longer invents a resolution or a palette the transcript never mentioned.

### Project files

`.pistproject` is read strictly, and every refusal names the entry at fault:

- a wrong-typed entry fails the load (`'build.includePaths' must be a list of
  strings`) instead of silently becoming an empty `-I` on the assembler command
  line;
- relative include paths and additional sources resolve against the project
  file's own directory, so a project builds the same wherever PiST was started
  from;
- a file written by a *newer* PiST is refused rather than loaded and saved back
  with the fields this build does not understand dropped from it;
- saving goes through a temporary file renamed into place, so a failed write —
  full disk, crash, power cut — leaves the project you had instead of a truncated
  one that the next load reports as invalid.

The format version is still 1: project files from earlier releases load as
before.

### Remote control

The localhost protocol changed shape, so a `pist` and a `pist-mcp` from different
releases refuse each other instead of half-working:

- PiST publishes a protocol version in its discovery file, and the shim checks it
  before dialling — a mismatch is refused naming both versions, rather than
  connecting and then failing verb by verb once an agent tries something the
  other half does not have;
- a block reply carries an explicit status line (`ok`, or `error` for a failed
  query) ahead of its body;
- a body line that begins with `.` is sent dot-stuffed with one extra `.`, so a
  body that legitimately contains a lone `.` cannot end the block early.

The vocabulary is now one table that drives `help`, the block framing and the
shim's tool list together, so the four places a verb used to be spelled cannot
disagree. A script written against 0.8.1 needs the small reader update shown in
the README's remote-control section.

### Under the hood

- `pist_core` is split into module libraries — model, image, build, debug, emu,
  project, toolchain, git, editor, ui, control, mcp — with an acyclic dependency
  graph. Modules that need to cross a boundary speak through `Host` interfaces
  and `ControlHost` instead of reaching into a sibling.
- The logic came out of the widgets: debug session, session launch, profiler,
  bitplane export, sheet slicing, animation preview, the breakpoint/watchpoint
  model and floppy transfer are units of their own (`MainWindow.cpp` 4,737 →
  4,414 lines, `ImageEditor.cpp` 2,593 → 2,295).
- The editor's colours are an injected theme value rather than a global, so an
  editor can be exercised without a palette behind it.
- A `tr()` sweep brought 102 user-facing strings into translation, and the dead
  code found along the way went out.
- `-Wall -Wextra` (`/W4` on MSVC) is now the build baseline and the tree is
  warning-free; CMake presets cover asan and ubsan builds.
- Every fix above landed with its regression test written against the failure
  first. All 15 suites are green, and the emulator framing cases run without an
  emulator present.

### Licence

- **The bundled Hatari is built without GNU Readline, and no archive ships it.**
  Three of the files Hatari compiles into every binary grant GPL version 2 alone,
  so the emulator PiST conveys is capped at GPLv2 and a GPLv3 library cannot join
  it. Nothing is lost: without readline the debugger takes its `fgets` fallback
  and writes the prompt to stderr instead of stdout, and PiST frames a command on
  the `> ` prompt on either stream. `NOTICE` and `docs/PLAN.md` §10 carry the
  audit of those three files and the include chains that reach them.

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
