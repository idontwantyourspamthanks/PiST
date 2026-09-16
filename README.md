# PiST

[![CI](https://github.com/idontwantyourspamthanks/PiST/actions/workflows/ci.yml/badge.svg)](https://github.com/idontwantyourspamthanks/PiST/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/idontwantyourspamthanks/PiST)](https://github.com/idontwantyourspamthanks/PiST/releases)
[![Licence: GPL-2.0-or-later](https://img.shields.io/badge/licence-GPL--2.0--or--later-blue)](LICENSE)

**An IDE for Atari ST assembly development.**

*Program in ST* — an IDE for writing 68000 assembly for the Atari ST.

Write 68000 assembly, assemble it, run it in an emulator, and debug it — without leaving the
editor. `PiST` bundles the pieces that are otherwise scattered across a text editor, a build
script, a terminal, a debugger and an emulator, and presents them as one tool.

> **Status: early, and usable.** The full loop works — write, assemble, run under Hatari, and
> debug with breakpoints, stepping, registers, memory, disassembly, watchpoints, a stack
> view and hardware registers, with the editor following the program counter. Projects have persistent settings (include paths, defines, machine, ROM,
> RAM, disk images). Linux, macOS and Windows all build and pass their tests in CI.
>
> What is missing is breadth rather than core function: no installers, and the emulator ships only
> in the Linux AppImage — the macOS and Windows archives still expect you to install Hatari. The
> integration itself has only been exercised on Linux — see
> [Known limitations](#known-limitations). [docs/PLAN.md](docs/PLAN.md) has the full design.

---

## What it does

```
  write  ──▶  assemble  ──▶  run in Hatari  ──▶  debug
   .s         vasmm68k_mot        .prg            breakpoints, stepping,
                                                   registers, disassembly
```

- **Editor** with m68k Motorola-syntax highlighting, error markers and the current execution line
- **Sprite editor** — File → New Image… (or open a `.pim`; import Degas `.PI1`,
  NeoChrome `.NEO`, IFF, PNG) to paint on a pixel grid with the STfm/STe palettes,
  layers, onion-skin, frames with an animated preview, and export to those formats
  plus STOS `.MBK` and an assembler include. Importing adopts the file's palette
  as the active set. The select tool drags a rectangle you
  can move (drag inside it or nudge with the arrows), and copy, cut, paste
  (Ctrl+C/X/V) and delete; pasting keeps transparent pixels see-through.
- **Project files** — the left pane groups the host project (the GEMDOS hard drive) with Disk A
  and Disk B. Change or eject a floppy there (the same paths as in project settings); export
  selected hard-drive files to a new `.st` or `.msa` image
- **Project settings** — include paths, defines, target CPU, and the emulator's machine, ROM,
  monitor, RAM, hard disk and floppy images, saved beside the source in a small JSON file
- **Build** through `vasmm68k_mot`, with its diagnostics shown against the exact source line
- **Run** in [Hatari](https://www.hatari-emu.org/), launched with the project's settings
- **Debug** with breakpoints, single-step, step-over, registers, memory and labelled
  disassembly — and the editor following the program counter as you step
  - **edit registers and memory** while stopped — poke a value and keep debugging
  - **multiple memory panes**, each watching its own region
  - **Watchpoints** break when a memory value changes (Hatari has no data watchpoints, so they are
    armed as change-tracking breakpoints), settable from Run ▸ Add watchpoint or the remote control
  - a **stack view** of the values at the stack pointer, with likely return addresses marked
  - a **hardware registers** view of the shifter, MFP, ACIA/IKBD, sound, blitter and more, from
    Hatari's `info` commands
  - a **PC history** view of how the machine reached the current stop
  - an **interactive debugger console** — type any Hatari debugger command (`r`, `d`,
    `m $12596 20`, …) and see its output in the console dock
- **Movable, tabbed debug panels** — arrange the views and the emulator display however you like;
  a hand cursor marks the drag surfaces (drag a title bar to move a panel between areas, drag a tab
  to rearrange), or right-click for a "Move to" menu. The layout persists.

The goal is *batteries included*: the toolchain and emulator ship with the IDE where their licences
allow and a usable version can be packaged, so there is nothing to assemble by hand before writing
your first line of code. The Linux AppImage meets that goal today; the macOS and Windows archives
still need Hatari installed separately.

On a machine with no assembler or ROM, the first run offers a **guided setup**: a
checksum-pinned vasm source build and an EmuTOS download, each named with its URL and checksum
before anything is fetched. Tools ▸ Set up tools and ROMs… reopens it.

## Target platform

Atari ST / STE (and, later, Mega ST, TT and Falcon) 68000 assembly, assembled with Motorola syntax.

The IDE itself is cross-platform: **Linux, Windows and macOS**.

## What it is built on

| Layer | Choice | Why |
|---|---|---|
| UI | Qt 6 (Widgets) | One codebase for all three platforms; strong tooling |
| Assembler | `vasmm68k_mot` | The de-facto standard for 68k assembly; excellent diagnostics |
| Emulator | Hatari | Accurate, actively maintained, and has a scriptable debugger |
| Line mapping | vasm's `-L` listing | Gives exact source-line ↔ address mapping with no DWARF and no cross-GDB |

A deliberate choice runs through all of this: **the assembler and emulator are driven as
subprocesses, through command-line arguments only.** They are never patched, never linked in, and
their configuration files are never rewritten. That keeps the integration small, keeps the licences
clean, and means a newer (or a user-supplied) toolchain just works.

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│  PiST  (Qt 6, GPL-2.0-or-later)                                      │
│                                                                      │
│  ┌────────────┐   ┌──────────────┐   ┌──────────────┐                │
│  │  Editor    │   │  Problems    │   │  Debug panels│                │
│  │  + ASM     │◀──│  pane        │   │  registers   │                │
│  │  highlight │   │              │   │  disassembly │                │
│  └────────────┘   └──────────────┘   │  memory      │                │
│         ▲                ▲           └──────▲───────┘                │
│         │                │                  │                        │
│         │           ┌────┴─────┐      ┌─────┴──────┐                 │
│         │           │ Build    │      │ Emulator   │                 │
│         │           │ Service  │      │ Host       │                 │
│         │           └────┬─────┘      └─────┬──────┘                 │
│         │                │                  │                        │
│         │           ┌────┴─────┐            │                        │
│         └───────────│ LineMap  │            │                        │
│           (PC↔line) │ (-L lst) │            │                        │
│                     └──────────┘            │                        │
└─────────────────────────────────────────────┼────────────────────────┘
                                              │
                          ┌───────────────────┴────────────────────┐
                          │ stdin · stdout · stderr · unix socket  │
                          └───────────────────┬────────────────────┘
                                              │
                                   ┌──────────┴──────────┐
                                   │   hatari            │  separate process;
                                   │   (GPL-2.0-or-later)│  never linked in
                                   └─────────────────────┘

        vasmm68k_mot ──▶ .prg + .lst        (separate process, CLI only)
```

### The debug transport

Stock Hatari's debugger has no GDB stub and no structured protocol, so the IDE drives it through the
channels it actually offers. That turned out to be the subtlest part of the project, and the
findings are worth knowing before touching the code:

- **The control socket is dead while the debugger is stopped.** Hatari only services it from its SDL
  event pump, so all debugger commands go over **stdin**. The socket is for control commands
  (`stop`/`continue`/option changes) while emulation is *running*.
- **The debugger prompt (`> `) is the command delimiter.** It marks the true end of a command, and
  which stream carries it depends on how Hatari was built.
- **Symbols are resolved in two phases**: stop at the program's entry point first (using Hatari's
  `TEXT` variable), *then* load symbols, because the program's load address is not known until it has
  been executed.
- **Source lines come from the listing**, not from DWARF: vasm's `-L` output maps each source line to
  a section offset, and the live base page supplies the section addresses.

These are all consequences of driving an emulator we do not control, and each one exists because a
test caught it. `docs/PLAN.md` §3.3 and §5 record them in full, along with the evidence.

**An alternative transport exists.** `IDebugBackend` abstracts the debug channel, with two
implementations: the native one above, and `HrdbBackend`, which speaks the typed TCP protocol of
the [hrdb-main fork of Hatari](https://github.com/tattlemuss/hatari) (upstream 2.6.1 plus a
remote-debug listener). The bundled emulator **is** that fork — the Linux AppImage ships it as
`hatari`, and PiST detects it by binary content and selects HRDB automatically. A stock Hatari
you install yourself lands on the native transport; Project Settings → Debug transport can force
either. HRDB works on Windows, carries live section bases in its register reply, and can pause a
*running* program — the things the native transport cannot do where Hatari's control socket is
absent.

### Where the code lives

| Path | Contents |
|---|---|
| `src/main.cpp` | Entry point: platform pinning, `--diagnose`, `--control-port`, the single `MainWindow` |
| `src/ui/` | `MainWindow` (the shell) and every debug panel; X11 display embedding (`EmbedX11`, `EmulatorDisplayWidget`) |
| `src/editor/` | `CodeEditor` (gutter, execution line, error markers) and `AsmHighlighter` (m68k Motorola syntax) |
| `src/build/` | `BuildService` (drives vasm/vlink), `Diagnostic`, and the line maps — `LineMap`, `LinkMap`, `ProgramLineMap` |
| `src/emu/` | `IDebugBackend` (the transport contract) with `EmulatorHost` (stock Hatari, stdin/prompt framing) and `HrdbBackend` (hrdb-main fork, TCP 56001); `HatariTextParse`, `SessionConfig`, `HatariProbe`, `TosRom`, `Machine`, `MemoryDump`, `Paths` |
| `src/debug/` | `Breakpoint` (file:line model + arming plan) and `Watchpoint` |
| `src/control/` | `RemoteControl` — the localhost TCP line protocol that drives the IDE |
| `src/project/` | `ProjectSettings` — the per-project `.pistproject` JSON |
| `src/toolchain/` | `Toolchain` — discovery of vasm, vlink and Hatari |
| `src/ui/SetupDialog.cpp` | First-run setup: checksum-pinned vasm source build and EmuTOS download |
| `tests/` | Parser unit tests and the offscreen GUI/emulator integration tests |
| `docs/` | Design documents — [PLAN](docs/PLAN.md), the [codebase guide](docs/ARCHITECTURE.md), [FUTURE](docs/FUTURE.md) |

## Building

Requirements: **CMake ≥ 3.21**, **Qt 6.5+** (Widgets and Network), a C++17 compiler.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

To try it without installing anything, `./run.sh` builds if needed and launches
the IDE with `demo/hello.s` open — a tiny program you can assemble, run and step
through to see the whole loop. `demo/demo.pim` is a sample sprite set for the
image editor — two phases ("Atari ST" 32×32, "Bum" 16×16) you can lay out on a
sprite sheet and export. It always uses your real display; run the tests
(below) for the headless checks.

Run the tests:

```sh
ctest --test-dir build --output-on-failure
```

There are two suites: parser tests (always run) and emulator integration tests, which skip
themselves unless Hatari, `vasmm68k_mot` and a TOS ROM are available.

### Installing

Download an archive from [Releases](../../releases). The macOS and Windows archives
contain PiST, the `vasmm68k_mot` assembler, and an EmuTOS ROM; the Linux AppImage
carries those plus **Hatari (the hrdb-main fork, 2.6.1-based)**, so a fresh download runs and debugs with
nothing else installed:

```sh
chmod +x pist-*-linux-x86_64.AppImage
./pist-*-linux-x86_64.AppImage your-program.s
```

On macOS and Windows you still need **Hatari** for the emulator: it is not bundled
there — there is no MSYS2 Hatari package for Windows — so install it yourself, or
point PiST at one in Project Settings. Everything else is included, so the IDE
assembles out of the box once Hatari is present.

To install from source instead:

```sh
cmake --install build --prefix ~/.local
```

which installs the binary, a desktop entry and icon (on Linux), and the licence
and third-party notices — but not vasm or Hatari, so see below.

To develop the IDE you will also want, at runtime:

- **`vasmm68k_mot`** — see [Assembler](#assembler)
- **Hatari** — see [Emulator](#emulator)
- **A TOS ROM, version 1.04 or later** — see [TOS ROMs](#tos-roms)

Both tools are discovered in this order, so no configuration is needed in the
common case:

1. an explicit path set in **Project Settings**;
2. beside the PiST executable (how release bundles are laid out);
3. `~/.local/share/PiST/tools` (`%LOCALAPPDATA%` on Windows) — the same place a
   bundled copy would live;
4. the system `PATH`.

If one is missing, PiST says which and where to get it rather than failing later
with a process error.

## Platform support

Every platform is built and tested on real runners by CI: **Linux, macOS and
Windows all compile warning-free and pass the full test suite**, and all three
install cleanly. See the badge in the Actions tab for the current state.

All three targets build and run a full assemble → run → debug session. One feature differs, because
of an upstream Hatari limitation rather than anything in `PiST`:

| Capability | Linux | Windows | macOS |
|---|---|---|---|
| Build, run, break at program entry | ✓ | ✓ | ✓ |
| Registers, memory, disassembly, stepping | ✓ | ✓ | ✓ |
| Breakpoints (set before launch) | ✓ | ✓ | ✓ |
| Break in when the program faults | ✓ | ✓ | ✓ |
| Pause a healthy running program | ✓ | ✗\* | ✓ |
| Change breakpoints while running | ✓ | ✗\* | ✓ |
| Swap disk images while stopped | ✓ | ✓ | ✓ |
| Swap disk images while the program is running | ✓ | ✗\* | ✓ |

\* Pause on Windows, and live breakpoint / running-disk changes there, work when the debug
transport is the [hrdb-main fork](https://github.com/tattlemuss/hatari) (Project Settings → Debug
transport), which speaks typed TCP instead of the POSIX-only control socket. Stock Hatari on
Windows still lacks those *while the program is running*; a stopped session can still insert or
eject a floppy via the debugger (`setopt`).

The three gaps are all downstream of one thing: Hatari's control channel is compiled only on
POSIX systems (`HAVE_UNIX_DOMAIN_SOCKETS`), and it is the only way to command an *already running*
emulator. `PiST` detects this and omits the option rather than passing it and failing to start.
Everything else works everywhere, because debugger commands travel over stdin — the control socket
is starved whenever the debugger is stopped anyway, so it was never carrying the debug traffic.

Removing that gap is a small, well-understood upstream patch. It is written up, with the proposed
approach, in **[docs/FUTURE.md](docs/FUTURE.md)**.

## Linker

Multi-file projects are assembled separately and linked with **vlink**, from the
same author as vasm and under the same licence terms (unmodified redistribution,
non-commercial use). Release archives do not include it — a source build needs it
installed separately if you use more than one source file:

```
http://sun.hasenbraten.de/vlink/
```

It builds with plain `make`. A single-file project needs no linker at all.

## Emulator embedding

On Linux, `PiST` can run the emulator's display **inside the IDE** instead of in a
separate window: **View ▸ Embed emulator display**. The mechanism is X11
reparenting — the emulator attaches its own window into a panel in the IDE — so
it works on X11 and, under Wayland, through XWayland. The preference is
remembered. Where reparenting is not possible (Wayland without XWayland, and for
now macOS and Windows), the option is unavailable and the emulator always runs as
a separate window, which remains the default everywhere.



`PiST` does **not** ship original Atari TOS ROMs: they remain copyrighted, so you must supply your
own. [EmuTOS](https://emutos.sourceforge.net/) is a free, GPL-licensed alternative that works well.

ROMs are discovered in the standard Hatari data locations, next to the emulator executable, or in
the directory named by the `PIST_TOS_DIR` environment variable:

```sh
PIST_TOS_DIR=~/atari/roms ./build/pist
```

**TOS 1.04 or later is required.** Hatari cannot autostart a program from a GEMDOS hard disk on
older TOS versions, so the run-and-debug workflow depends on it. `PiST` reads the version field from
the ROM image's header — the same field Hatari itself reads — and tells you if the image you have is
too old. If the version cannot be determined, it asks before running, because a too-old ROM
otherwise fails *silently*: the emulator boots, the program never starts, and debugging never
attaches.

## Remote control (driving the IDE from a script or an AI agent)

`PiST` can be driven from another program — a build script, a test harness, or an
AI agent — over a small text protocol, instead of by synthesising keyboard and
mouse input (brittle) or by reading the screen (worse). It is the same idea as
the control socket the emulator itself exposes, and PiST speaks both ends of
that arrangement.

The server is **off by default** and listens on localhost only; an IDE that opens
a network port unasked would be a surprise. Enable it with a flag or an
environment variable:

```sh
./pist --control-port 9999 your-program.s
PIST_CONTROL_PORT=9999 ./pist your-program.s
```

Connect and send one command per line. Replies are a single line (`ok` or `error
<message>`) or, for queries that return text, a block that ends with a line
containing only `.`:

```
open <path>        open a source file
build              assemble; the reply arrives when the build finishes
run                build and start the emulator; the reply arrives when it is running
stop               stop the emulator session
step / stepover / continue
breakpoint <n>     toggle a breakpoint at source line n
watchpoint <addr>  break when the value at an address changes
setreg <name> <value>   set a register (while stopped)
setmem <addr> <value>  write a memory byte (while stopped)
cmd <command>      run an arbitrary Hatari debugger command (block reply)
screenshot <file>  save the window as a PNG (default /tmp/pist-screenshot.png)
console            the build & debug console text (block reply)
state              registers and PC (block reply)
help               list the commands
quit               close the IDE
```

`build` and `run` answer only once the work is actually done, so a script does
not have to poll: `run` returning `ok` means the emulator session is up.

A minimal client in Python:

```python
import socket
s = socket.create_connection(("127.0.0.1", 9999))
def cmd(line, block=False):
    s.sendall((line + "\n").encode())
    if not block:
        return s.recv(4096).decode().strip()
    out = b""
    while not out.endswith(b"\n.\n"):
        out += s.recv(4096)
    return out.decode()

print(cmd("run"))                       # ok, once the session is up
cmd("screenshot /tmp/pist.png")         # save the window (including the embedded display)
print(cmd("state", block=True))         # registers
```

There is no authentication, so the socket is bound to localhost and nothing else;
do not forward or expose it. It is a debugging and automation aid, not a general
IPC mechanism. The `screenshot` command raises the window first, so it reflects
what is actually on screen.

## Licence

`PiST` is free software under the **GNU General Public License, version 2 or later**
(see [LICENSE](LICENSE)). Contributions are welcome under the same terms.

Bundled or invoked third-party components keep their own licences. In particular `vasm` is *not*
free software (it permits unmodified, non-commercial redistribution, which is why the IDE never
patches it), and Qt is used under the LGPL. The Hatari bundled in the Linux AppImage is the
[hrdb-main fork](https://github.com/tattlemuss/hatari) (upstream 2.6.1 plus the remote-debug
listener PiST's HRDB transport uses), GPL-2.0-or-later, redistributed unmodified from a
checksum-pinned commit tarball, and the GNU Readline (GPL-3.0-or-later) that build links travels
with it. See [docs/PLAN.md](docs/PLAN.md) §7 for the full breakdown and the obligations that
follow.

## Known limitations

Stated plainly, because an early release should not imply more than it does:
- **The emulator integration has only been exercised on Linux.** CI builds and
  tests on Windows and macOS, and the path handling, tool discovery and install
  steps are verified there — but neither runner runs the emulator suite: Windows
  has no Hatari package for MSYS2, and the macOS Hatari build produces an
  application bundle whose binary does not run standalone. Linux CI builds the
  pinned Hatari 2.6.1 *and* the hrdb-main fork, and exercises assembling and
  debugging against both transports, so bug reports from real Windows or macOS
  machines remain genuinely useful.
- **On stock Hatari, pause, changing breakpoints while running, and swapping disks at runtime
  do not work on Windows.** Hatari compiles its control channel only on POSIX systems.
  Breaking at entry, on exceptions, and at source-line breakpoints all work. The bundled
  emulator (the hrdb-main fork, in the Linux AppImage) is not affected; on Windows a
  user-installed [hrdb-main Hatari](https://github.com/tattlemuss/hatari) build gets pause and
  live breakpoints back (auto-detected). See [docs/FUTURE.md](docs/FUTURE.md) for the upstream
  fix that would cover the rest.
- **No installers** — releases are an AppImage on Linux and tarballs/zips elsewhere,
  not deb/RPM/MSI/dmg.
- **Hatari is bundled only in the Linux AppImage.** That copy is the hrdb-main fork
  (upstream 2.6.1 plus the remote-debug listener), redistributed unmodified from a
  checksum-pinned commit tarball, so it debugs with nothing else installed — over HRDB. The macOS
  and Windows archives do not include it (and neither does a source build), so
  there the emulator must be installed separately — and no distribution package
  will do: Ubuntu 22.04 ships 2.3.1 and 24.04 ships 2.4.1, whose truncated
  debugger responses break source-line debugging, while the IDE is developed and
  verified against 2.6.1.
- Multi-file projects are supported through the linker (add sources in Project
  Settings; needs `vlink`, which is not bundled with the source build).
- The interface is functional rather than polished.

## Documentation

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — a guide to the codebase: the module map, the
  build/run/debug flows, the hard-won invariants, and how to extend it. Start here to change code.
- **[docs/PLAN.md](docs/PLAN.md)** — the design document: verified findings, architecture
  rationale, the launcher rules, roadmap, risk register and licensing analysis
- **[docs/FUTURE.md](docs/FUTURE.md)** — deferred work, with the reasoning and the starting point for
  each item (notably: a portable emulator control channel, which is what Windows is missing)
- **[NOTICE](NOTICE)** — third-party components and the licence obligations that follow from them
