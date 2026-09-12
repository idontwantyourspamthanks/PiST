# pist — an IDE for Atari ST assembly development

Cross-platform (Linux, Windows, macOS) Qt6 IDE for developing m68k assembly for the Atari ST/STE,
with an embedded Hatari emulator and integrated debugging.

**Status:** design phase complete. Phase 0 ready to build.
**Project licence:** GPL-2.0-or-later (see §10) — free software, contributions welcome.
**Baseline verified against:** Hatari 2.6.1 (Ubuntu build) + vasm 2.0f, on Linux x86-64.

---

## 1. Scope

| In scope | Out of scope (for now) |
|---|---|
| vasm m68k (Motorola syntax) assembler integration | C/C++ toolchain (m68k-atari-mint, vlink multi-module) |
| Embedded Hatari, driven as a subprocess | Falcon DSP *source-level* debugging |
| Editor with 68k assembly syntax highlighting | Cycle-accurate profiling tooling |
| File/project browser | Floppy image authoring |
| Emulator setup UI (machine, TOS, RAM, monitor, drives) | |
| Build diagnostics in a Problems pane | |
| Breakpoints, stepping, disassembly, registers, memory, hardware views | |
| Source-line mapping (PC ↔ file:line) | |

**Guiding constraint:** the IDE sets emulator state through **command-line arguments**, never by
writing or mutating `hatari.cfg`.

---

## 2. Verified baseline

Everything below was observed by execution or read directly from source during design. See §9 for
the full ledger of what remains unverified.

### 2.1 Toolchain present

```
hatari           /usr/bin/hatari            Hatari v2.6.1 (Feb 2026 build), linked against libreadline
vasmm68k_mot     /usr/local/bin/vasmm68k_mot  vasm 2.0f
gst2ascii        /usr/bin/gst2ascii           DRI/GST → ASCII symbol converter
TOS ROMs         /usr/share/hatari/TOS*.img   supplied by the distro package
```

### 2.2 vasm behaviour

| Behaviour | Observed |
|---|---|
| Assemble | `vasmm68k_mot -quiet -Ftos -o prog.prg src.s` → exit 0 |
| Banner / section sizes | **stdout** (suppressed by `-quiet`) |
| Diagnostics | **stderr**, format `error <N> in line <L> of "<file>": <msg>`, followed by the offending source line prefixed with `>` |
| Columns | **not reported** — no column information |
| Exit code | `1` on error; output file is **deleted** |
| Listing | `-L <file>` → per-line `section:offset bytes … line#: source`, plus symbol tables by name and value |
| `-dwarf=3` + `-Ftos` (multi-section) | **fails**: `error 3004: section attributes <r> not supported` / `fatal error 3008`, exit 1 |
| `-dwarf=3` + `-Felf` | works, emits `.debug_{aranges,info,abbrev,ranges,line}` |
| `-linedebug` | **Amiga hunk output module only** — unusable for `.PRG` |
| Symbols | DRI/GST emitted by default; `-devpac` suppresses them; `-monst` retains |
| Not options | `-g`, `-l` do not exist (`-L` is the *listing* file) |

### 2.3 Hatari debug surfaces (upstream)

| Surface | Verdict |
|---|---|
| GDB stub / RSP | **Does not exist upstream.** No `gdb`/`remotedebug` path; no `--gdb` option |
| Native CLI debugger | Full command set: `a b d m w l r s n c find struct profile history symbols bt*` (CPU) and `da db dd dm dr dspsymbols dp` (DSP) |
| `--control-socket` | POSIX-only; `hatari-debug <cmd>` injects any debugger command; **responses are not returned on the socket** |
| `--cmd-fifo` | Same command set via FIFO; POSIX-only |
| `--parse <file>` | Deterministic command scripts at launch; resolves relative paths against the file's own directory |
| `--conout <0-7>` | Captures guest console output to host stdout |
| Watchpoints | **None.** `grep -rni watchpoint src/debug/` → 0 matches |

\* `bt`/`backtrace` exists on git `main`, **not** on 2.6.1.

### 2.4 Known 2.6.1 vs `main` drift

| Feature | 2.6.1 | `main` |
|---|---|---|
| Symbol autoload field | `bSymbolsAutoLoad` (bool), default **true** (`configuration.c:615`) | `nSymbolsAutoLoad`, default `SYM_AUTOLOAD_DEBUGGER` (`configuration.c:621`) |
| Autoload command | `symbols autoload on\|off` | `symbols autoload <exec\|debugger\|off>` |
| CLI equivalent | *none* | `--symload <mode>` |
| `bt` / backtrace | absent | present |
| `echo` in a `--parse` file | **aborts the emulator** — `assert(s2 < s1)` at `str.c:240` | fixed: `assert(s2 <= s1)` at `str.c:256` |

The `echo` abort is triggered by any argument containing no backslash escape, because
`DebugUI_Echo` calls `Str_UnEscape` on every argument. **Never put `echo` in a bootstrap script.**

---

## 3. Architecture

### 3.1 Shape

