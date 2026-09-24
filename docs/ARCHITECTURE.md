# PiST codebase guide

A map of the code as it exists today, for someone about to change it. It describes *what is
there and how it connects*, not *why it was designed that way* — that rationale, with the
supporting research, is [PLAN.md](PLAN.md). Work that is deliberately not done yet, with the
reasoning, is [FUTURE.md](FUTURE.md).

PiST is a Qt 6 (Widgets) IDE for writing 68000 assembly for the Atari ST. You write assembly in
the editor, build it with `vasmm68k_mot`, run it in Hatari, and debug it — breakpoints, stepping,
registers, memory, disassembly, watchpoints, a stack view and hardware registers — with the editor
following the program counter.

## The one rule that shapes everything

**The assembler and emulator are driven as subprocesses, through command-line arguments only.**
They are never patched, never linked in, and their configuration files are never rewritten. This
is a correctness and licensing requirement, not a preference (see PLAN.md §3.2, §7):

- Hatari's licence makes linking it a combined work; keeping it a subprocess keeps the GPL
  boundary explicit, and it is the only approach that works on all three platforms today.
- vasm is not free software — it permits *unmodified* redistribution, which is exactly why the IDE
  never patches it.
- A user-supplied (or newer) toolchain just works, because nothing is compiled against it.

Everything else in this document is a consequence of that rule. The debug transport is convoluted
*because* we drive a stock emulator we do not control; the line mapping uses vasm's listing
*because* we cannot rely on DWARF reaching a TOS executable.

## Map

