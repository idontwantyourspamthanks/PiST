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
  duty is now carried deliberately: the bundled emulator IS the hrdb-main fork (pause and live
  breakpoints everywhere, typed framing), with the native backend as the stock-Hatari fallback.
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

**Status:** on `feature/libretro-macos`. The plan is [docs/agents/mac.md](agents/mac.md): an
in-process core forked from the pinned Hatari, loaded as `hatari_libretro.dylib` from
`Contents/Frameworks`. Launch selects it on macOS when that file is present and the project
names no Hatari of its own. The release job builds the dylib; a person does not compile on a
Mac for each release. macOS cannot reparent a foreign process window — `WId` is a process-local
`NSView*` and AppKit has no cross-process reparenting — so a subprocess stays a detached window
(`docs/PLAN.md` §3.2). Linux and Windows keep that subprocess.

The core is licence-compatible: Hatari contains three GPL-2.0-only files, so a combined work is
conveyed under GPLv2, which GPL-2.0-or-later permits. The published `libretro/hatari` tree is not
the one we build — it still compiles the pre-2.6 CPU, and `retro_get_memory_data` returns NULL —
so the fork adds its own RAM and debugger exports. Frames, the entry stop, step, resume,
registers, breakpoints, keyboard, mouse, audio and the session's monitor are in
place. Each macOS archive's `THIRD-PARTY.txt` names the pist-libretro commit the
dylib was built from.

---

## 5. Packaging and toolchain acquisition

**Status:** mostly delivered. Release artefacts carry the assembler, the linker (vlink, same
licence shape as vasm) and EmuTOS on every platform; per-platform installers exist (AppImage, deb
and rpm on Linux; dmg on macOS; MSI on Windows); and the Linux AppImage and the Windows archive
both bundle the hrdb-main fork of Hatari (2.6.1-based) — the Windows one built with MSYS2 ucrt64,
with its runtime DLLs beside the exe. The first-run setup flow is delivered (`ui/SetupDialog`:
checksum-pinned vasm source build and EmuTOS download, shown when the assembler or ROM is
missing). Hatari on macOS is the in-process core in `docs/agents/mac.md`, which links `libm` and
`libz` only. Until that dylib ships, `brew install hatari` (2.6.1) is how a Mac session runs.

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
pause hint are in; only step-back is not, and the analysis below records why it is not merely
unbuilt.

- **Register and memory editing.** Delivered. Registers and memory are editable while stopped — the
  typed backend intents `writeRegister` (`r <reg>=$<val>` on both backends) and `writeMemoryByte`
  (`w b $<addr> $<val>`; HRDB's wire form is `memset`) — gated on the stop state, and the views
  refresh after a write because Hatari prints nothing on success.
- **Multiple memory panes.** Delivered. `MainWindow::addMemoryPane()` adds panes, each routed by an
  integer tag so concurrent dumps reach the right pane rather than sharing one `memoryDumpReady`.
- **PC history and step-back.** The PC-history view is delivered (Hatari's `history` command,
  refreshed on each stop). True step-back was attempted and is documented here because it is *not*
  simply unbuilt: the only mechanism Hatari offers, `statesave`/`stateload`, restores by resetting
  the machine into a *running* state and completing the restore on the next CPU instruction
  (`m68k_go`, `UAE_Set_State_Restore` + `SPCFLAG_MODE_CHANGE`). Verified against 2.6.1: after a
  `stateload` the machine runs forward and never returns to a debugger prompt, which is
  fundamentally incompatible with this IDE's prompt-framed debug transport — the whole debugger is
  built on "stopped at a prompt". So a clean "stop at the previous state" cannot be built on
  stateload. Real step-back would need either upstream Hatari support for a non-destructive
  restore, or a record/replay mechanism (relaunch + step forward N times), the latter being slow
  and state-lossy.
- **Pause hint on the embedded display.** Delivered. The embedded panel shows a "Paused" badge when
  the debugger is stopped, so a frozen frame is not mistaken for a crash.
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

**Status:** roadmap, deliberately unstarted. An agent-facing assistant built into the IDE: a chat
sidebar, inline autocomplete, and edit/refactor suggestions, against a configurable **endpoint and
API key** (so the user picks the provider/model rather than the IDE hard-coding one). Fits the
existing remote-control surface (the assistant can drive the IDE through it) and the project's
"hooks for agents" direction. Needs a settings page for endpoint/key/model, a sidebar panel, and a
clear line on what context is sent (open file, project, build errors) and what is not.

---

## 10. Scripted memory editing on pause (user scripts driven by the debugger)

**Status:** not started. **Related:** the remote-control socket (already scripts memory edits from
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

Still not in PiST, on purpose:

- Tilemaps — regions name rectangles on a sheet; a tilemap (a grid of tile indices) is still a
  second product inside this one.
- Other machines (C64, Spectrum, CPC, Amiga, 8-bit). This IDE is for the ST.
- Degas PI2/PI3 (medium/high resolution). The editor is 16-colour low-res, which is the sprite path.
- Auto-export on Build / an asset list in `.pistproject`. Regions live in the `.pim` so a sheet is
  self-contained; export is explicit; the assembler consumes whatever `.s` or `.bin` the user wrote.

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
