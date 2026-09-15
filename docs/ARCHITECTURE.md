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
| `src/ui/` | `MainWindow` (the application shell: document tabs via a central `QTabWidget`, with `m_editor` as the current text editor and `m_image` as the current sprite editor) and every debug panel, plus the X11 display embedding (`EmulatorDisplayWidget`, `EmbedX11`), `Appearance` (dark-first Fusion theme with a GEM-green accent, editor font family/size, toolbar icons; preferences in QSettings), and the sprite editor (`ImageEditor`, `ImageCanvas`). |
| `src/editor/` | `CodeEditor` (the editor widget: line-number gutter, breakpoint dots, execution-line highlight, error markers) and `AsmHighlighter` (m68k Motorola syntax, with light and dark colour sets). |
| `src/image/` | Sprite document and ST graphics: `ImageDocument` (v2 `.pim` JSON: phases own their frames and cell size and carry sprite-sheet placement; `StFormats::composeSheet()`/`sliceSheetCells()` move between placed phases and sheet images), `Palette` (STfm 512 / STe 4096 cubes and colour words), `Tools` (brush/line/rect/fill), `Transform` (flip, rotate, onion/phase math), `StFormats` (PI1, NEO, IFF, STOS MBK, PNG, assembler include, plus `spriteSafeDocument()` — the palette shift that keeps colour 0 as background so an export re-imports losslessly). |
| `src/build/` | `BuildService` (plans and runs vasm/vlink steps), `Diagnostic` (a parsed warning/error), the line maps — `LineMap`, `LinkMap`, `ProgramLineMap` — and `FloppyImage` (AUTO-folder FAT12 writer for pre-1.04 TOS, plus `.st` / `.msa` listing, export, and in-place edits — `readFileRaw` extracts a file's bytes and `updateImage` rewrites an existing image with entries added and removed). |
| `src/emu/` | `IDebugBackend` (`DebugBackend.h`, the transport contract) with two implementations: `EmulatorHost` (stock Hatari over stdin/prompt framing) and `HrdbBackend` (the hrdb-main fork over TCP 56001); `HatariTextParse` (their shared `d` parser), `SessionConfig` (one session's argv), `HatariProbe` (capability detection), `TosRom` (ROM discovery + version), `Machine` (ST/STe/TT/Falcon model), `MemoryDump` (memdump parsing), `Paths` (ROM/session directories). |
| `src/debug/` | `Breakpoint` (the file:line model and the pure `planBreakpoints()` that turns lines into `b pc = $addr` commands) and `Watchpoint` (a change-tracking conditional breakpoint). |
| `src/control/` | `RemoteControl` — the localhost TCP line protocol that drives the IDE from a script or an AI agent. |
| `src/project/` | `ProjectSettings` — the per-project `.pistproject` JSON file (build + emulator settings). |
| `src/toolchain/` | `Toolchain` — discovery of vasm, vlink and Hatari (explicit path → beside the exe → bundled tools dir → `PATH`), plus install hints; `ToolFetch` — the checksum-pinned fetch/build/install the setup dialog drives (`ui/SetupDialog`, shown at startup when the assembler or ROM is missing). |
| `tests/` | Parser and image unit tests (always run) and the offscreen GUI/emulator integration tests (gated on tools being present). |
| `demo/` | `hello.s` — a tiny program used by `run.sh` and the first-run experience; `demo.pim` — a sample sprite. |

### Per-file detail

- **`ui/MainWindow.{h,cpp}`** — the central controller and by far the largest file. It owns the
  document tabs (`CodeEditor` and `ImageEditor`), every dock, the `BuildService`, the
  `EmulatorHost` and the `ProgramLineMap`; creates the actions/menus/toolbar/status bar; persists
  the layout and the embedded-display preference; owns the app-level event filter (dock move menu +
  drag pass-through); and drives breakpoints, watchpoints and PC→line navigation. Almost everything
  is wired together here. `openPath()` dispatches `.pim` / ST still-image files to `addImageTab()`;
  Build/Run use `buildSourcePath()` so an image tab being focused does not strand the assembler.
- **`ui/EmulatorDisplayWidget.{h,cpp}`** — a black `WA_NativeWindow` container whose `winId()` is
  handed to Hatari. Tracks the emulator's video size, aspect-fits it inside the dock, and paints a
  "Paused" badge when stopped.
- **`ui/EmbedX11.{h,cpp}`** — free X11 functions over Qt's `QX11Application` native interface:
  `mapEmbeddedWindowChildren`, `embeddedContainerSize`, `resizeEmbeddedChild`,
  `setEmbeddedChildrenInputTransparent`, `captureWindowImage`. All no-ops without
  `PIST_HAVE_X11`/`PIST_HAVE_XEXT`.
- **Debug panels** (`ui/`): `RegistersView` (editable D0–D7/A0–A7, PC, SR, USP/ISP, flags),
  `DisassemblyView` (current PC highlighted), `MemoryView` (hex, byte editing, address navigation),
  `StackView` (longs at SP, return-address annotation), `HardwareView` (`info <subject>` output),
  `PcHistoryView` (`history` output), `BreakpointPanel` (breakpoint + watchpoint table),
  `FileBrowser` (hard-drive project tree plus Disk A/B floppy listings, with a browser-wide clipboard and drag & drop that copy and move entries within and between the panes — onto a disk this rewrites the image via `floppy::updateImage`; double-clicking a text or still-image entry on a disk extracts it to the session directory — text as an editor tab, a still image as a sprite-sheet document whose phases slice the sheet — and saving that tab writes the recomposed sheet back through `MainWindow::writeBackFloppyDoc`). Each is a thin view;
  `MainWindow` feeds it parsed state and routes its edit/activation signals to debugger writes.
- **`editor/CodeEditor.{h,cpp}`** — `QPlainTextEdit` subclass with a `LineNumberArea` gutter.
  Emits `gutterClicked` (breakpoint toggle) and `gutterContextMenuRequested`; paints the execution
  line and error underlines via extra selections.
- **`image/`** — palette cubes, `.pim` load/save, paint geometry, layers/phases, and ST file codecs. No widgets.
  **`ui/ImageEditor.{h,cpp}`** is the document tab; **`ui/ImageCanvas`** paints the grid with onion-skin.
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
- **`control/RemoteControl.{h,cpp}`** — a `QTcpServer` on `127.0.0.1`. Reaches the IDE *only* by
  `QMetaObject::invokeMethod` on `MainWindow` slots, and waits on its signals for blocking commands.

## How the pieces connect

`main()` creates one `MainWindow`. `MainWindow` constructs and owns the long-lived objects and
connects their signals:

```
MainWindow
├── QTabWidget            (document tabs)
│   ├── CodeEditor        (current assembly tab; m_editor)
│   └── ImageEditor       (current sprite tab; m_image)
├── BuildService          (build → diagnostics, listings)
├── EmulatorHost          (run/debug → Hatari subprocess)
├── ProgramLineMap        (listings + live bases → PC ↔ line)
├── docks (tabbed panels) ←── fed by EmulatorHost parse results
└── RemoteControl (optional) ── invokes MainWindow slots
```

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
`--control-socket` argument on `m_caps.hasControlSocket`, calls
`EmulatorHost::writeBootstrapScript()` to write the per-session `boot.ini` (which arms the entry
breakpoint), validates the TOS ROM can autostart, and calls `EmulatorHost::start(config)`.
`start()` isolates the child (`HOME`/`XDG_CONFIG_HOME` point at a per-session dir), optionally pins
`PARENT_WIN_ID` + `SDL_VIDEODRIVER=x11` for embedding, opens the `QLocalServer` **before** spawning
(Hatari is the `connect()`ing client, so the IDE must already be listening), and runs
`SessionConfig::toArgv()`.

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

Each panel is a thin view. On every stop, `EmulatorHost` emits parse results (registers,
disassembly, stack, hardware `info`, memory dumps, PC history) and `MainWindow` pushes them into the
views. Edits flow the other way: editing a register or a memory byte emits a signal that `MainWindow`
turns into a debugger write (`r d0 <val>`, `memwrite`), gated on the debugger being stopped. Memory
panes are created by `addMemoryPane()`, each routed by an integer tag so concurrent dumps reach the
right pane. Watchpoints are address-based change-tracking conditional breakpoints
(`b ($a).w ! ($a).w`) and can arm before any stop.

### Embedding the emulator display

On X11, Hatari reparents its own SDL window into `EmulatorDisplayWidget` (Hatari does the reparent;
PiST only supplies the window ID). Two sides must both be X11: the app is pinned to `xcb`, and the
child to `SDL_VIDEODRIVER=x11`; otherwise the option is disabled. Hatari creates its SDL window
*hidden and never maps it*, so `mapEmbeddedWindowChildren()` must map it or the display stays black.
Sizing uses `embeddedContainerSize()` (the real X11 window size) as ground truth — Qt's geometry for
a native dock can disagree — and `resizeEmbeddedChild()` letterboxes the video. During a dock drag
that crosses the video, `setEmbeddedChildrenInputTransparent()` gives the foreign window an empty
input region so the drag keeps tracking (a foreign window otherwise swallows the pointer events).

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
protocol. One command per line; replies are `ok` / `error <msg>` or a block ending in a lone `.`
line. Blocking commands (`build`, `run`, `cmd`) spin a nested event loop waiting on `MainWindow`
signals so a script never polls. It reaches the IDE only through `QMetaObject::invokeMethod` on
`MainWindow` slots (`openPath`, `build`, `run`, `step`, `toggleBreakpointAtLine`, `setRegister`,
`setMemoryByte`, `addWatchpointAddress`, `debugCommand`, …) and read-only helpers (`screenshot`
uses `captureWindowImage`, not `grabWindow`). There is no authentication — it is localhost-only by
design.

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
   flags (`--control-socket`, `--symload`, `--debug-except`) on the probe.
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

## Testing

Build, then run the suite:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

There are two kinds of test:

- **Parser/unit tests** (`tst_parsers`, `tst_image`, `tst_tosrom`, `tst_debug`, `tst_link`,
  `tst_settings`) — always run, no emulator needed.
- **Integration tests** (`tst_gui`, `tst_remotecontrol`, `tst_emulatorhost`) — run offscreen
  (`QT_QPA_PLATFORM=offscreen`), drive `MainWindow`, and `QSKIP` themselves unless Hatari,
  `vasmm68k_mot` and a TOS ROM are available (set `PIST_TOS_DIR`; `PIST_REQUIRE_EMULATOR=1` turns a
  skip into a failure for CI). GUI tests synthesize input through `QApplication::notify`, which is
  what the app-level event filter listens on — so dock menus and similar are testable without a
  display.

When you change a behaviour, run the specific test that covers it. A change to the debug loop should
be proven against a real emulator run, not a mock (see PLAN.md §11 "Definition of done").

## How to extend

- **Add a debug view**: create a thin view in `src/ui/` (follow `StackView`/`HardwareView`), add a
  dock for it in `MainWindow::createDocks()`, parse the data in `EmulatorHost` (or reuse an existing
  dump), and wire the signal to a `setX()` on the view. Give the dock a stable `objectName` so the
  layout persists.
- **Add a remote command**: add a slot to `MainWindow`, then dispatch it in
  `RemoteControl::execute()` (blocking commands wait on a `MainWindow` signal; queries return a
  block via `replyBlock`). Document it in README §Remote control.
- **Add a Hatari CLI option**: emit it in `SessionConfig::toArgv()`, gated on a `HatariCapabilities`
  flag from `probeHatari()` if it is not universal.
- **Add a machine**: extend the `Machine` enum and the TOS-compatibility table in
  `src/emu/Machine.cpp`; the settings UI and ROM selection pick it up from there.
- **Add an ST image format**: decode/encode in `src/image/StFormats.{h,cpp}` with a round-trip in
  `tst_image`, then offer it from `ImageEditor::exportFile` / `importFile`. Keep `pixels` on each
  `.pim` frame so older readers still load the composite; extra keys (`layers`, `phases`) are
  ignored by v1. Tilemaps, sprite-sheet slicing, and PI2/PI3 belong in [FUTURE.md](FUTURE.md).

## Where to look next

- **Design rationale, verified findings, launcher rules, risk register, licensing** — [PLAN.md](PLAN.md)
  (§3 architecture, §3.3 transport, §5 launcher rules, §9 verified-vs-unverified ledger).
- **Deferred work and why** — [FUTURE.md](FUTURE.md) (a portable control channel for Windows,
  DWARF, DSP debugging, macOS embedding, packaging).
- **A worked example of a full debug session** — `demo/hello.s` via `./run.sh`.