| Path | Contents |
|---|---|
| `src/main.cpp` | Entry point. Pins `QT_QPA_PLATFORM=xcb` when `DISPLAY` is set (needed for X11 embedding), parses `--diagnose` / `--control-port` / the positional source file, creates the single `MainWindow` and the optional `RemoteControl` listener. |
| `src/ui/` | `MainWindow` (the application shell: document tabs via a central `QTabWidget`, with `m_editor` as the current text editor and `m_image` as the current sprite editor), the five session seams (`DebugSessionController`, `BreakpointWatchpointModel`, `ProfilerController`, `SessionLauncher`, `RemoteStateAdapter` — each wired through its own `Host` collaborator struct, see the tree below), every debug panel — including `ProfilerView` (per-line hot lines, rendered from an already-*attributed* profile), `SymbolsView` (labels/equates browser), `InstructionRefView` (the 68000 reference that follows the cursor) and `ConsoleInput` (the debug console's history/completion line edit) — the X11 display embedding (`EmulatorDisplayWidget`, `EmbedX11`), `Appearance` (dark-first Fusion theme with a GEM-green accent, editor font family/size, toolbar icons, and the `EditorTheme` the editors are handed; preferences in QSettings), `Icons.cpp` (the hand-drawn themed glyphs), `SettingsDialog` (the project-settings editor, whose machine and ROM controls are linked), `UiText.h` (`firstLine()` — the one-line status-bar form of a multi-line refusal) and the sprite editor (`ImageEditor`, `ImageCanvas`, `SheetCanvas` — the composed sheet's view: strips, phase drag/move, the staging gutter and raw-sheet slicing — `BitplaneExportController` (the repeatable `.dat` export recipe), `SheetSlicing` (the slice-phase dialog), `AnimationPreviewWidget` (the frame player) and `BitplaneExportDialog`). |
| `src/editor/` | `CodeEditor` (the editor widget: line-number gutter with breakpoint dots, execution-line highlight, error markers and profiler heat, an optional git blame lane to the left of that gutter, find/replace bar), `AsmHighlighter` (m68k Motorola syntax, painted with the `EditorTheme` it is handed), `EditorTheme` (the monospace font, the effective darkness and exactly the colours the editor and its highlighter read — the value `ui/` builds and hands over, so `editor/` includes nothing from `ui/`), `InstrRef` (the instruction-reference table and lookup behind the dock), `OsCallRef`/`OsCallScan` (the GEMDOS/BIOS/XBIOS table and the trap-sequence scanner behind the same dock), `OsCallBinding` (the canonical insertable call sequence — argument pushes, function number, trap, stack cleanup — and the stack byte count beside it) and `IncludeNav` (include-target/label resolution behind Ctrl+click). |
| `src/git/` | `GitService` drives `git` as a subprocess (argument lists only: status, blame, commit, pull, push, `switch` / `switch -c`, `diff`, `log`, `show`) and `GitParse` reads porcelain. The panel is `ui/GitPanel`. |
| `src/image/` | Sprite document and ST graphics: `ImageDocument` (v2 `.pim` JSON: phases own their frames and cell size and carry sprite-sheet placement; `StFormats::composeSheet()`/`sliceSheetCells()` move between placed phases and sheet images), `Palette` (STfm 512 / STe 4096 cubes and colour words — the 4-bit-per-channel machine words, with `rgbFromStfmWord`/`stfmColourWord` for the 3-bit words the file formats actually store), `Tools` (brush/line/rect/fill), `Transform` (flip, rotate, region cut/stamp/move, onion/phase math), `StFormats` (PI1, NEO, IFF, STOS MBK, PNG, assembler include, plus `spriteSafeDocument()` — the palette shift that keeps colour 0 as background so an export re-imports losslessly — and `bitplaneLayout()`/`exportBitplaneData()` for the sprite editor's `.dat` — the selected blocks of one phase, every frame of it, laid out so a frame and a pre-shift are strides — plus `exportScrollDemo()`, which writes an `incbin`ing scroller that animates and scrolls them). |
| `src/build/` | `BuildService` (plans and runs vasm/vlink steps), `Diagnostic` (a parsed warning/error), the line maps — `LineMap`, `LinkMap`, `ProgramLineMap` — `SymbolTable` (labels/equates from a vasm listing, feeding the Symbols dock and console completion) and `FloppyImage` (AUTO-folder FAT12 writer for pre-1.04 TOS, plus `.st` / `.msa` listing, export, and in-place edits — `readFileRaw` extracts a file's bytes and `updateImage` rewrites an existing image with entries added and removed) with `FloppyTransfer` (`floppy::Transfer` — the one implementation of the three disk/host/between-disk transfers the browser runs, widget-free behind a `Host` interface). |
| `src/emu/` | `IDebugBackend` (`DebugBackend.h`, the transport contract) with two implementations: `EmulatorHost` (stock Hatari over stdin/prompt framing) and `HrdbBackend` (the hrdb-main fork over TCP 56001); `EmbedSocket` (the `QLocalServer` on Hatari's `--control-socket` — the `hatari-debug` channel and the embedded display's video-size reports; invariant 14), `MachineState` (the per-stop snapshot the typed reads fill and `stateUpdated` carries), `HatariTextParse` (their shared `d` parser: the `$`-optional address, a byte column that is a run of single-space-separated hex words — so a hex-only mnemonic like `dbf` is never absorbed into it, and a ten-byte instruction's `23+` cut token stays with the bytes — and the instruction text), `SessionConfig` (one session's argv), `HatariProbe` (capability detection; content-scans the binary on Windows, where Hatari's info options print to a fresh console, not the pipe), `ProfileData` (the `profile save` parser behind the Profiler dock), `AttributedProfile` (the attribution of a parsed profile to routines and source lines, with the ROM/TOS row — MAJ-44), `TosRom` (ROM discovery + version), `MemoryDump` (memdump parsing), `Paths` (ROM/session directories). |
| `src/debug/` | `Breakpoint` (the file:line model and the pure `planBreakpoints()` that turns lines into `b pc = $addr` commands) and `Watchpoint` (a change-tracking conditional breakpoint). |
| `src/model/` | `Machine` — the machine vocabulary shared by `emu/` and `project/`: the `Machine` enum, its display/CLI name mapping and TOS-acceptance. ROM discovery stays in `emu/`; this leaf exists so `project/` no longer includes `emu/` (MIN-56). |
| `src/support/` | `FileWrite` — the one rule for replacing a file the user already has: a temporary in the destination's own directory, an explicit flush and a device-error check, and only then the rename. Every save path calls `files::write()` — the editor's source save, `image/` (`.pim` and the ST exports), `project/`, `build/FloppyImage::saveRaw`, and the UI's floppy extraction and bitplane export — and none of them opens a destination with `Truncate` (invariant 16). |
| `src/control/` | `ControlHost` (`ControlHost.h`, the interface the IDE implements for the protocol: the verbs it serves and the events its verbs wait on) and `RemoteControl` — the localhost TCP line protocol that drives the IDE from a script or an AI agent, with `watch`/`unwatch` event subscriptions — plus `mcp/`, the `pist-mcp` stdio MCP shim that bridges MCP clients to it. Nothing here includes or names a `src/ui/` type: the control layer depends on `ControlHost`, and `MainWindow` is one of its implementers (MIN-86). |
| `src/project/` | `ProjectSettings` — the per-project `.pistproject` JSON file (build + emulator settings). |
| `src/toolchain/` | `Toolchain` — discovery of vasm, vlink and Hatari (explicit path → beside the exe → bundled tools dir → `PATH`), plus install hints; `ToolFetch` — the checksum-pinned fetch/build/install the setup dialog drives (`ui/SetupDialog`, shown at startup when the assembler or ROM is missing). |
| `tests/` | Parser and image unit tests (always run) and the offscreen GUI/emulator integration tests (gated on tools being present). |
| `demo/` | `hello.s` — a tiny program opened by `run.sh` (and by `run.sh` only; the first run itself is the setup dialog), plus `hello.pistproject`, `demo.pim` — a sample sprite set (two phases, v2 format) — and `spritedemo/`. |

### Per-file detail

- **`ui/MainWindow.{h,cpp}`** — the central controller and by far the largest file. It owns the
  document tabs (`CodeEditor` and `ImageEditor`), every dock, the `BuildService`, the debug backend
  (`m_host`, an `IDebugBackend` — `EmulatorHost` or `HrdbBackend`) and the `ProgramLineMap`; creates
  the actions/menus/toolbar/status bar; persists the layout and the embedded-display preference;
  owns the app-level event filter (dock move menu + drag pass-through); and wires the panels to the
  backend. Almost everything is routed through here.
  Five seams hold the state that used to be spread across it, and each is an ordinary child of the
  window rather than a `friend` of it (MAJ-41; the friend edges are gone — MIN-89):
  `DebugSessionController` (`m_session`: the session's lifecycle flags, the entry-stop attach, the
  one `armBreakpoints` gate and the `stepOut` state — it also completes a step-out from the stack
  dump that answers it), `BreakpointWatchpointModel` (`m_bpModel`: the breakpoint and watchpoint
  lists and *every* edit to them), `ProfilerController` (`m_profiling`: profiling as a mode, plus
  the last `AttributedProfile`), `SessionLauncher` (`m_launcher`: ROM selection, the floppy-boot
  fallback, the transport switch and the `SessionConfig`) and `RemoteStateAdapter` (`m_remoteState`:
  the read-only JSON the remote verbs report).
  The window constructs each one and hands it a **`Host`** — the narrow set of operations that seam
  performs on the window (the debug backend, one line on the console, the panel rows, the project
  settings, the current editor, the tab list, the program map, …) as plain callbacks
  (`DebugSessionController::Host` and its four siblings). A seam therefore compiles against its own
  collaborator list instead of the window's private surface, and that list is the whole of what it
  can reach; the aggregates are built in `MainWindow`'s constructor, so what each seam may do is
  readable in one place. The seams are what wire the panels to the backend, and they reach the
  widgets through that same Host: `BreakpointWatchpointModel::refreshMarkers()` pushes the models to
  every editor's gutter and to `BreakpointPanel`, `ProfilerController::showResults()` hands one
  attributed profile to `ProfilerView`, the editor's gutter heat and the remote `profile results`
  verb, and `DebugSessionController::armBreakpoints()` carries the arming plan to the backend and
  its resolved addresses back to the panel. Breakpoint and watchpoint *edits* therefore no longer
  originate here (they go through the model and the gate);
  the PC→source-line follow itself is still routed here (the stop handler maps the PC through the
  `ProgramLineMap` and calls `setCurrentExecutionLine()` / `gotoLine()` on the target editor).
  `openPath()` dispatches `.pim` / ST still-image files to `addImageTab()`; Build/Run use
  `buildSourcePath()` so an image tab being focused does not strand the assembler.
  `MainWindow` is also the control layer's `control::ControlHost` (`control/ControlHost.h`): the
  remote-control verbs are its own methods, so `RemoteControl` drives the IDE through an interface
  `src/control/` owns instead of including this header (MIN-86).
- **`ui/EmulatorDisplayWidget.{h,cpp}`** — the `WA_NativeWindow` container the emulator's display
  lands in. On X11 its `winId()` is handed to Hatari, which reparents itself into it; on Windows it
  adopts the emulator's own window by process id (`attachEmulatorProcess()` starts a poll that ends
  when that window appears, or says on the panel that it never did). Tracks the video size,
  aspect-fits it inside the dock, and paints a "Paused" badge when stopped.
- **`ui/EmbedX11.{h,cpp}`** — free X11 functions over Qt's `QX11Application` native interface:
  `mapEmbeddedWindowChildren`, `embeddedContainerSize`, `resizeEmbeddedChild`,
  `setEmbeddedChildrenInputTransparent`, `captureWindowImage`. All no-ops without
  `PIST_HAVE_X11`/`PIST_HAVE_XEXT`.
- **`ui/EmbedWin32.{h,cpp}`** — the Windows half: `findEmulatorWindow` (by process id, nothing else
  names the emulator's window), `embedForeignWindow` / `releaseForeignWindow` (`SetParent`, with the
  window styles set in the order MSDN requires of it), `windowHandleValid`, `windowClientSize`,
  `moveEmbeddedChild`. All no-ops off Windows.
- **Debug panels** (`ui/`): `RegistersView` (editable D0–D7/A0–A7, PC, SR, USP/ISP, flags),
  `DisassemblyView` (current PC highlighted), `MemoryView` (hex, byte editing, address navigation),
  `StackView` (longs at SP, return-address annotation), `HardwareView` (`info <subject>` output —
  a parsed header above the transcript, showing the screen base as eight hex digits, the refresh
  rate and overscan, and nothing the report does not state),
  `PcHistoryView` (`history` output), `BreakpointPanel` (breakpoint + watchpoint table),
  `FileBrowser` (hard-drive project tree plus Disk A/B floppy listings, with a browser-wide clipboard and drag & drop that copy and move entries within and between the panes — onto a disk this rewrites the image via `floppy::updateImage`; double-clicking a text or still-image entry on a disk extracts it to the session directory — text as an editor tab, a still image as a sprite-sheet document whose phases slice the sheet — and saving that tab writes the recomposed sheet back through `MainWindow::writeBackFloppyDoc`). Each is a thin view;
  `MainWindow` feeds it parsed state and routes its edit/activation signals to debugger writes.
- **`editor/CodeEditor.{h,cpp}`** — `QPlainTextEdit` subclass with a `LineNumberArea` gutter.
  Emits `gutterClicked` (breakpoint toggle) and `gutterContextMenuRequested`. View → Git blame
  opens a lane to the left of the numbers and blames only the visible lines; a click in that lane
  does not toggle a breakpoint. It paints the execution line, error underlines and the find hits
  via extra selections (on-screen hits only). Its find bar (a child widget along
  the bottom edge, with the viewport margin to match) searches as you type, wraps, counts the hits
  (capped at 2000 — the label reads "N of more than 2000"), and replaces one or all — replace-all back to front inside one edit block, so it undoes in one
  step. The window's Search menu drives it, and the shortcuts are bound there.
  It is themed, not theme-aware: `CodeEditor(EditorTheme, QWidget*)` takes the font, the darkness and
  the colours it paints with, and `setTheme()` reapplies them when the preferences change — the
  window hands it `appearance::editorTheme()`, so the widget never includes `src/ui/` (MIN-86).
  **`editor/EditorTheme.{h}`** is that value, and **`editor/AsmHighlighter`** takes it too, via the
  same constructor/`setTheme()` pair, instead of reading the appearance itself.
- **`image/`** — palette cubes, `.pim` load/save, paint geometry, layers/phases, and ST file codecs. No widgets.
  `exportBitplaneData()` writes the sprite editor's `.dat`: a raw blob of ST screen-format blocks
  (palette, sprite, masked sprite, pre-shifted copies) for every frame of a phase, whose byte map
  `bitplaneLayout()` produces and the encoder walks, so the offsets the export dialog prints are the
  offsets the file has. `exportScrollDemo()` turns the same options into a `-Ftos` scroller that
  `incbin`s that `.dat`, animates the frames and moves them across the screen.
  **`ui/ImageEditor.{h,cpp}`** is the document tab; **`ui/ImageCanvas`** paints the grid with onion-skin, and owns the selection rectangle: the select tool drags a marquee, dragging inside it moves the pixels (live cut+patch preview, committed through `ImageEditor::moveSelection` as one undoable layer edit), and copy/cut/paste/delete/deselect run off an internal palette-index clipboard. **`ui/SheetCanvas`** shows the composed sheet (strips, phase dragging, the staging gutter, raw-sheet slicing), with **`ui/SheetSlicing`** the slice-phase dialog, **`ui/BitplaneExportController`** the per-document recipe a re-export repeats and **`ui/AnimationPreviewWidget`** the frame player.
- **`build/FloppyTransfer.{h,cpp}`** — `floppy::Transfer`: the three disk transfers the file browser
  offers (host files onto a disk, entries off a disk, entries between disks) written **once** over a
  source/destination pair instead of three times. It is widget-free: the two questions it cannot
  answer itself — which image a drive holds, and whether rebuilding one is acceptable — come in
  through its `Host` interface (finding MAJ-43).
- **`build/BuildService.{h,cpp}`** — `planSteps()` chooses a single-file `-Ftos` build or a
  per-module `-Fvobj` build plus a `vlink -b ataritos` link; `runNextStep()` streams output and
  parses vasm/linker diagnostics. Emits `finished(success, diagnostics)`.
- **`build/LineMap.{h,cpp}`** — parses one vasm `-L` listing into section:offset ↔ source-line
  entries and the section extents. **`LinkMap.{h,cpp}`** parses a vlink `-M` map.
  **`ProgramLineMap.{h,cpp}`** aggregates one `LineMap` per module plus the `LinkMap`, and resolves
  line ↔ address once `setLiveBases()` supplies the runtime section addresses.
- **`emu/EmulatorHost.{h,cpp}`** — the heart of the debug side: owns the Hatari `QProcess`, both
  debug channels, the command queue, the prompt/settle framing, the stderr/stdout parsers, and
  `writeBootstrapScript()` (the entry-stop generator). See the transport section below.
- **`emu/SessionConfig.{h,cpp}`** — `toArgv()` is the single place Hatari's command line is
  composed. If you add an option, add it here, gated on `HatariCapabilities` when it is not universal.
- **`emu/HatariProbe.{h,cpp}`** — `probeHatari()` fills `HatariCapabilities` by running Hatari and
  matching **option names**, never version numbers (see Invariants).
- **The session seams** (`ui/`, MAJ-41) — `DebugSessionController` (the session's lifecycle flags,
  the entry-stop attach, the single arming gate — `canReArmBreakpoints` decides when an edit re-arms
  at all — and the step-out state, completed from the stack dump that answers it),
  `BreakpointWatchpointModel` (the breakpoint and watchpoint lists — file:line breakpoints,
  address-based self-inequality watchpoints — with every edit funnelled through that one gate),
  `SessionLauncher` (`launch()`: ROM selection, the floppy-boot fallback, the backend
  switch and the `SessionConfig`; the quiet/remote refusal path lives here too) and
  `RemoteStateAdapter` (the read-only JSON the remote verbs report; every source it reads is const).
  Each one's `Host` struct declares what it may do to the window (MIN-89) — callbacks only, no
  `MainWindow` pointer — so the seam boundary is a list rather than a privilege: the launcher's
  mid-launch backend swap is `Host::selectBackend(BackendKind)` instead of a write into the
  window's `m_host`, and a seam can be constructed and driven with no window at all, against
  whatever its Host supplies.
- **`ui/ProfilerController.{h,cpp}`** — profiling as a mode: `start()` / `stop()` / `toCursor()` and
  the one session-end reset (the flags are the session's state, not one action's). `showResults()`
  parses the save (`emu/ProfileData`), attributes it to routines and source lines
  (`emu/AttributedProfile`, MAJ-44) and hands that single value to `ProfilerView`, the editor's
  gutter heat and the remote `profile results` verb — the attribution used to live inside the view,
  so the other two readers had to scrape a dock. `results()` holds the last attributed profile.
- **`control/ControlHost.{h,cpp}` / `control/RemoteControl.{h,cpp}`** — `ControlHost` is the
  protocol's view of the IDE: one virtual per verb (`openPathQuiet`, `build`, `run`, `stopSession`,
  `step`/`stepOver`/`resume`, `profileStart`/`profileStop`, `toggleBreakpointAtLine`,
  `debugCommand`, the typed reads, the JSON state reads and `saveScreenshot`) plus six event
  subscriptions (`onBuildCompleted`, `onDebugCommandFinished`, …) which are the `QObject::connect`
  calls a waiter would otherwise make against the window, taking the waiter as the connection's
  context. It is deliberately not a `QObject`: a QObject-derived interface cannot be a *second*
  base of a QObject (the class gets two QObject subobjects and moc's generated code for it stops
  compiling), so the events are explicit hooks instead — see the header's comment.
  `RemoteControl` is a `QTcpServer` on `127.0.0.1`, on the UI thread, that holds a
  `control::ControlHost *`. Each connection line is marshalled onto that thread with a queued
  `QMetaObject::invokeMethod`, then dispatched: verbs call the interface directly, and the
  asynchronous ones wait on its event hooks (a nested `QEventLoop`, as before) instead of on
  `MainWindow` signals. The screenshot verb hands the path to the host: the capture is the UI's
  (XGetImage, `ui/EmbedX11.h`), since only the UI has the window handle.

### The build graph

`CMakeLists.txt` builds one static library per module through the `pist_module()` helper —
`pist_support`, `pist_image`, `pist_build`, `pist_debug`, `pist_emu`, `pist_model`, `pist_project`,
`pist_toolchain`, `pist_git`, `pist_editor`, `pist_control`, `pist_mcp`, `pist_ui` — plus the two
executables (`pist`, `pist-mcp`) and one test executable per suite. The module graph is declared with
one edge per include in the sources, and the layering is enforced by the linker: `support`, `image`,
`build`, `debug`, `emu`, `model`, `project`, `toolchain`, `git` and `mcp` link no `Qt6::Widgets`, so
a core module cannot reach a widget (and by extension cannot reach the UI).

The graph is acyclic, so no link line has to repeat an archive to break a cycle. Two static-library
cycles were stated rather than hidden when the modules were split; both are now gone (MIN-86):

- **control → ui:** nothing under `src/control/` includes or names a `src/ui/` type.
  `control/ControlHost.h` declares the IDE surface the protocol needs, `MainWindow` implements it,
  and `RemoteControl` holds the interface — so the include edge points one way (`ui → control`), and
  `pist_control` needs neither `pist_ui` nor `Qt6::Widgets` for anything it compiles (its link line
  is `Qt6::Network pist_emu`; every symbol it leaves undefined is QtCore/QtNetwork). The interface
  lives in `src/control/` (rather than in a neutral module) because that is the layer that consumes
  it: the shim's module, `pist_mcp`, speaks the socket and never sees the interface, so putting it
  beside the server that uses it holds to the one-edge-per-include rule.
- **ui ↔ editor:** `editor/CodeEditor.cpp` and `editor/AsmHighlighter.cpp` no longer include
  `ui/Appearance.h`. The editor is handed its colours as a value — `pist::EditorTheme`
  (`editor/EditorTheme.h`: the monospace font, the effective darkness, and exactly the colours the
  editor and its highlighter read) — which `ui/Appearance.cpp` composes from the application
  appearance (`appearance::editorTheme()`). `MainWindow` pushes it at construction and on every
  preference change, through `CodeEditor::setTheme()`; the theme travels ui → editor, never back.
  `pist_editor` therefore links `Qt6::Widgets`, `pist_git` and `pist_support`, and nothing else.

## How the pieces connect

`main()` creates one `MainWindow`. `MainWindow` constructs and owns the long-lived objects and
connects their signals:

```
MainWindow
├── QTabWidget            (document tabs)
│   ├── CodeEditor        (current assembly tab; m_editor)
│   └── ImageEditor       (current sprite tab; m_image)
├── BuildService          (build → diagnostics, listings)
├── IDebugBackend m_host  (run/debug → Hatari subprocess; EmulatorHost or HrdbBackend)
├── ProgramLineMap        (listings + live bases → PC ↔ line)
├── BreakpointWatchpointModel m_bpModel  (the two lists + every edit → the arming gate)
├── DebugSessionController    m_session  (session flags, entry attach, armBreakpoints gate)
├── ProfilerController        m_profiling (profile mode; holds the last AttributedProfile)
├── SessionLauncher           m_launcher (run: ROM choice, floppy fallback, SessionConfig → start)
├── RemoteStateAdapter        m_remoteState (read-only JSON for the remote verbs)
├── docks (tabbed panels) ←── fed by the backend's parse results
└── RemoteControl (optional) ── drives MainWindow through control::ControlHost
```

Each of the five seams is constructed with its own `Host` aggregate (MIN-89) — the callbacks that
are the whole of what it can do to the window — and four of them are also reached through
`MainWindow`'s accessors (`sessionController()`, `breakpointsModel()`, `profilerController()`,
`sessionLauncher()`) by the handlers that drive them. The state adapter is not exposed at all: the
remote JSON verbs go through `MainWindow`'s thin `…Json()` helpers, which delegate to it
(`MainWindow.cpp:4019-4056`), and those same helpers are `ControlHost`'s state reads — the interface
and the IDE's own callers meet at the one implementation rather than at two copies of the JSON.