```mermaid
flowchart TB
  subgraph IDE["pist (Qt6 Widgets, Qt linked dynamically under LGPLv3)"]
    ED[Editor + 68k mot highlighter]
    PR[Problems pane]
    FB[File / project browser]
    SET[Emulator setup UI]
    DBG[Dock panels: registers, memory, disassembly, breakpoints, hardware, console]
    BK[BuildService]
    LB[LaunchBuilder]
    CL[DebugClient]
    IDB[IDebugBackend interface]
  end

  BK -->|QProcess| VASM[vasmm68k_mot]
  VASM -->|stderr diagnostics| PR
  VASM -->|listing file| LM[LineMap: line → address]
  LB -->|argv only, per-session config dir| HAT[hatari subprocess]
  SET --> LB
  CL --> IDB
  IDB --> NB[NativeBackend: socket + stdin + parse]
  IDB -.->|optional, later| HB[HrdbBackend: TCP 56001]
  NB --> HAT
  HAT -->|stderr: responses| CL
  HAT -->|stdout: prompt + --conout guest console| CL
  CL --> DBG
  LM --> DBG
```

### 3.2 Decisions and rationale

**Emulator is always a separate process.** Hatari's `readme.txt` states that linking it statically
*or dynamically* creates a combined work under the GPL. Keeping Hatari out-of-process is the only
approach that works on all three platforms today, and it keeps the GPL boundary explicit and
auditable — an important property for a project that asks others to contribute to it.

The project itself is GPL-2.0-or-later, so in-process linking would be permissible *provided* the
combined work is conveyed under GPLv2 — Hatari contains three GPL-2.0-**only** files that are
compiled in (audited in §10). Two things nonetheless keep it a subprocess:

1. **Platform capability.** macOS cannot reparent a foreign process window (`WId` is a process-local
   `NSView*`), and embedding via libretro's flat C ABI means reimplementing video, input, and audio
   plumbing that Hatari's SDL frontend already provides.
2. **Maintenance surface.** The transport is text-based and version-gated (§3.3), which is far
   cheaper to keep working against an upstream we do not control than a compiled-in dependency that
   must be rebuilt and re-audited on every release.

In-process libretro remains possible for macOS later — the `libretro/hatari` core is a fork of
exactly the tree audited in §10, so the same GPLv2 conveyance condition applies.

**No Hatari fork for the MVP.** The full debug loop — breakpoints, single-step, step-over, continue,
registers, memory, labelled disassembly, live-relocated symbols, DSP — works on **stock** Hatari.
A fork is only needed for typed framing and memory watchpoints; that is deferred behind
`IDebugBackend`.

**Loopback TCP is a Phase-3 option, not a Phase-0 requirement.** HRDB (`tattlemuss/hatari`,
branch `hrdb-main`, TCP 56001, protocol `0x100a`) provides typed request/response framing, full
conditional breakpoints, DSP registers, and hardware register reads — and works on Windows, unlike
`--control-socket`. GDB (the `dgis/hatari` fork, TCP 2345) is the only path with memory watchpoints
(`Z2-Z4`), but is m68k-only, has no DSP, no `qSymbol`/DWARF, and silently downgrades watchpoints in
some paths.

**Backends are complementary, not ranked:**

| Capability | Native (upstream) | GDB stub (dgis fork) | HRDB (tattlemuss fork) |
|---|---|---|---|
| Register read/write | yes | yes (no USP/ISP) | yes |
| Memory read/write | yes | yes | yes |
| Conditional breakpoints | yes, full expression language | address-only | yes, full expression language |
| Memory watchpoints | **no** | yes (`Z2-Z4`) | no |
| Single-step / step-over | yes | step only (step-over is client-side) | no step-over frame (client-synthesised) |
| DSP registers / breakpoints | **yes** | no | yes |
| Typed framing | no | RSP | yes |
| Platforms | POSIX socket; stdio everywhere | TCP | TCP |

**Embedding strategy per platform:**

| Platform | Approach | Limitation |
|---|---|---|
| X11 | `PARENT_WIN_ID` — Hatari reparents its own SDL window (`src/sdl/screen.c:219`) | X11 only |
| Wayland | force `QT_QPA_PLATFORM=xcb` + `SDL_VIDEODRIVER=x11`, run under XWayland | depends on XWayland |
| Windows | `SetParent` on the Hatari HWND | input-queue/focus quirks |
| macOS | **cannot** reparent a foreign process window (`WId` is a process-local `NSView*`) | detached window, or in-process core |

**A detached emulator window mode ships from day one.** It removes the entire embedding risk class
at no cost, and is the only option on macOS initially.

### 3.3 Debug transport

The native backend uses three asymmetric channels. This is the highest-risk area of the project.

```mermaid
sequenceDiagram
  participant IDE
  participant H as Hatari
  participant E as Editor

  IDE->>H: spawn: --parse boot.ini <builddir>/prog.prg
  Note over H: boot.ini arms: b pc = TEXT && pc < $e00000 :once
  H-->>IDE: stderr "You have entered debug mode."
  Note over H: debugger enters AT PROGRAM START, prints session dump
  H-->>IDE: stdout "> " prompt  (marks entry dump complete)
  IDE->>H: stdin: symbols prg
  H-->>IDE: stderr "Loaded N symbols" + stdout "> "
  IDE->>H: stdin: b pc = $ADDR
  IDE->>H: stdin: c
  Note over H: emulation runs; ONLY NOW is the socket serviced
  H-->>IDE: stderr breakpoint hit, PC dump
  IDE->>E: highlight line for PC (via LineMap)
  IDE->>H: stdin: s / n
  H-->>IDE: stderr new PC + disassembly
```

