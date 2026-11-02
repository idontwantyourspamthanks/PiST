# Future enhancements

Ideas that are understood well enough to describe but deliberately not built yet. Each entry
records *why* it isn't done, so a future decision has the context rather than just the idea.

---

## 1. A portable emulator control channel (upstream `--control-socket` on Windows)

**Status:** pause is delivered on both transports — natively via `hatari-debug b pc ! 0 :once`
over the control socket (entering the debugger properly; `hatari-stop` alone only halts the VBL
loop and wedges the session), and over HRDB's `break` on the fork, which works on Windows too.
Live breakpoint editing works on both as well. Live floppy insert/eject in a *stopped* session
uses debugger `setopt` (stdin / HRDB `console`) and is not socket-bound. Since 2026-09 the
Windows release archive bundles the fork itself (MSYS2 ucrt64 build), so a fresh Windows download
has all of this. What remains uncovered on Windows for *stock* Hatari: pause, live breakpoints,
and floppy swap or other `hatari-option` changes while the program is *running*. The upstream
patch below remains the right route for those.

### The gap

Hatari's control channel — the only way to command a *running* emulator — is compiled only under
`HAVE_UNIX_DOMAIN_SOCKETS`:

```c
/* src/options.c */
#if HAVE_UNIX_DOMAIN_SOCKETS
        { OPT_CONTROLSOCKET, NULL, "--control-socket", ... },
        { OPT_CMDFIFO, NULL, "--cmd-fifo", ... },
#endif
```

`src/includes/control.h` stubs the whole API out elsewhere. On Windows the option does not exist, so
passing it makes Hatari exit with `Unrecognized option` — which is why the IDE gates it on a
capability probe (see `docs/PLAN.md` §5 rule 12) rather than passing it unconditionally.

### What this costs on Windows today

| Feature | Linux | Windows |
|---|---|---|
| Build, run, break at entry | ✓ | ✓ |
| Registers, memory, disassembly, stepping, breakpoints | ✓ | ✓ |
| Break in on exceptions (`--debug-except`) | ✓ | ✓ |
| Pause a healthy running program | ✓ (`hatari-stop`) | ✗ stop and relaunch |
| Add/change breakpoints while running | ✓ | ✗ set them before launch |
| Swap disk images at runtime | ✓ | ✗ relaunch |

Everything except the last three works identically, because debugger commands travel over **stdin**
— the socket is starved whenever the debugger is stopped anyway (`docs/PLAN.md` §3.3).

### Proposed shape

Make `Control_SetSocket` accept `host:port` in addition to a filesystem path, so the same flag works
on every platform:

```c
/* src/control.c */
if (strchr(socketpath, ':') && !strchr(socketpath, '/')) {
        /* host:port -> TCP loopback */
} else {
        /* existing AF_UNIX path */
}
```

Roughly 30–50 lines: a `getaddrinfo`/`socket`/`connect` branch, an `accept` loop for the listening
side if needed, plus a `#else` for platforms without Unix sockets. HRDB already demonstrates that
loopback TCP works on Windows for this exact purpose, so the approach is proven rather than
speculative.

Two things it must get right:

- **Existing behaviour unchanged.** A path with a `/` keeps using `AF_UNIX`, so hconsole and the
  Python UI are unaffected.
- **Loopback binding needs a token.** A TCP port is reachable by any local process, unlike a
  `0600` socket file. A shared secret in a `hatari-token` line, or an ephemeral port written to a
  file the IDE reads, avoids turning "any local program can drive your emulator" into the default.

### Why it isn't done now

- It requires carrying a **Hatari fork**, with rebase duty against every upstream release. That
  duty is now carried deliberately: the bundled emulator IS the hatari-pist fork (the HRDB typed
  framing plus the media channel; pause and live
  breakpoints everywhere), with the native backend as the stock-Hatari fallback.
- It only pays off when someone actually needs Pause or runtime disk swap *on Windows*. The two
  break-ins that matter most for assembly work — program entry, and a program that faulted — are
  already available everywhere via `--parse` and `--debug-except`.

### How to start

Do it **upstream first**. `hatari-devel` is the submission path (the GitHub mirror explicitly says
not to open pull requests there). A patch that adds an optional `host:port` form to an existing
option, without changing existing behaviour, is a much easier sell than a new debug protocol — and
if it lands, `PiST` needs no fork at all. Only vendor a fork if upstream declines *and* the Windows
features become a real user request.