The build path and the debug path share only the line map and the settings; they never talk to each
other's subprocess directly.

## Core flows

### Build

`MainWindow::build()` saves the editor, derives the output paths (`<base>.prg` / `.lst` / `.map`)
from the source via `ProjectSettings`, configures a `BuildService` from those settings (includes,
defines, CPU, sources), and runs it asynchronously. `BuildService::planSteps()` emits one vasm step
(`-Ftos -L listing`) for a single-file project, or a `-Fvobj` step per module plus a `vlink` step
for multi-file. Diagnostics are parsed into `Diagnostic` values; `MainWindow::onBuildFinished()`
fills the Problems tree and rebuilds the `ProgramLineMap` from the listings (+ `LinkMap` when
linked). Editor error markers are set for the currently open file only.

### Run / launch

`MainWindow::run()` sets `m_launchAfterBuild` and calls `build()`; on success
`onBuildFinished()` chains to `launchEmulator()`. That builds a `SessionConfig`, gates the
`--control-socket` argument on the capability probe, calls
`EmulatorHost::writeBootstrapScript()` to write the per-session `boot.ini` (which arms the entry
breakpoint — native only, per `IDebugBackend::setCapabilities`), validates the TOS ROM can
autostart, and calls `m_host->start(config)`.
`start()` isolates the child (`HOME`/`XDG_CONFIG_HOME` point at a per-session dir), optionally pins
`PARENT_WIN_ID` + `SDL_VIDEODRIVER=x11` for embedding, opens the control socket
(`EmbedSocket::listen`, invariant 14) **before** spawning (Hatari is the `connect()`ing client, so
the IDE must already be listening), and runs `SessionConfig::toArgv()`.