Note that every debugger command — symbols, breakpoints, stepping — goes over **stdin**, because the
socket is not serviced while the debugger is stopped.

| Channel | Direction | Carries | Notes |
|---|---|---|---|
| stdin | IDE → Hatari | **All debugger commands.** The only channel that works while the debugger is stopped | The debugger blocks in `DebugUI_GetCommand` reading stdin |
| `--control-socket` | IDE → Hatari | `hatari-stop` / `hatari-cont`, `hatari-option`, `hatari-event`; debugger commands **only while emulation is running** | **Hatari is the client**: it calls `connect()` (`control.c:588`) and never binds, so the IDE must already be listening. Returns almost nothing |
| stderr | Hatari → IDE | Command output, debugger output, Hatari log lines | Mixed; responses are delimited by the prompt/echo markers below |
| stdout | Hatari → IDE | The debugger prompt (readline builds), `--conout` guest console | |
| `logfile <f>` | Hatari → file | Registers, memory, disassembly dumps only | Avoids mixing bulk output with logs |

**The control socket is starved while stopped.** `Control_CheckUpdates()` has exactly one call site —
`src/sdl/gui_event.c:134`, the SDL event pump — and none under `src/debug/`. When the debugger is
waiting for input, nothing services the socket, so `hatari-debug` commands sent while stopped are
never read. Verified: with the debugger stopped at entry, a socket `hatari-debug e TEXT` produced
**zero bytes** on stderr, while the same command over stdin worked immediately; resuming emulation
made the socket deliver again.

Consequence: the split is **not** "socket = commands, stdin = stepping". It is
**stdin = everything while stopped**, socket = control commands while running. This also means the
`--control-socket` POSIX-only limitation is a far smaller problem than it first appears: Windows
loses only `hatari-stop`/`hatari-option`-style control, not debugging.

**Command completion is framed by the debugger prompt, not by content.** The debugger writes `> `
before each blocking read, and cannot print the next prompt until the current command has finished
executing, so a prompt is a true end-of-command signal. Which stream carries it is build-dependent:

| Build | Prompt | Command echo |
|---|---|---|
| readline (Linux, macOS) | `readline("> ")` → **stdout** | readline echoes to stdout |
| `!HAVE_LIBREADLINE` (Windows) | `fprintf(stderr, "> ")` → **stderr** | **none** |

Both streams are therefore counted, stderr being recognised by a trailing `> ` with no newline —
which cannot be confused with the `> <cmd>` echoes that `DebugUI_ParseLine` and `DebugUI_ParseFile`
write to stderr, since those always end in a newline.

Two further framing requirements, both found by test failures:

- **Consume the entry dump before dispatching.** After announcing entry the debugger prints its
  session dump (autoloaded symbols, `info` output, registers). Dispatching the first command
  immediately attributes that dump to it; the queue must wait for the entry prompt.
- **Account for owed prompts.** A command that times out (step-over can legitimately block until its
  internal breakpoint hits) is still executing; its late prompt must be swallowed, or it completes
  the *next* command with the previous command's output.

**`echo <nonce>` framing is unnecessary** — and unusable, since `echo` aborts 2.6.1 (§2.4).

**Implementation:** use `QLocalServer`/`QLocalSocket` (`Qt6::Network`, already part of qtbase) rather
than raw POSIX sockets. On Unix these are `AF_UNIX`/`SOCK_STREAM`, exactly matching Hatari's
`socket(AF_UNIX, SOCK_STREAM, 0)` + `connect()`, and they are event-loop driven — no blocking
`accept()` on a GUI thread. Three details:

- Pass the **absolute** socket path to `listen()`; Qt uses it verbatim on Unix, so it is
  byte-identical to the argument given to `--control-socket`.
- Call `QLocalServer::removeServer(path)` before `listen()`, otherwise a socket file left by a
  crashed session causes the bind to fail.
- Keep the path short — `sockaddr_un::sun_path` is limited (108 bytes on Linux), so a per-session
  temp directory must not nest deeply.

---

## 4. Assembler integration

### 4.1 Build

```
vasmm68k_mot -quiet -Ftos -I<incdirs> -D<defines> -L <listing> -o <out.prg> <source.s>
```

Diagnostics parsing (stderr):

```
error 10 in line 2 of "bad.s": number or identifier expected
>	move.l	#0,	d1
```

- Primary regex: `^(error|warning|fatal error) (\d+) in line (\d+) of "([^"]+)": (.*)$`
- **Fallback regex** for line-less diagnostics produced by module-level and fatal failures:
  `^(error|fatal error) (\d+): (.*)$` — file and line are unknown, so these attach to the build log
  rather than a specific editor line. Both forms are real:

  ```
  error 10 in line 2 of "bad.s": number or identifier expected
  error 3004: section attributes <r> not supported
  fatal error 3008: output module doesn't allow multiple sections of the same type ()
  ```
