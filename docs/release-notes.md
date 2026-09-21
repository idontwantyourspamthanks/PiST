<!--
# PiST — release notes

This file is the body of every GitHub release: `.github/workflows/release.yml`
publishes it via `body_path`, so the text is reviewed in-tree and cannot drift
from what the workflow and README actually do. Edit it here, not in the
workflow. Maintainer guidance lives in this comment so it does not ship in the
published body.
-->

PiST — an IDE for Atari ST assembly development.

## What's new in 0.6.3

This release is about driving PiST from another program — an AI agent, a
script, or a test harness. The remote-control socket and the `pist-mcp` MCP
server grew a real feature set, and `pist-mcp` now ships in **every**
archive, including the Windows and macOS ones.

- **25 MCP tools.** Beyond build/run/step: open and read back the document
  the IDE is showing (`pist_open`, `pist_read`), the Problems pane with
  severities (`pist_problems`), the build's symbols (`pist_symbols`), memory
  reads and disassembly (`pist_readmem`, `pist_disasm`), and the profiler
  (`pist_profile_start` / `pist_profile_stop` / `pist_profile_results`).
- **Structured results.** State, problems, symbols, memory, disassembly and
  profile results answer as JSON — in `structuredContent` as well as text —
  so an agent consumes fields instead of parsing console output.
- **Breakpoints by label.** `pist_breakpoint` accepts a symbol name and
  resolves it to the first code line at or after its definition (a label on
  its own line has no code to break on), replying with the file:line and
  address it actually armed at.
- **Session token and zero configuration.** A listening IDE now requires a
  per-session token (`auth <token>` is the first line on a connection — on a
  shared machine, "localhost only" still means every local user) and
  publishes it with the port in an owner-only discovery file. `pist-mcp`
  finds both there, so against a locally started IDE it needs no flags at
  all. **Note for existing raw-protocol clients: send `auth` first — see the
  README.**
- **Push events.** Watchers are told when the debugger stops (with the real
  program counter) and resumes, as socket events and as MCP log
  notifications — no polling for a breakpoint hit.
- **Blocking semantics.** `run`, `build` and now `profile stop` answer only
  when the work is genuinely done, and block-typed queries return errors in
  the same framing, so a reader never hangs waiting for a terminator.
- **Correctness throughout the agent surface**, much of it found by driving
  the tools end-to-end: the shim no longer introduces itself with the wrong
  version, watch subscriptions can't hang against an unreachable IDE,
  breakpoints on non-code lines say so instead of promising a stop that
  never comes, `open` errors on a path that never opened instead of
  reporting success, and the shim's protocol suite runs in CI on all three
  platforms (the Linux and macOS archives also assert `pist-mcp` resolves
  its bundled Qt).

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