### Related, and already proven

The larger version of this idea is the **HRDB protocol** (`tattlemuss/hatari`, branch `hrdb-main`),
which adds a typed TCP protocol on port 56001 with explicit framing, hardware register reads, DSP
registers and breakpoints. It is richer than the control socket, but it is a bigger patch, it is not
upstream, and it has never been offered there. It stays the Phase 3 option behind `IDebugBackend`
(see `docs/PLAN.md` §3.2 and §6) — reach for it when the typed protocol or DSP support is actually
needed, not before.

---

## 2. Source-level debugging via DWARF

**Status:** deferred. **Trigger:** mixed C/asm projects, or vasm gaining a TOS-compatible DWARF path.

Line mapping currently comes from vasm's `-L` listing, which gives an exact line ↔ address map for
single-file assembly with no DWARF and no cross-GDB (`docs/PLAN.md` §4.2). That covers the intended
use.

`vasm -dwarf=3 -Felf` does emit real `.debug_line`, but:

- it fails outright with `-Ftos` (`error 3004: section attributes <r> not supported`), so it needs a
  link step through vlink for a `.PRG`;
- `-linedebug` is Amiga-hunk-only — unusable here;
- Hatari consumes no DWARF, so the IDE would parse it itself and relocate by the live text base.

Worth doing only when multi-module C/asm interop appears; vlink's map file is the lighter-weight
path for multi-module assembly.

---

## 3. Falcon DSP support

**Status:** deferred — Falcon is out of scope for now. Nothing in the IDE is DSP-aware: the
plan's Phase 2 DSP panels (registers, memory, breakpoints) were never built, so DSP commands
are reachable only by sending them verbatim through the remote-control `cmd`; there is no DSP
UI at all.

When Falcon support returns, the layering is already right: upstream's debugger drives the DSP
with its own command set (`dspreg`, `dspmemdump`, `dspbreak`, ...), which the native transport
carries unchanged, so panels are a UI job on top of `EmulatorHost`, not new plumbing. The
interactive debug console (deferred to after Phase 3) is the natural first surface for them.

*Source-level* DSP debugging remains out of scope entirely: the dgis GDB stub has no DSP support
at all, and GDB is m68k-only. Anything deeper means going through HRDB or writing
Falcon-specific tooling.

---

## 4. Emulator embedding on macOS

**Status:** not started. macOS cannot reparent a foreign process window — `WId` is a process-local
`NSView*` and AppKit has no cross-process reparenting — so the emulator runs as a detached window
there (`docs/PLAN.md` §3.2).

An in-process emulator core would fix that, and with `PiST` at GPL-2.0-or-later it is
**licence-compatible**: Hatari contains three GPL-2.0-only files, so a combined work must be
conveyed under GPLv2, which our licence permits. The cost is reimplementing the video, input and
audio plumbing that Hatari's SDL frontend already provides, plus the licence audit for each release.
The `libretro/hatari` core also returns `NULL`/`0` from `retro_get_memory_data`/`size`, so a RAM
viewer would need a core patch.

Do it for macOS polish, not as an architectural simplification.

---

## 5. Packaging and toolchain acquisition

**Status:** mostly delivered. Release artefacts carry the assembler, the linker (vlink, same
licence shape as vasm) and EmuTOS on every platform; per-platform installers exist (AppImage, deb
and rpm on Linux; dmg on macOS; MSI on Windows); and every archive bundles the hatari-pist
fork of Hatari (2.6.1-based) — the Windows one built with MSYS2 ucrt64, with its runtime DLLs
beside the exe, the macOS one carrying the fork's Homebrew dylib closure inside the app. The
first-run setup flow is delivered (`ui/SetupDialog`:
checksum-pinned vasm source build and an EmuTOS download, shown when the assembler or ROM is
missing). Hatari-on-macOS bundling, once the open item here, has since landed: the release
workflow copies the fork's Homebrew dylib closure (SDL2, libpng, SDL3) into the .app and rewires
every load path to `@rpath`.