### The debug transport (read this before touching EmulatorHost)

Hatari's debugger has no GDB stub and no structured protocol, so the IDE drives it through the
channels it actually offers. Two channels, deliberately **not interchangeable**:

- **stdin carries every debugger command** (`symbols`, `b`, `s`, `n`, `r`, `m`, `d`, …) because it
  is the only channel Hatari reads while the debugger is *stopped*.
- **the control socket is for control commands** (`hatari-stop`, `hatari-option`) and works **only
  while emulation is running**, because Hatari services it solely from its SDL event pump.

Live floppy insert/eject (`IDebugBackend::setFloppyImage`) follows that split. A stopped session
sends Hatari's debugger command `setopt --disk-a|--disk-b <path>` (`none` ejects) over stdin
(native) or `console setopt …` (HRDB). A native session that is *running* uses `hatari-option` on
the control socket instead, because stdin is unread then. HRDB uses `console setopt` in both
states (its TCP channel is serviced in the break loop and while emulating; `needsStop` is false
so a running insert is not deferred). Paths with spaces cannot be given to `setopt` as-is
(Hatari tokenizes with `strtok` and treats quotes as expressions), so they are staged as a
symlink under the session directory. A running native session sends that `setopt` via
`hatari-debug` (the same control-socket path as pause). The AUTO-folder floppy used for TOS
< 1.04 autostart must not overwrite a user image already in A: — that image is moved to B:
and the log says so. Fast-forward is omitted while a user floppy is mounted, because TOS
floppy I/O misses sectors under turbo. The Disk A/B listings in the project pane are a
host-side FAT12 parse of the image file — they update even when no emulator is running, and
they do not mean Hatari has mounted the disk.