- The next line beginning with `>` is the source excerpt — used to place the caret.
- No column data exists; map by line only, and refine using the listing's per-line byte offsets.
- Output file is deleted on error; the build service must not assume a stale `.PRG` exists.

### 4.2 Line mapping (PC ↔ source line)

Derived from the `-L` listing, **not** from DWARF. The listing contains, for each source line:

```
00:00000008 702A            	     4: start:	move.l	#42,d0
00:0000000A 4E75            	     5: 	rts
01:00000000 6869            	     8: msg_ptr:	dc.b	"hi",0
```

with `Sections:` declaring `00: "text"`, `01: "data"`, `02: "bss"`.

Mapping:

- **line → address:** `section_offset` + (live basepage section address)
- **address → line:** binary search over the flattened map

Live section addresses come from `info basepage` (`TPA start`, `Text/Data/BSS segment`). At the
first debugger stop, `Text segment` equals the program's load address.

This keeps `-Ftos` single-file builds working with zero DWARF, zero GDB, and no fork. DWARF only
becomes relevant for future mixed C/asm interop; multi-module builds should use vlink's map file.

**Verified exact:** listing `loop` at `00:0000000C` → live `0x0125a2` = TEXT `0x012596` + `0x0C`.

---

## 5. Launcher rules (hard-won, non-obvious)

These are the operational constraints the launch builder must encode.

1. **Exactly one positional argument, and it must be the `.PRG`.** This is the only invocation that
   both mounts the GEMDOS HD *and* autostarts the program:
   `Opt_HandleArgument` detects an Atari program, calls `INF_SetAutoStart()`, sets
   `szHardDiskDirectories[0]` to the program's directory, and sets `bBootFromHardDisk`.
   Two positionals produce `Unrecognized option`.
2. **`--gemdos-drive` takes a drive letter only** (`C`–`Z`, or `skip`). The host directory comes
   from the positional argument. Never pass a directory to it.
3. **Autostart requires TOS ≥ 1.04.** Autostart is implemented via `C:\EMUDESK.INF`, and
   `inffile.c:1042` bails for `TosVersion < 0x0104` with
   *"Only TOS versions >= 1.04 support autostarting & resolution overriding!"*.
   On TOS 1.00/1.02 the run silently no-ops: no Pexec, no `CurrentProgramPath`, no autoloaded
   symbols, and the entry breakpoint never fires — the session simply appears to hang.
   Because ROM filenames are arbitrary for user-supplied images, the version must be read from the
   **image header**, not the name: `TOS_LoadImage` reads it as a big-endian 16-bit value at offset 2
   (`src/tos.c:1027`). Two refinements matter when predicting Hatari's behaviour:
   - a filename is **not evidence**; Hatari never reads names, so a version inferred from one must
     not authorise the autostart path. Only a header-derived version may.
   - version bytes are displayed in **hex**, matching Hatari's own `%x.%02x` (`src/tos.c:874`), so
     the field `0x0162` is TOS **1.62**, not 1.98. The filename convention follows the same rule
     ("TOS v1.62"), so a decimal parse silently produces `0x013E`.

   Hatari itself rejects GEMDOS HD entirely below 1.04 with
   *"Please use at least TOS v1.04 for the HD directory emulation"*, so this is a hard floor for the
   whole GEMDOS-HD run path, not just autostart. Three outcomes must be distinguished, never
   collapsed:
   - **known too old** → refuse, naming the version read from the header;
   - **known good** → proceed;
   - **unknown** → warn and ask. Refusing outright would be wrong for a valid ROM whose header is
     unreadable, and proceeding silently reproduces the hang this rule exists to prevent.
4. **Never write `<prg>.sym` next to the `.PRG`.** A sidecar wins over the PRG's own DRI/GST table
   (`symbols.c:922-928`, `1032-1038`) and, because it is loaded with no base offset for code
   symbols, **stale content silently yields wrong addresses**. Put generated symbols in a
   per-session temp directory keyed by build hash.
5. **Prefer `symbols prg` over sidecars.** It re-reads the PRG's DRI table and relocates against the
   live basepage, so it cannot go stale.
6. **Two-phase attach.** Symbol names are unavailable until a table is loaded, so:
   arm `b pc = TEXT && pc < $e00000 :once` at launch (uses the `TEXT` *variable*, needs no symbols)
   → stop → `symbols prg` → only then use names in conditions, disassembly, and memory commands.
7. **Per-session config isolation.** Hatari always loads the user's `hatari.cfg` (only the *global*
   `/etc` file is skipped, and only under `HATARI_TEST`); CLI options are parsed afterwards and
   override it. Point `HOME`/`XDG_CONFIG_HOME` at a temp directory per session so leftover user
   settings (machine, monitor, joysticks, printer) cannot leak in.
8. **Reset-requiring settings are applied by relaunch, not live.** `hatari-option` runs the full CLI
   parser at runtime, but a change needing a cold reset raises a modal `DlgAlert_Query()` inside the
   embedded window — which will hang a headless IDE. Launch with `--alert-level fatal` and
   `--confirm-quit off`, and relaunch the process for machine/RAM/monitor/TOS changes.
