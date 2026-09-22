<!--
# PiST — release notes

This file is the body of every GitHub release: `.github/workflows/release.yml`
publishes it via `body_path`, so the text is reviewed in-tree and cannot drift
from what the workflow and README actually do. Edit it here, not in the
workflow. Maintainer guidance lives in this comment so it does not ship in the
published body.
-->

PiST — an IDE for Atari ST assembly development.

## What's new in 0.8.0

Git lives in the IDE. You can see what changed, commit it, switch branch, and
look back — without leaving the file you were assembling.

- **A Git panel**, tabbed with Project files. Staged, changed and untracked
  files sit in a list; check the ones that belong in the next commit, write a
  message, and commit. Hooks run. Pull and Push are the ordinary commands, and
  git's own text is what you see when they fail.
- **Switch and create branches** under the commit message. The selector
  switches; New… asks for a name. A switch that would overwrite local edits is
  refused — git's error, not a discard prompt.
- **A diff of the file you pick.** Select a row and the patch appears under the
  list: staged rows show what is already in the index, other rows the worktree,
  untracked files as new. Deselect and the pane goes away.
- **History.** Newest first, up to 200 commits. Select one to see its message
  and patch in that same view.
- **Blame in the gutter.** View → Git blame puts the author beside the line
  numbers. A dirty line reads as uncommitted, and a click in that lane does not
  toggle a breakpoint.
- **Sprite editor and profiler, packed.** Onion-skin sits with the frame
  buttons, play sits above a 96-pixel preview that no longer grows when you
  hit play, and phase placement only appears in spritesheet mode. The layers
  list and the profiler icons no longer stretch to fill leftover space.

## What's new in 0.7.2

The window spends its space on the work in front of you.

- **A first run is for writing.** Debug docks stay on the View menu until a session needs them. The editor takes the centre, project files sit on the left, and Problems and the console are a short strip along the bottom.
- **Three layouts.** View → Layout offers Editing, Debugging and Sprite, and Restore my layout puts back the arrangement they replaced. The first time a session stops, and the first time an image opens, the status bar offers the matching layout. The embed checkbox still owns the emulator dock.
- **The View menu lists every dock**, including a memory pane opened later, and Reset layout returns to that first-run arrangement.
- **Status bar.** Session (Not running / Running / Stopped, with the source line), a build result that stays put, and the caret. Hatari's capability probe moves to the session chip's tooltip.
- **The instruction under the caret.** A one-line strip under the editor names the mnemonic or the TOS call. Clicking it opens the reference. While stopped, flag chips and a one-line register strip sit under that; the full registers table is still there.
- **Go to line** is Ctrl+G. Return in the editor copies the indent of the line you just left.
- **F8 toggles a breakpoint.** The PiST keys are otherwise unchanged: F5 run, F9 continue, F10 step into, F11 step over. Settings → Appearance can switch to the common IDE scheme (F9 toggles a breakpoint, F10 steps over, F11 steps into, and F5 continues while stopped).
- **Navigation opens the file.** A problem, a breakpoint or a symbol in another source opens that file. A disassembly row opens its source line when the program map knows it, and does nothing when it does not. A PC-history address opens the source line, or memory.
- **Problems mark warnings.** Errors and warnings both carry a coloured square, and a warning tints the message amber.
- **Hardware, in one line.** The video subject leads with the screen address, refresh rate and overscan Hatari's `info video` actually prints, and each chip name has a tooltip.
- **Build has its own menu.** Build and the F4 diagnostic walk live there; Run is run, step and breakpoints. Set up tools and ROMs… is a button at the bottom of Project Settings.
- **File → New File**, with the image exports gathered under File → Export. An empty floppy is one line ("A: no disk"), and the project pane is titled with the directory. Export to a floppy image stays on the hard-drive's context menu.
- **Sprite editor.** Palette swatches are numbered, frames run in a filmstrip under the canvas, phases are a combo, and flip, rotate and shift live in a Transform menu.
- **Appearance.** The dialog shows a sample line in the editor face you picked, and the splitter handles are wide enough to grab. Comments, gutter numerals and zero bytes in the dark theme clear ordinary reading contrast.
- **Profiler icons**, redrawn so Start, Stop and To cursor read as one tool: a play mark over the sample count, the count on a baseline, and that count beside the caret.