**Completion is framed by the `> ` prompt, not by content.** The debugger prints `> ` before each
blocking read and cannot print the next one until the current command finishes, so a prompt is a
true end-of-command signal. Which stream carries it is build-dependent (readline builds → stdout,
non-readline → stderr), so **both** are counted, and a stderr `> ` is only a prompt if the buffer
ends in `> ` with no newline (a `> <cmd>` echo is not a prompt). `EmulatorHost` enqueues a command,
records the current prompt count, waits for the next prompt, then gives stderr a 40 ms settle and
drains the pipe with a real `bytesAvailable()`/`waitForReadyRead()` loop before parsing — because
Qt's buffer is not the pipe.

**Attach is two-phase.** Hatari announces entry with "You have entered debug mode." On the first
stop, `MainWindow` queues `symbols prg`, `info basepage`, `r`, `d`. Only when the `info basepage`
reply arrives (`stateUpdated`) does `MainWindow::onStateUpdated()` call
`ProgramLineMap::setLiveBases()` and then `armBreakpoints()` — because the program's load address
is not known until it has executed (GEMDOS relocates it every run). The entry stop itself uses the
debugger variable `TEXT` (`b pc = TEXT && pc < $e00000 :once`) precisely because it needs no symbols.

### Line mapping (PC ↔ source line)