9. **Version-gate the bootstrap script.** `symbols autoload on` for 2.6.1; `--symload debugger` for
   `main`. Capability-probe at startup rather than assuming.
10. **The IDE is the socket *server*.** Hatari calls `connect()` (`src/control.c:588`) and never
    binds. Listen on the socket path **before** spawning Hatari, or Hatari exits immediately with
    `connection to control socket failed`. Use `QLocalServer`/`QLocalSocket` (see §3.3), call
    `removeServer()` first to clear a stale path, and keep the path under the `sun_path` limit.
11. **Accept both disassembler output formats.** Which engine produces the disassembly is a *user
    configuration* setting, not a build property: `bDisasmUAE` defaults to `true`
    (`configuration.c:624`), giving `00012596 7001   moveq #$01,d0`, while `false` selects Capstone,
    giving `$00012596 7001   moveq #$01,d0`. Because the IDE always runs with an isolated config
    directory (§5 rule 7) it consistently sees the *default* engine — so a parser tuned against a
    developer's own `~/.config/hatari/hatari.cfg` will differ from what the IDE actually receives.
    Treat the `$` as optional. The same applies to other engine-dependent output.

### 5.1 Bootstrap parse file

Generated per session into the temp config dir:

```
symbols autoload on
b pc = TEXT && pc < $e00000 :once
```