Because `PiST` is free software, vasm's redistribution terms permit bundling it **unmodified** for
non-commercial use, and EmuTOS can ship as the default ROM — so a one-click install is legally
achievable. The plan (`docs/PLAN.md` §7) is to keep the repository free of non-free binaries and to
bundle everything a release artefact can carry. A source build bundles none of it.
Original TOS ROMs stay user-supplied, always.

The dependency-notice generation is delivered as `packaging/collect-notices.py`, which derives the
shipped licence texts from each archive's actual contents — including whether the bundled Hatari
links Capstone, the MinGW runtime DLLs a Windows emulator carries, and the MSVC runtime DLLs the
Windows bundle now ships beside pist.exe. The MSVC terms are the one text not yet captured:
Microsoft publishes them only as a web page and inside the installer, so `packaging/copyright`
states the redistribution basis and a `KNOWN_LIBS` entry waits on a copy in `packaging/notices/`.

---

## 6. Debug surface depth: editing, multiple panes, history, step-back, pause hint

**Status:** mostly delivered — register/memory editing, multiple memory panes, PC history and the
pause hint are in. A memory pane can mark the bytes it is showing and compare a later stop
with that mark, or with the previous stop's window, and Build → Compare With Last Build
previews how the linked image changed. Neither writes the machine, and neither records who
stored a byte. Step-back is still not in, and the analysis below records why it is not merely
unbuilt.

- **Register and memory editing.** Delivered. Registers and memory are editable while stopped — the
  typed backend intents `writeRegister` (`r <reg>=$<val>` on both backends) and `writeMemoryByte`
  (`w b $<addr> $<val>`; HRDB's wire form is `memset`) — gated on the stop state, and the views
  refresh after a write because Hatari prints nothing on success.
- **Multiple memory panes.** Delivered. `MainWindow::addMemoryPane()` adds panes, each routed by an
  integer tag so concurrent dumps reach the right pane rather than sharing one `memoryDumpReady`.
- **PC history and step-back.** The PC-history view is delivered (Hatari's `history` command,
  refreshed on each stop). Stock `stateload` is still not a step back: it restores by resetting
  the machine into a *running* state and completing the restore on the next CPU instruction
  (`m68k_go`, `UAE_Set_State_Restore` + `SPCFLAG_MODE_CHANGE`). Verified against 2.6.1: after a
  `stateload` the machine runs forward and never returns to a debugger prompt, which is
  incompatible with this IDE's prompt-framed debug transport. The fork's `reststop` does return
  to the prompt with that instruction unexecuted, and PiST uses it (§14). What is in the product
  is Save Stop, Restore Stop, and a short read-only list of those stops. Reverse Step — restore
  an earlier snapshot, then replay up to the instruction before the one on screen — has a
  mechanism on the fork and is not in PiST. `--replay-snapshot` writes one statesave during
  the recording, after that frame's keys. `reststop` restores it. `replaykeys` applies the
  recording's later keys, and a breakpoint on a later instruction returns to the prompt
  before that instruction executes. The proof stops on the replay fixture's spin, with the
  add that the key selects still unexecuted, and a different key stops on the other spin
  the same way. A statesave from the debugger prompt does too: the fixture's first
  instruction, vertical blank 0, supervisor set from that prompt, then the same
  replay. A stop during the recording itself is not part of that result. The per-instruction
  debugger can also stay on for a recording that never stops: one breakpoint that cannot match
  leaves it on, the debugger is never entered, and the trace matches the run with the flag clear.
- **Pause hint on the embedded display.** Delivered. The embedded panel shows a "Paused" badge when
  the debugger is stopped, so a frozen frame is not mistaken for a crash. The badge is placed in
  widget coordinates, so on a dock small enough for the fitted video to reach the widget's top-right
  corner it overlaps the picture rather than the letterbox: measured at 320x200, where it covers four
  of the 64 cells `tst_gui::mediaDisplayFeedsThePanel` samples, which is what made that check fail on
  CI while passing on a wider dock. Clamping the badge inside the letterbox, or scaling it with the
  panel, is the fix if the overlap ever bothers anyone.
- **Profiler icons.** Redrawn in the hand-drawn toolbar style (`paintProfileStart`,
  `paintProfileStop`, `paintProfileToCursor` in `src/ui/Icons.cpp`). The three share a rising
  sample count: Start is the accent play mark over that count, Stop is the count on a baseline,
  and To Cursor stands an I-beam beside it. The old set was a record disc, a chart stacked on
  the stop square, and a crosshair.

---

## 7. Panel aesthetics: tabs and movable panels

**Status:** delivered. Every dock is movable, floatable and
closable, docks nest and tab within an area, and the whole arrangement persists across runs
(QSettings saveState/restoreState, round-trip pinned by tst_gui::dockLayoutPersistsAcrossRestart).
The View menu lists every dock for show/hide, including a memory pane added later, and has a Reset
layout action (tst_gui::viewMenuListsEveryDock). Moving a panel is discoverable: right-click any
dock tab or title bar for a "Move to left / right / bottom / Float" menu, and drags track across
the embedded video (the foreign SDL window is made input-transparent mid-drag). A first run is the
Editing arrangement: debug docks stay hidden (they remain on the View menu, and showing them
still opens Registers on top of that tab group), the editor takes the centre, and the bottom
group is a short strip (tst_gui::factoryLayoutShowsRegistersAndTheEditor). When embedding is on, the
emulator dock sits above that group. Window geometry persists next to the dock state
(tst_gui::windowGeometryPersistsAcrossRestart), and a saved arrangement is left alone
(tst_gui::savedLayoutBeatsTheFactorySplit). View → Layout applies Editing, Debugging,
and Sprite arrangements and can restore the arrangement they replaced
(tst_gui::layoutPresetsHideDocksAndRestore).
---

## 8. Emulator window resize correctness

**Status:** fixed. The embedded display tracks the dock: on the reported video size it maps the
hidden SDL child and fits it to the container's *real* X11 size (`embeddedContainerSize`, not Qt's
geometry, which can disagree for a native dock), aspect-preserved and centred, with a settle timer
to re-fit after launch. Shrinking no longer clips and expanding no longer leaves the video small
and top-left.