Source lines come from vasm's `-L` listing, **not** DWARF. Each listing maps a source line to a
*section offset*; the `LinkMap` gives each module's placement within the output sections; and the
live basepage supplies the runtime *section addresses*. `ProgramLineMap::setLiveBases()` combines
the three to resolve line ↔ address in both directions. This is what turns a gutter click into a
`b pc = $addr` and a stopped PC back into a highlighted source line.

### The debug surface

Each panel is a thin view that renders what it is handed: `RegistersView`/`DisassemblyView` take the
snapshot (`setState`), `HardwareView` takes the parser's `HardwareSummary` (`setInfo`),
`StackView` takes the parsed memory rows and `PcHistoryView` the history text. The parsing happens
**once**, in the transport (MAJ-45): `HatariTextParse` and `MemoryDump` turn one `info` / `d` / `m` /
`history` reply into typed values, and the backend emits those — `stateUpdated`, `hardwareInfoReady`,
`historyReady`, … — rather than a transcript a widget would have to interpret. On every stop
`MainWindow` pushes those values into the views. Edits flow the other way: editing a register or a
memory byte emits a signal that `MainWindow` turns into a *typed* backend intent —
`IDebugBackend::writeRegister` / `writeMemoryByte` (`DebugBackend.h`), gated on the debugger being
stopped. Each backend spells its own wire form (native `r <reg>=$<val>` and `w b $<addr> $<val>`;
HRDB the same register form and `memset` for the byte), so no caller builds debugger text. Memory
panes are created by `addMemoryPane()`, each routed by an integer tag so concurrent dumps reach the
right pane. Watchpoints are address-based change-tracking conditional breakpoints (`b ($a).w ! ($a).w`)
and can arm before any stop.

### Embedding the emulator display

On X11, Hatari reparents its own SDL window into `EmulatorDisplayWidget` (Hatari does the reparent;
PiST only supplies the window ID). Two sides must both be X11: the app is pinned to `xcb`, and the
child to `SDL_VIDEODRIVER=x11`; otherwise the option is disabled. Hatari creates its SDL window
*hidden and never maps it*, so `mapEmbeddedWindowChildren()` must map it or the display stays black.
Sizing uses `embeddedContainerSize()` (the real X11 window size) as ground truth — Qt's geometry for
a native dock can disagree — and `resizeEmbeddedChild()` letterboxes the video. During a dock drag
that crosses the video, `setEmbeddedChildrenInputTransparent()` gives the foreign window an empty
input region so the drag keeps tracking (a foreign window otherwise swallows the pointer events).

On Windows there is no handshake to answer. Hatari's reparenting is compiled in only under
`HAVE_X11 && SDL_VIDEO_DRIVER_X11` upstream (`src/sdl/screen.c`, `Screen_ReparentWindow`), while the
`PARENT_WIN_ID` check that creates the SDL window *hidden* (`screen.c:439`) is not inside that guard
at all — so naming the panel in the environment would leave the user with no emulator window
whatsoever if the adoption then failed. PiST therefore leaves the environment alone and adopts the
window Hatari already showed: `findEmulatorWindow()` polls by process id
(`IDebugBackend::emulatorProcessId()`), `embedForeignWindow()` clears `WS_POPUP` and the caption,
sets `WS_CHILD`, and calls `SetParent` in MSDN's order, and `moveEmbeddedChild()` letterboxes it in
the container's physical pixels. The poll stays alive at 500 ms because the emulator replaces its
window on a guest resolution change; every failure path ends at the detached window that already
works, and the panel says so. Input queues are deliberately *not* attached (`AttachThreadInput`):
Hatari pumps no messages while stopped in its debugger (`src/debug/debugui.c` never calls
`SDL_PumpEvents`), so coupling the two queues would freeze the IDE at every breakpoint.