## What's new in 0.7.1

The OS-call reference completes its round trip, and the profiler becomes
usable:

- **Insert a call's binding from the reference dock.** Pick a GEMDOS, BIOS
  or XBIOS call and "Insert binding into editor" (button or right-click)
  drops its assembler binding in above the cursor — argument pushes with the
  parameter names as placeholders, the trap, and the exact stack cleanup.
  The odd shapes are right too: Pexec's fixed layout, Mshrink/Frename's
  reserved word, Dbmsg's literal 5.
- **The profiler, discoverable at last.** Its controls live in the profiler
  dock as icon buttons (they used to hide, greyed, in the Run menu), the
  empty dock teaches the flow, and every click — success or refusal — says
  what happened in the dock's own status line.
- **Profile to cursor line.** The whole ritual in one gesture: cursor where
  measuring should end, and PiST arms a one-shot there, collects, and shows
  the results when the run stops.
- **Results worth reading.** Hot spots as a routines tree (nearest label,
  hot lines underneath) in counts *and* cycles, with milliseconds and frames
  in the status line, a noise floor for the long tail, and the operating
  system's share as a TOS/ROM row instead of silently discarded.
- The profile actions follow the mode: Start is disabled while collecting,
  Stop when nothing collects, and every disabled button's tooltip says why.

## What's new in 0.7.0

The instruction reference dock learned the operating system. It was always
good at telling you what `addq.l` does; now it tells you what your *program*
is doing:

- **TOS system-call reference.** Cursor on a `trap #1`, `trap #13` or
  `trap #14` line — or on any of the pushes feeding one — and the dock names
  the GEMDOS, BIOS or XBIOS call being made: 111 entries covering the whole
  TOS 1.x/2.x set, each with its C prototype, what d0 returns, the TOS
  version it needs, and the stack layout.
- **It reads the idiom, not just the word.** The dock resolves the function
  number from the push above the trap (`move.w #9,-(sp)`, `#$0b`,
  `#Cconws`, `clr.w`) and shows what the call is being made *with* —
  "Calling with: #msg". A register-pushed number still shows the generic
  TRAP entry.
- Everything joins the one searchable list, CPU families first, so the
  filter covers instructions and calls alike.

The demo sources get a correction out of the bargain: function 7 is Crawcin,
not Cnecin as their comments claimed — exactly the mix-up this feature
exists to catch.

## What's new in 0.6.3.2

The rest of the agent surface, in one point release:

- **Tool annotations.** Read-only tools declare `readOnlyHint`, and the ones
  that end or rewrite live state (`pist_stop`, `pist_setreg`, `pist_setmem`)
  declare `destructiveHint` — an MCP client can now ask your confirmation
  for exactly the right calls.
- **MCP resources and prompts.** `pist://console`, `pist://state` and
  `pist://document` as readable resources (`pist://state` subscribable, with
  updates pushed on every stop/resume), and two ready-made prompts:
  `diagnose-build` and `find-hot-loop`.
- **The document surface, completed.** `pist_tabs` lists every open document
  (path, modified, current) and `pist_save` saves the current one.
- **Build failures answer with their diagnostics.** A failed `pist_build`
  now includes the Problems pane in `structuredContent` — file, line,
  severity, message — instead of a bare `error build failed`.
- Also: 27 tools in total, the `symbols` verb restored to the README's
  protocol table, and the `quit` decision recorded (it stays off the tool
  list deliberately).

## What's new in 0.6.3.1

A point release for one security-relevant fix: a connection refused by the
remote-control token check could have bytes it sent during the
(asynchronous) disconnect window executed without a token. The client now
stays refused until the socket is gone. Everything below from 0.6.3 is
unchanged.

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