(No `echo`, per §2.4. Relative paths inside a parse file resolve to that file's directory.)

### 5.2 Session argv (template)

```
hatari \
  --tos <rom> --machine <st|megast|ste|megaste|tt|falcon> \
  --memsize <n> --monitor <mono|rgb|vga|tv> --tos-res <res> \
  --ide-master <img> --acsi <id>=<img> \
  --fast-forward yes --sound off \
  --alert-level fatal --confirm-quit off \
  --control-socket <sock> --parse <boot.ini> \
  <builddir>/prog.prg
```

---

## 6. Phases

### Phase 0 — spike (no fork; de-risks the debug loop)

Deliverables:
- CMake + Qt6 Widgets application shell
- Editor with m68k Motorola-syntax highlighter
- `BuildService` invoking `vasmm68k_mot`, parsing stderr diagnostics into a Problems pane
- `LaunchBuilder` producing the argv above with a per-session config dir
- `DebugClient` implementing: bootstrap parse file, control-socket one-shot commands, stdin
  stepping loop, stderr response parsing
- Dock panels: registers, disassembly, memory

**Acceptance:** F5 assembles; a deliberately introduced syntax error appears on the correct line;
the program is launched and stops at its entry point; `symbols prg` relocates; registers and the
current source line update as `s`/`n`/`c` are issued.

*Prerequisite:* `qt6-base-dev` (Qt6 is not currently installed on the development machine).

### Phase 1 — IDE skeleton and setup UI

- Project/workspace model, file browser, settings persistence
- Emulator setup dialog emitting **CLI arguments only**: machine type, monitor, RAM, TOS ROM,
  hard disk images, floppy images, fast-forward, sound
- ROM import with SHA-256 known-good table (Hatari itself only sanity-checks size, version, and
  address in `TOS_LoadImage`, and explicitly skips the ROM's own checksum test)
- EmuTOS shipped as the default ROM, with the GPLv2 source offer
- Toolchain discovery for `vasmm68k_mot`
- Run-path selection paired with the ROM choice (see §5, rule 3)

**Acceptance:** a user can create a project, choose a machine and ROM, build, run, and quit, with no
writes to any real `hatari.cfg`.

### Phase 2 — full debug UI on the native backend

- Breakpoints: address and full conditional expressions, gutter-integrated
- Source-line breakpoints via the `-L` line map
- Registers, two hex memory panes, disassembly with symbol labels, PC history, debug console
- DSP panels (registers, memory, breakpoints) for Falcon targets
- `IDebugBackend` interface with `NativeBackend` as the sole implementation

**Acceptance:** a conditional breakpoint set from the UI fires; a register can be edited; stepping
follows the source line; step-over correctly clears a subroutine.

### Phase 3 — optional backends

- `HrdbBackend` over TCP for hardware register reads
- GDB-stub backend for memory watchpoints
- In-process libretro core for macOS integration (permitted under GPL-2.0-or-later combined with
  Hatari; see §10)

None of Phase 3 changes the UI if `IDebugBackend` is designed correctly.

### Phase 4 — packaging

- Dynamic Qt linking (LGPLv3 relink obligation), dependency notices
- vasm **detect-and-use**, not bundled (see §7)
- Installers per platform

---

## 7. Legal constraints

| Component | License | Can we ship it? | Boundary |
|---|---|---|---|
| **pist** (this project) | GPL-2.0-or-later | — | Chosen for compatibility with the emulator ecosystem; see §10 |
| Hatari | GPL-2.0-or-later, with an explicit statement that static **or dynamic** linking makes a combined work | Yes | **Separate process/binary**. Isolated in `EmulatorHost` so the boundary stays auditable; see §10 |
| libretro Hatari core | GPL-2.0-or-later (identical `readme.txt` blob to upstream) | Yes | Same; `dlopen` does not escape the GPL |
| libretro API header | MIT-style, per-file | Yes | Preserve notice |
| **vasm / vbcc** | Non-free: "may be redistributed without modifications and used for non-commercial purposes" | **Yes — because pist is free software** | **Redistribute unmodified only.** Never patch vasm. Ship its `readme.txt`/manual and mark it as third-party. Any commercial use still needs the author's written consent |
| Original Atari TOS ROMs | Proprietary | No | User-supplied; validate size/version; add our own hash check |
| EmuTOS | GPLv2 | Yes | Data file, not linked. GPLv2 text + source offer |
| Qt 6 | LGPL-3.0-only / GPL-2.0-only / GPL-3.0-only / commercial | Yes | **Dynamic linking** under LGPLv3 (deliver Qt source, relink ability, Installation Information). Qt's GPL-2.0-only option is also available to us, since pist is GPL-2.0-or-later |
| SDL2 (only if embedding upstream Hatari) | zlib | Yes | Acknowledgement |
| Capstone | BSD-3-Clause | Yes | Attach `LICENSE.TXT` |

**Batteries-included is legally achievable.** Because pist is free software, the redistribution
terms of every component permit bundling:

- **EmuTOS** — GPLv2, ships as the default ROM so the IDE works out of the box with no user setup.
- **vasm** — vasm's licence permits redistribution **unmodified** for non-commercial purposes, which
  covers a free IDE. This is the single biggest win of the open-source decision: the assembler can
  be bundled and pinned to a known-good version instead of relying on the user to install it. The
  constraint is that the bundled binary must be byte-for-byte upstream's, so **the IDE must never
  patch vasm**; behaviours we need are obtained by command-line flags only.
- **Hatari** — ships alongside as a separate executable (mere aggregation).

Two distribution rules follow from this:

1. **Never modify bundled third-party binaries.** The IDE drives vasm and Hatari entirely through
   command-line arguments, which is already a design requirement (§1).
2. **A user-supplied toolchain must still work.** Detection order: project-local override →
   user-configured path → bundled. This keeps the IDE usable with a different `vasmm68k_mot`, with
   a newer/patched Hatari, or for commercial use where bundling would not be permitted.

### Toolchain acquisition

Where each component comes from, kept deliberately separate from the source tree:

| Context | vasm | Hatari |
|---|---|---|
| **git repository** | **Never committed.** Keeps the repo 100% free software and DFSG-clean, so distributions and contributors never have to strip a non-free binary | Not vendored; a build dependency |
| **Release artifacts** (Windows/macOS bundles, Linux AppImage) | Bundled unmodified, with its `readme.txt`; non-commercial redistribution is expressly permitted | Bundled as a separate executable (mere aggregation) |
| **Linux distro package** | Optional dependency; the distro's `vasm` package is used if present | Optional dependency |
| **First run without a toolchain** | Offer a guided fetch of the author's official archive, verifying a pinned checksum. This is a *convenience*, never a silent download | Reported with an install hint |

This yields one-click setup on every platform without placing non-free bytes in the repository, and
without ever breaching vasm's no-modification clause.

**The remaining vasm caveat:** its commercial exception is scoped to AmigaOS, so anyone shipping a
*commercial* product based on this IDE must supply their own `vasmm68k_mot` (or obtain the author's
consent). The bundled binary is therefore a convenience for free/non-commercial use, never a
load-bearing dependency — which the detection order above already guarantees.

---

## 8. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Debug transport is three asymmetric channels with no framing | High | `logfile` + quiescence delimiting; capability probe; encapsulate entirely inside `IDebugBackend` |
| Text scraping breaks across Hatari versions | High | Version-gate parsers; probe at startup; pin a tested range |
| Response interleaving with Hatari `INFO`/`WARN` logs | Medium | Route bulk output through `logfile`; filter stderr by known prefixes |
| `echo` aborts the emulator on 2.6.1 | Medium | Never emit `echo` into script files |
| Stale symbol sidecar yields silently wrong addresses | High | Per-session temp dir keyed by build hash; prefer `symbols prg` |
| TOS < 1.04 silently has no autostart | Medium | Pair ROM choice with run-path selection; AUTO-folder or floppy fallback |
| Runtime option change raises a modal dialog and hangs a headless IDE | Medium | `--alert-level fatal`; relaunch for reset-requiring changes |
| User config leakage between sessions | Medium | Per-session `HOME`/`XDG_CONFIG_HOME` |
| macOS cannot embed a foreign window | Medium | Detached-window mode; in-process core as a later option |
| Wayland lacks foreign-window support | Low | XWayland via `QT_QPA_PLATFORM=xcb` |
| Embedded SDL GUI can save over the user's real `hatari.cfg` | Low | Isolated config dir makes this harmless |
| Maintaining a Hatari fork (only if Phase 3 proceeds) | Medium | Keep the patch minimal; upstream accepts patches via the `hatari-devel` mailing list, not GitHub PRs |

---

## 9. Verified vs unverified ledger

### Verified by execution

- vasm: build, exit codes, stderr diagnostic format, output deletion on error, `-L` listing
  contents, `-dwarf` + `-Ftos` failure, `-dwarf` + `-Felf` success
- `gst2ascii` output format and address convention
- Hatari: control-socket connection and `hatari-debug` one-shot commands
- **The control socket is starved while the debugger is stopped** (0 bytes returned), and works
  again once emulation resumes
- **The debugger prompt is a reliable end-of-command signal**, on stdout for readline builds
- `b pc = TEXT && pc < $e00000 :once` armed at launch stops at program entry
- `symbols prg` relocates onto the live basepage exactly (`start` == `Text segment`)
- Stepping: `r`, `s`, `n`, `d`, `c` all functional over stdin
- Disassembly annotated with symbol labels
- `info basepage` field layout
- `--conout 2` captures guest console output to stdout
- Autoload fires on debugger entry, and a `.sym` sidecar takes precedence over the PRG's DRI table
- `echo` in a `--parse` file aborts 2.6.1 (`assert(s2 < s1)`)
- **TOS 1.02 cannot use the GEMDOS-HD run path at all** — Hatari refuses it, so both autostart and
  symbol loading fail silently
- **TOS version bytes are hex**: the field `0x0162` is TOS 1.62 (verified against the four ROMs
  installed on the development machine, whose header values agree with their conventional names)
- **The `r` response does not contain a `CPU=` header**; the PC must be taken from the trailing
  instruction line
- **Disassembly format follows the user's `bDisasmUAE` setting**, not the build
- End-to-end in `pist`: `ctest` runs 9 parser tests, 12 ROM-identification tests, and 7
  emulator-integration tests against a real Hatari, all passing

### Verified by reading source (not executed)

- Absence of a GDB stub and memory watchpoints upstream
- Autoload defaults and gating (`configuration.c:615` / `:621`, `symbols.c`, `debugui.c:1346`)
- Single-positional argument handling and `INF_SetAutoStart` (`options.c:1128-1183`)
- TOS ≥ 1.04 requirement for INF autostart (`inffile.c:1042-1046`)
- `PARENT_WIN_ID` reparenting (`src/sdl/screen.c:219`)
- DSP command set (`dspreg` reads **and writes**)
- Fork status, licences, and the Qt/SDL foreign-window capability matrices

### Unverified — to resolve during Phase 0/1

1. Does a conditional breakpoint on a memory expression work as a practical write-watchpoint?
2. Reliable `logfile` delimiting under rapid command bursts
3. Whether the embedded SDL GUI/shortcuts can be disabled so Hatari cannot rewrite config from
   inside the IDE window
4. Viability of the AUTO-folder / floppy fallback for TOS 1.00/1.02
5. Windows and macOS behaviour of the embedding paths and `SetParent`
6. Behaviour of `--control-socket` alternatives on Windows (only stdio is available)

---

## 10. Licensing decision: open source

**Decided.** pist is **free software, GPL-2.0-or-later**, released for anyone to use and contribute
to. This resolves the `libretro` question that previously gated Phase 3: an in-process emulator core
is *possible*, subject to the compatibility note below.

### Why GPL-2.0-or-later rather than GPL-3.0-or-later

Determined by an actual audit of Hatari's 369 `.c`/`.h` files, not by assumption:

| Finding | Count |
|---|---|
| Canonical header: *"distributed under the GNU General Public License, version 2 **or at your option any later version**"* | 289 files |
| **GPL-2.0-only**: *"Licensed under the terms of the GNU General Public License version 2."* | **3 files** — `src/cpu/uae/{attributes,types,vm}.h` |

Those three files are **not vestigial**. Two independent include chains reach them:

- `src/cpu/sysdeps.h:102` includes `"uae/types.h"`, and `sysdeps.h` is included by `fpp_native.c`
  (`:18`), which is listed in `WINUAE_SRCS` (`src/cpu/CMakeLists.txt:12-13`) and therefore built
  into the `UaeCpu` object library.
- `newcpu.h`, `cpummu.h`, `custom.h` and others likewise pull in `uae/types.h`.

So GPL-2.0-only source is compiled into every Hatari binary, and the resulting work can only be
conveyed under **GPLv2** — the GPL-2.0-only files cannot be relicensed upward.

Consequence:

| Option | Consequence |
|---|---|
| **GPL-2.0-or-later** (chosen) | A GPLv2 combination including Hatari is permitted, so **in-process integration stays available** for the macOS path. Also compatible with EmuTOS (GPLv2) and with Qt under its GPL-2.0-only option |
| GPL-3.0-or-later | **Forecloses in-process integration.** Combining Hatari's GPL-2.0-only files into a GPLv3 work is not permitted. It would also rule out Qt's GPL-2.0-only option for no compensating benefit |
| MIT / Apache-2.0 | Maximum contributor uptake, but permits closed forks of a project whose value lies substantially in its emulator/toolchain integration |
| LGPL-3.0-or-later | Same GPLv2-vs-v3 incompatibility in the linking direction, with less benefit for an end-user application |

"Or later" is retained (rather than plain GPL-2.0-only) so downstream users combining pist with
other GPLv3 code — and *not* with Hatari — may take the whole work to v3.

GPL-2.0-or-later also matches the surrounding ecosystem (Hatari, EmuTOS), which keeps
combined distribution and shared tooling straightforward.

**Consequence for Phase 3:** the in-process libretro core remains licence-compatible in principle,
provided any combined distribution is conveyed under GPLv2. This does not change the plan's
sequencing — it stays an optional backend behind `IDebugBackend` — but the blocker is now
understood precisely rather than assumed.

### What stays out-of-process, and why

Hatari still runs as a subprocess. That is now driven by *platform capability and maintenance cost*
rather than licence necessity, and it keeps the integration surface small and auditable:

- macOS cannot reparent a foreign process window, so a subprocess gives a detached-window experience
  there and real embedding on Linux/Windows.
- The debugger transport (§3.3) is text-based and version-gated, which is easier to maintain against
  an upstream we do not control than a compiled-in dependency.

### Not bundled

Original Atari TOS ROMs remain user-supplied (proprietary). Everything else needed to build and run
assembly out of the box is bundleable — see §7.

---

## 11. Contribution workflow

`master` is the integration branch and should always build and pass tests.

### Branching

One branch per feature or phase, merged back to `master` when complete:

```
feature/phase-0-spike          # CMake skeleton, editor, build service, debug client
feature/phase-1-setup-ui       # project model, file browser, emulator setup dialog
feature/phase-2-debug-ui       # breakpoints, panels, line map
feature/phase-3-hrdb-backend   # optional typed backend
```

Branch naming: `feature/<slug>`, `fix/<slug>`, `docs/<slug>`, `refactor/<slug>`.

### Flow

```bash
git switch master && git pull
git switch -c feature/<slug>
# ... work, committing in logical units ...
git switch master
git merge --no-ff feature/<slug>      # --no-ff keeps the phase boundary visible in history
git branch -d feature/<slug>
```

`--no-ff` is deliberate: it preserves each phase as a recognisable unit in `git log`, which matters
once there are outside contributors.

### Commit messages

```
<area>: <imperative summary>
```

where `<area>` is a subsystem (`build`, `editor`, `debug`, `emu`, `ui`, `docs`), e.g.:

```
build: parse vasm stderr diagnostics into the problems pane
debug: bind control socket before spawning Hatari
```

### Definition of done for a feature branch

1. Builds cleanly (`cmake --build build`).
2. New behaviour verified against the real thing — a real build, a real emulator run — not a mock.
3. `docs/PLAN.md` updated if a verified fact, launcher rule, or phase scope changed. **Findings that
   contradict the plan should be recorded in §9 rather than silently absorbed.**
4. No writes to a user's real `hatari.cfg` (§5 rule 7).

### Standing engineering rules

Derived from the constraints in §5; these are not style preferences, they are correctness
requirements:

- Never patch bundled third-party binaries — drive them by command-line arguments only (§7).
- Never write a symbol sidecar next to the `.PRG` (§5 rule 4).
- Never emit `echo` into a Hatari script file (§2.4).
- Capability-probe Hatari at startup; never assume a feature exists in the user's build (§2.4, §5).
- Keep all emulator-specific knowledge behind `IDebugBackend`/`EmulatorHost`.

---

## Appendix A — useful commands

```bash
# build with a listing for line mapping
vasmm68k_mot -quiet -Ftos -L prog.lst -o prog.prg prog.s

# convert DRI/GST symbols to Hatari's ASCII format (only if a sidecar is required)
gst2ascii prog.prg > prog.sym

# run with debugger attached, GEMDOS HD mounted at the PRG's directory
hatari --tos tos.img --machine ste --memsize 1 --monitor mono \
       --alert-level fatal --confirm-quit off \
       --control-socket /tmp/pist.sock --parse boot.ini /path/to/prog.prg
```

## Appendix B — key source references

| Topic | Location |
|---|---|
| Control socket command set | `src/control.c` — `Control_ProcessBuffer`, `Control_Usage` |
| Embed size reply format | `src/control.c:64-74` — `Control_SendEmbedSize` (`"WxH"`) |
| Debugger main loop / stdin | `src/debug/debugui.c` — `DebugUI()`, `DebugUI_GetCommand` |
| `logfile` (short `f`) | `src/debug/debugui.c` — command table |
| CPU commands | `src/debug/debugcpu.c:1429` |
| DSP commands | `src/debug/debugdsp.c` |
| Symbol formats and sidecar precedence | `src/debug/symbols.c:922-928`, `1032-1038` |
| Autoload on debugger entry | `src/debug/debugui.c:1346` |
| Autoload trigger on Pexec | `src/gemdos.c:4593` |
| Basepage fields | `src/debug/debugInfo.c` — `BASEPAGE_SIZE 0x100` |
| Positional argument handling | `src/options.c:1128-1183` |
| INF autostart TOS gate | `src/inffile.c:1042-1046`, `695` |
| Runtime option application | `src/change.c` — `Change_ApplyCommandline`, `Change_Options` |
| X11 reparenting | `src/sdl/screen.c:219` |