### Panels, docks and the event filter

`MainWindow::createDocks()` enables nesting, installs the app-level event filter, and builds docks
through `makeDock()` (movable/floatable/closable + a stable `objectName`). The debug views are one
tab group; Problems, the Build & debug console and the memory panes are ordinary docks tabbed
together in the bottom area (each a real dock, so they drag and move like any other panel). The
factory arrangement is captured with `saveState()`, the user's restored from `QSettings`, and
re-saved on close. Drag surfaces advertise themselves with an open-hand cursor: `makeDock()` sets it
on the dock (whose painted title bar is its own region) and gives the content an explicit arrow so
the hand does not inherit into the panel body, and the event filter sets it on each dock tab bar
(a `QMainWindowTabBar`, distinguished from a content `QTabWidget`'s bar by class name). The event
filter offers a "Move to" menu on a right-click of a dock **tab** (matched to a dock by title) or
**title bar** (a press inside a dock but outside its content), and arms the drag video pass-through
on a left-press. See README §Emulator embedding and the commit history for why the tab matching is
done by title.

### Remote control

`RemoteControl` (off by default, `--control-port` / `PIST_CONTROL_PORT`) is a localhost TCP line
protocol gated by a per-session token: the first line on every connection must be `auth <token>`
(generated at `listen()`), and anything else is an error and a drop — "localhost only" still
means every local user on a shared machine. The port and token are published to a discovery file
(`PiST/PiST/control-port` under the platform user-data directory, owner-only file and directory),
which is what a flagless `pist-mcp` reads. One command per line after that; replies are one line
(`ok` / `error <msg>`) or a block: a status line (`ok`, or `error` for a failed query), the body
with any line beginning `.` dot-stuffed (one extra leading `.`, stripped by the reader), then a
closing lone `.`. The explicit status line is what keeps a body that merely *starts* with "error"
(a compiler's `error 2 in line`, a debugger transcript) from reading as a failed reply, and the
stuffing is what keeps a body line that is only `.` from ending the block early (MIN-9). Blocking
commands (`build`, `run`, `profile stop`, and the debugger round-trips behind
`cmd`/`readmem`/`disasm`) spin a nested event loop waiting on
`MainWindow` signals so a script never polls. Everything runs on the UI thread: the connection
handler marshals each line with a queued `QMetaObject::invokeMethod`, and `execute()` then calls
`MainWindow` — the asynchronous verbs through `invokeMethod` on the named slots (`openPathQuiet`,
`build`, `run`, `stopSession`, `step`, `stepOver`, `resume`, `toggleBreakpointAtLine`,
`profileStart`, `profileStop`), and the rest (`setRegister`, `setMemoryByte`,
`toggleBreakpointAtLabel`, `addWatchpointAddress`, `debugReadMemory`, `debugReadDisassembly`,
`debugCommand`, `saveCurrentDocument`, `raise`, `activateWindow`) as ordinary calls, alongside the
read-only JSON helpers (`documentJson`, `stateJson`, `problemsJson`, `symbolsJson`, `tabsJson`,
`profilerResultsJson`; `screenshot` uses `captureWindowImage`, not `grabWindow`). `src/control/mcp/`
bridges all of this to MCP: `pist-mcp` is a stdio JSON-RPC server whose tools map onto the verbs,
serving the JSON ones as `structuredContent`, with session events pushed as MCP log notifications.
The `quit` verb is deliberately not exposed as a tool: an agent that can close the user's IDE
unprompted is a foot-gun (the decision lives at the verb, RemoteControl.cpp).

## Invariants — the rules that will bite you

These exist because a test or a real failure caught them. Do not break them.

1. **Never patch, link, or write config for a third-party tool.** Drive vasm/Hatari by CLI only;
   never write a symbol sidecar next to the `.PRG`; never touch the user's `hatari.cfg`.
2. **stdin vs socket is a hard split.** Debugger commands go over stdin (works while stopped);
   control commands over the socket (works only while running). A debugger command sent while
   running is silently deferred by Hatari until the next stop.
3. **Frame on the prompt, not the content.** Count `> ` on both streams; a stderr `> <cmd>` echo is
   not a prompt; a timed-out command (step-over can block on its internal breakpoint) still owes a
   prompt that must be absorbed, or it completes the *next* command with the previous output.
4. **Consume the entry dump before dispatching.** After "You have entered debug mode" the debugger
   prints its session dump; the first command must wait for the entry prompt or the dump is
   attributed to it.
5. **Arm breakpoints only after the basepage arrives.** `setLiveBases()` then `armBreakpoints()` in
   `onStateUpdated()` — never on a zero-delay timer. GEMDOS relocates the program every run, so
   breakpoints are file:line, never raw addresses; re-arming clears first (`b all`). Resume must
   not drop those queued `b` commands: Continue is enabled at the first stop, which is *before*
   arming finishes.
6. **Capability-probe Hatari by option name, never by version** (`probeHatari`). Gate optional CLI
   flags (`--control-socket`, `--debug-except`) on the probe, and the symbol-autoload *form* — the
   bootstrap script writes `symbols autoload debugger` on a build with the three-mode command and
   `symbols autoload on` otherwise. `--symload` is never passed: `SessionConfig::toArgv()` emits
   no such flag.
7. **Never emit `echo` into a Hatari script file** — it aborts Hatari 2.6.1.
8. **TOS autostart needs ≥ 1.04 read from the image header** (`TosRom`), not the filename. Below
   that there is no Pexec, no symbols, and the entry breakpoint never fires on the GEMDOS-HD
   path — so pre-1.04 (and unverifiable) ROMs take the AUTO-folder fallback instead: a generated
   FAT12 floppy (`build/FloppyImage.{h,cpp}`) booted via `--disk-a`, with `--debug` added because
   the `--debug-except` mask only arms on INF load, which a floppy boot never performs.
9. **Qt's buffer is not the pipe.** Drain emulator output with a `bytesAvailable()` /
   `waitForReadyRead()` loop, not a timer or a single read (the settle timer exists for this reason).
10. **`memdump` count is decimal, address is hex**: `m $12596 100` returns 100 bytes, not 0x100.
11. **Reset per-session transport state in `resetTransport()`** (called from `start()`): owed
    prompts, the stderr watermark, the socket buffer and any pending settle timer each corrupt the
    next session's framing in a distinct way.
12. **Embedding needs both sides on X11** and the SDL child is created hidden — always
    `mapEmbeddedWindowChildren()`, always fit against `embeddedContainerSize()`.
13. **Register/memory writes only while stopped**; refresh the view after a write because Hatari
    prints nothing on success.
14. **The control socket path is short and absolute**, and `removeServer()` before `listen()` —
    `sun_path` is ~108 bytes and a stale file breaks the bind.
15. **Tests must pass with no emulator present.** Parser/unit tests always run; emulator and GUI
    tests `QSKIP` themselves when Hatari/vasm/a ROM is absent. Never make a test depend on a real
    display or a real emulator unconditionally.
16. **Never open a destination with `Truncate` and check the byte count afterwards.** The check
    reports the failure honestly and the file is still empty: on a full disk or a quota that is the
    only copy of the user's source, artwork, export or floppy image gone. Write through
    `files::write()` (`support/FileWrite.h`), which stages in the destination's directory, flushes,
    checks the device error and only then replaces. It cannot be delegated to `QSaveFile::commit()`
    either — on Qt 6.8.1, the Qt every release archive bundles, a flush that fails inside `commit()`
    still renames the truncated temporary file over the destination and returns true.

## Testing

Build, then run the suite:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

There are two kinds of test:

- **Parser/unit tests** (`tst_parsers`, `tst_image`, `tst_tosrom`, `tst_debug`, `tst_link`,
  `tst_settings`, `tst_toolfetch`, `tst_oscall`, `tst_profile`, `tst_git`, `tst_mcp`) — always
  run, no emulator and no display needed (`tst_link`'s end-to-end case runs a real vasm + vlink
  when both are present and skips otherwise; `tst_git` skips its live cases without `git`).
- **Integration tests** (`tst_gui`, `tst_remotecontrol`, `tst_emulatorhost`, `tst_hrdb`) — run
  offscreen (`QT_QPA_PLATFORM=offscreen`), drive `MainWindow`, and `QSKIP` themselves unless
  Hatari, `vasmm68k_mot` and a TOS ROM are available (set `PIST_TOS_DIR`;
  `PIST_REQUIRE_EMULATOR=1` turns a skip into a failure for CI). `tst_hrdb` skips unless
  `$PIST_HRDB_HATARI` names the hrdb-main fork binary, which CI supplies. GUI tests synthesize
  input through `QApplication::notify`, which is what the app-level event filter listens on — so
  dock menus and similar are testable without a display.

When you change a behaviour, run the specific test that covers it. A change to the debug loop should
be proven against a real emulator run, not a mock (see PLAN.md §11 "Definition of done").

## How to extend

- **Add a debug view**: create a thin view in `src/ui/` (follow `StackView`/`HardwareView`), add a
  dock for it in `MainWindow::createDocks()`, parse the data in the backend (or reuse an existing
  dump), and wire the signal to a `setX()` on the view. Give the dock a stable `objectName` so the
  layout persists.
- **Add a remote command**: add the verb to `control::kVerbs` (`control/ControlProtocol.h`) and
  dispatch it in `RemoteControl::execute()` (blocking commands wait on a `ControlHost` event hook;
  queries return a block via `replyBlock`). The *operation* goes on `control::ControlHost`
  (`control/ControlHost.h`) and is implemented by `MainWindow` — which is what keeps `src/control/`
  free of UI types; if the verb answers with JSON, the serialisation belongs in
  `RemoteStateAdapter`, whose readers come in through its own `Host`. Document the verb in
  README §Remote control.
- **Add a Hatari CLI option**: emit it in `SessionConfig::toArgv()`, gated on a `HatariCapabilities`
  flag from `probeHatari()` if it is not universal.
- **Add a machine**: extend the `Machine` enum and the TOS-compatibility table in
  `src/model/Machine.cpp`; the settings UI and ROM selection pick it up from there.
- **Add an ST image format**: decode/encode in `src/image/StFormats.{h,cpp}` with a round-trip in
  `tst_image`, then offer it from `ImageEditor::exportFile` / `importFile`. `.pim` is v2-only
  (`ImageDocument::fromJson()` rejects anything else), and each frame carries a `pixels` composite
  beside its `layers` — a frame with no `layers` array loads as one layer built from that composite,
  so keep it current. Tilemaps and PI2/PI3 belong in [FUTURE.md](FUTURE.md); sprite-sheet slicing
  has shipped (`StFormats::composeSheet()` / `sliceSheetCells()`, `ui/SheetCanvas`).

## Where to look next

- **Design rationale, verified findings, launcher rules, risk register, licensing** — [PLAN.md](PLAN.md)
  (§3 architecture, §3.3 transport, §5 launcher rules, §9 verified-vs-unverified ledger).
- **Deferred work and why** — [FUTURE.md](FUTURE.md) (a portable control channel for Windows,
  DWARF, DSP debugging, macOS embedding, packaging).
- **A worked example of a full debug session** — `demo/hello.s` via `./run.sh`.