---

## 9. LLM integration: chat sidebar, autocomplete, suggestions

**Status:** binned (2026-10-16), not deferred. The MCP route — `pist-mcp`
exposing the IDE to the terminal-side agent the user already runs — answers
more than an embedded chat would, for either side of it. The reason is
recorded in [glm-features.md §0](glm-features.md), which also carries the
features agreed to grow the verbs instead. The original entry is kept below
for the record of what the sidebar would have been. An agent-facing assistant built into the IDE: a chat
sidebar, inline autocomplete, and edit/refactor suggestions, against a configurable **endpoint and
API key** (so the user picks the provider/model rather than the IDE hard-coding one). Fits the
existing remote-control surface (the assistant can drive the IDE through it) and the project's
"hooks for agents" direction. Needs a settings page for endpoint/key/model, a sidebar panel, and a
clear line on what context is sent (open file, project, build errors) and what is not.

---

## 10. Scripted memory editing on pause (user scripts driven by the debugger)

**Status:** superseded by [glm-features.md §1](glm-features.md), agreed for
development 2026-10-16 as "scripted stop hooks" (with Lua recorded as the
language choice). The analysis below is what that entry builds on and stays
accurate. **Related:** the remote-control socket (already scripts memory edits from
outside; README §Remote control) and the LLM assistant (#9), which is a different agent-facing
surface.

### The idea

Let a user supply a script — in a language to be decided — that runs when the debugger pauses (at a
breakpoint, a watchpoint, or any stop), can inspect and edit memory and registers, and can choose
to continue. Uses: automated patching mid-session, fault injection, drive-by test harnesses that
poke inputs and assert on state, trainers/cheats, and dynamic analysis (log every write to a
region). It is the emulator-scripting model that FCEUX, VBA, Mesen and BizHawk standardised on Lua,
applied to this IDE's debug loop — the script lives *outside* the compiled program, so users extend
behaviour without rebuilding.

### What already exists

Most of the machinery is here, which is why this is a *glue* feature rather than new plumbing:

- Memory and register writes while stopped already work (`MainWindow::setMemoryByte` →
  `IDebugBackend::writeMemoryByte`, `setRegister` → `writeRegister` — typed intents each backend
  spells on its own wire), gated on the stop state, and the views refresh after a write.
- The trigger already exists: `EmulatorHost::stoppedChanged(true)` fires on every stop, and
  breakpoints/watchpoints are already resolved to addresses by `planBreakpoints()`.
- The remote-control socket already exposes `setmem`, `setreg`, `cmd` and `state`, so an *external*
  program can already do scripted memory edits while stopped today. What it lacks is a *push*: the
  protocol is request/response, so an external script must poll `state` to notice a stop, and there
  is no way to say "run this when we break."

### The two decisions

- **Language.** Lua is the natural fit: tiny, MIT-licensed, trivially embeddable, and the
  established standard for emulator scripting. The zero-third-party-dependency option is Qt's own
  `QJSEngine` (JavaScript), at the cost of pulling in the QtQml module. An embedded Python is
  heavier and complicates packaging. Record the choice here once made.
- **Trigger and API shape.** Run on *every* stop, or only on named/tagged breakpoints? The API
  should stay small: `readmem(addr, len)`, `writemem(addr, value)`, `readreg(name)`,
  `writereg(name, value)`, `continue()`, maybe `log(msg)`. A script runs to completion while the
  transport is stopped; its writes are ordinary debugger commands, which are legal then.

### Why it isn't done now

- It is a convenience/trigger layer over capability that mostly exists (the remote socket), so it
  blocks no one who is willing to drive the socket and poll.
- The language choice and the API surface deserve a decision, and an embedded-language dependency
  (if Lua) has to be carried on all three platforms.

### How to start

A minimal v0 needs no new *transport*: on `stoppedChanged(true)`, evaluate a configured script file
with an API object backed by the existing `setMemoryByte` / `setRegister` / `debugCommand` slots.
Vendor Lua if the embedded-language route is taken (small, and the BizHawk/Mesen precedent makes the
API shape familiar); use `QJSEngine` if zero added dependency is preferred. Either way, document the
script API in README and add an integration test that stops at a line and asserts a scripted write
landed.

---

## 11. tst_toolfetch on Windows: `vasmTarballBuildsAndInstalls` skipped

**Status:** skipped on Windows (`QSKIP` at the top of the test). **Blocks:** nothing — the
production path (tarball → make → install) is covered on Linux and macOS.

### What is known

The suite never passed on Windows. Instrumented CI runs (branch `diag/toolfetch-windows`,
since deleted) established:

1. `installToolBinaryIsFoundByDiscovery` hung 300 s in `QProcess::start` on the suite's
   shell-script fixture (a non-PE file named `vasmm68k_mot.exe`) — fixed by the PE-magic gate in
   `probeVersion` (`src/toolchain/Toolchain.cpp`).
2. The tarball fixture's `cp`/`chmod` recipe could not run under cmd.exe — fixed with
   `cmake -E copy` and a platform-correct output name.
3. Every other function in the suite demonstrably *passed* on the Windows runner (breadcrumbs
   proved completion; a `QVERIFY` failure returns early and would have skipped them).

`vasmTarballBuildsAndInstalls` still fails on the runner afterwards — in 3 s, with no output,
because ctest on Windows captures nothing from a test process (which is also why the failing
line is unknowable from here).

### How to start

Run `tst_toolfetch vasmTarballBuildsAndInstalls` on a Windows workstation (or a VM with the
runner's toolset: Chocolatey GNU make, Git's tar, hostedtoolcache python3). The failure is
immediate and the test prints normally outside ctest, so one local run shows the line.

---

## 12. Sprite editor: tilemaps, PI2/PI3, other machines

**Status:** sprite-sheet regions in progress; the rest deferred. The grid editor now has
LemonAndLime's ST-relevant editing: layers, onion-skin, animated preview, named phases,
flip/rotate/shift, and 8-way rotation bake. That lives in `src/image/{ImageDocument,Transform}.*`
and `src/ui/ImageEditor`.

**Sprite-sheet phases** — decided 2026-09 (superseding a first-cut regions design): a phase is an
animation *and* its home on a sheet. Each phase owns its frames outright and carries a cell size,
so one document mixes 32×32 characters with a 64×64 boss; it also carries sheet placement
(sheet index + x/y; frame k paints at [x + k·cellW, y]), and a document lists sheet targets
(path + size). `composeSheet()` composes a sheet from its placed phases and `sliceSheetCells()`
cuts an imported sheet into phase frames. The ImageEditor has a spritesheet mode: the composed
sheet with drag-to-move strips and numeric placement. A `.pi1` opened from a disk registers as a
sheet whose saves recompose and write back; Export Sprite-Safe keeps colour 0 as background so an
export re-imports losslessly (the failure `demo/test.pi1` shows).

**Sprite data export** — delivered: the bitplane `.dat` (`bitplaneLayout()` /
`exportBitplaneData()`) writes one phase's selected blocks in screen format for *every* frame of
that phase, so a frame and a pre-shift are strides rather than pointer tables, and
`exportScrollDemo()` writes a ready-to-assemble `-Ftos` scroller that `incbin`s that `.dat`,
animates the frames and scrolls them across an ST low-resolution screen until a key is pressed.
The dialog picks the phase, the blocks and the pre-shift count, and prints the file map the
assembler source's `equ`s come from.

**Declared sprite export** — a project can list `.pim` files on the build settings page. Each
row names a phase, the same block options as the export dialog, and one output file name.
Build, before assemble, hashes the document and those options. Unchanged leaves the previous
`.dat` and generated include alone. Changed writes both under the project's `build/` directory
through `files::write()`: the bytes, and an include (`incbin` plus `equ`s from
`bitplaneLayout`). The build's include path gains that directory for that build only.
A failed export keeps the previous outputs and fails the build. The project file stays at
version 2; an older PiST ignores the `assets` list.

Still not in PiST, on purpose:

- Tilemaps — regions name rectangles on a sheet; a tilemap (a grid of tile indices) is still a
  second product inside this one.
- Other machines (C64, Spectrum, CPC, Amiga, 8-bit). This IDE is for the ST.
- Degas PI2/PI3 (medium/high resolution). The editor is 16-colour low-res, which is the sprite path.
- Sound, packing, a byte map from a memory pane back to a phase, and uploading an asset into a
  running program. The declared list has to be how a program is built before any of those.

The `.pim` format is version 2 — phases own their frames — and v1 files are rejected outright
(decided 2026-09: no users yet, and the two models are structurally incompatible).

---

## 13. Git: hunks, history graph, conflicts

**Status:** the panel, the blame lane, branch create/switch, a read-only diff, and a flat
history are in. `GitService` runs `git` as a subprocess (argument lists only, no config
writes): `status --porcelain=v1 -z`, `commit -F -` of the checked paths, plain `pull` /
`push`, `switch` / `switch -c`, `diff` of the selected row, `log` / `show` for history, and
`blame -p -L` of the visible lines with the editor buffer on stdin so a dirty line reads
as uncommitted. The dock is `gitDock`, tabbed under Project files. Blame is off until
View → Git blame (`git/blame`).

The branch selector sits under the commit message. New… asks for a name and runs
`git switch -c`, which keeps local edits. Switching is plain `git switch`: if the
worktree would be overwritten, git refuses and the panel shows that text. There is no
`--discard-changes` path.

Selecting a row shows `git diff --cached` for a staged row, `git diff` for an unstaged
row, and `git diff --no-index` against `/dev/null` for an untracked file (Git for
Windows understands that path; Qt's `NUL` device does not). The text
is read-only. `--no-index` exits 1 when the sides differ; that patch is the result.

History is a flat list, newest first, capped at 200 commits. Selecting one runs
`git show` into the same view. There is no graph.

Not in this pass, on purpose:

- **Staging individual hunks.** The checkbox is the whole file. Hunks mean `git apply
  --cached` of a chosen diff, which is a different selection model than the current list.
- **History graph.** The list is the log. A graph is lane-drawing on top of it, and these
  repos are mostly linear.
- **Merge-conflict editor.** Conflict markers in the buffer, plus `git status` unmerged
  paths. Do not invent a mergetool config — that would be writing git config, which this
  IDE does not do.

---

## 14. hatari-pist experiments: replay, stopped restore, write trace, palette clock

**Status:** passed, on `hatari-pist`, pinned in PiST at `623747d`
(`623747d0d9decab28a9bf0badd09305bf8a6c033`). The debugger-on replay and
`replaykeys` are on the fork after that pin; the pinned build does not have them.
Each gate was a pass/fail trace and no dock.
Stock Hatari still has none of these commands. PiST probes by command name and treats
`Command '<name>' not found.` as a skip, not a failure. The host-side memory compare, the
last-build preview, and declared sprite export never waited on them.

What each gate proved, and where the proof stops:

1. **Replay one fixture twice.** Passed. `--replay-record` / `--replay` record one program's
   inputs and a per-vertical-blank trace (format `HPISTREPLAY 1`) and play it back. The
   matching run agreed for all 40 frames. Changing one key made the traces diverge at frame
   16. The same fixture also matches with the per-instruction debugger left on: one breakpoint
   at `pc = $1` never fires, the file says `debugger yes`, and all 40 frames are identical to
   the run with the flag clear. A change in that flag during the run is still refused. A stop
   in the middle of the recording is not proven. From a snapshot taken at vertical blank 10,
   `reststop` then `replaykeys` applies the later keys and a breakpoint stops on the spin the
   key selects, with that add still unexecuted. A different key stops on the other spin the
   same way. A `statesave` from the debugger prompt, on the fixture's first instruction
   at vertical blank 0, restores through `reststop` and reaches that same unexecuted add.
   The recording is otherwise narrow: `--tos none`,
   no floppy. TOS and the FDC are not proven.
   A Lua file that boots Hatari and hashes RAM is not this result, and stop hooks are not
   record/replay.
2. **Restore and stay stopped.** Passed. `reststop` restores one `statesave` file and returns
   to the debugger prompt with that instruction unexecuted. `stateload` is unchanged (§6): it
   resumes the machine and never returns to a prompt. Restoring that stop and executing the
   same steps again matched, including one vertical blank. PiST drives this from a real HRDB
   session: Run ▸ Save Stop writes the stopped machine, Run ▸ Restore Stop runs `reststop`,
   and Run ▸ Saved Stops shows an earlier stop's registers and analysis mark without calling
   the debugger. That session stopped on the entry breakpoint and on steps. A restore from a
   stop that arrived via pause was not part of the proof.
3. **Targeted write events.** Passed. `writetrace` records writes in one range as they happen
   (format `HPISTWRITE 1`). A CPU store carries its PC. A blitter store does not. A debugger
   poke is `unknown`, not whichever instruction the CPU is stopped on. The ring keeps the
   oldest events and counts the rest as `dropped`. It does not stop the CPU. The host-time
   comparison around 64 writes was clock noise, not a measured overhead. PiST's memory pane
   Trace button arms the window it is showing, and the Writes list reads the file at each
   later stop. A CPU row opens that instruction's source line when the program map knows
   it. The list does not move the machine.
4. **Palette writes against a scanline clock.** Passed. `paltrace` records each palette write
   where the hardware writes it (format `HPISTPAL 1`), with the vertical blank, the line, and
   the position on that line. The same two writes landed on different lines at 50 Hz (512
   cycles per line) and 60 Hz (508), because the position is the shifter's own line clock and
   not a frame-cycle count divided by 512. PiST's Palette dock Trace button runs `paltrace
   on`, and the list reads the file at each later stop. A row opens that instruction's
   source line when the program map knows it. The list does not move the machine, and it
   is not a scanline inspector. The profiler overlay is not that stream. The media channel's frames
   are RGB, and `info video` does not carry the palette.

Still not built:

- **Reverse Step** restores an earlier snapshot and replays to just before the instruction on
  screen. The fork can do that for the replay fixture: a snapshot taken during the recording,
  `reststop`, then `replaykeys`, then a breakpoint on the later instruction. The snapshot
  can be the one `--replay-snapshot` writes while the recording runs, or a `statesave`
  from the debugger prompt at the fixture's first instruction. PiST does not call
  it. The saved-stop list is still the navigation in the product. A stop during the recording
  itself is not part of the proof. Breakpoint seeking, branches, and a recording browser wait
  with a PiST command for this.
- **Reads, and where a value came from,** are the rest of memory history. The Writes list
  is the forward-only view of gate 3. The Palette list is the forward-only view of gate 4.
  Interrupts, video-base changes, the blitter, and a known-good frame are not that list.
- **Applying a patch** stays closed until the preview has been wrong in public at least once,
  and then only for one same-length range, while stopped, with a read-back, and only when the
  preview said the section size matched. Gate 2 has passed, so a snapshot to roll back to is
  available. Until a preview has been wrong in public, the recovery path is still restart.
  Same length does not prove absolute references survived.
