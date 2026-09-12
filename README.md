# PiST

**An IDE for Atari ST assembly development.**

*Program in ST* — an IDE for writing 68000 assembly for the Atari ST.

Write 68000 assembly, assemble it, run it in an emulator, and debug it — without leaving the
editor. `PiST` bundles the pieces that are otherwise scattered across a text editor, a build
script, a terminal, a debugger and an emulator, and presents them as one tool.

> **Status: early.** Phase 0 (the spike) is complete and working end to end. The IDE is usable for
> assembling, running and debugging a single-file program, but most of the interface described
> below does not exist yet. See [docs/PLAN.md](docs/PLAN.md) for the full design and roadmap.

---

## What it does

```
  write  ──▶  assemble  ──▶  run in Hatari  ──▶  debug
   .s         vasmm68k_mot        .prg            breakpoints, stepping,
                                                   registers, disassembly
```

- **Editor** with m68k Motorola-syntax highlighting, error markers and the current execution line
- **Project settings** — include paths, defines, target CPU, and the emulator's machine, ROM,
  monitor, RAM, hard disk and floppy images, saved beside the source in a small JSON file
- **Build** through `vasmm68k_mot`, with its diagnostics shown against the exact source line
- **Run** in [Hatari](https://www.hatari-emu.org/), launched with the project's settings
- **Debug** with breakpoints, single-step, step-over, registers, memory and labelled disassembly —
  and the editor following the program counter as you step

The goal is *batteries included*: the toolchain and emulator ship with the IDE where their licences
allow, so there is nothing to assemble by hand before writing your first line of code.

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
│         │           │ Build    │      │ Debug      │                 │
│         │           │ Service  │      │ Client     │                 │
│         │           └────┬─────┘      └─────┬──────┘                 │
│         │                │                  │                        │
│         │           ┌────┴─────┐      ┌─────┴──────┐                 │
│         └───────────│ LineMap  │      │ Emulator   │                 │
│           (PC↔line) │ (-L lst) │      │ Host       │                 │
│                     └──────────┘      └─────┬──────┘                 │
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

Hatari's debugger has no GDB stub and no structured protocol, so the IDE drives it through the
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

### Where the code lives

| Path | Contents |
|---|---|
| `src/editor/` | Editor widget and the m68k syntax highlighter |
| `src/build/` | `BuildService` (drives vasm) and `LineMap` (listing → line/address map) |
| `src/emu/` | `EmulatorHost`, session configuration, capability probing, ROM discovery |
| `src/ui/` | Main window and debug panels |
| `tests/` | Parser unit tests and emulator integration tests |
| `docs/` | Design documents |

## Building

Requirements: **CMake ≥ 3.21**, **Qt 6.5+** (Widgets and Network), a C++17 compiler.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the tests:

```sh
ctest --test-dir build --output-on-failure
```

There are two suites: parser tests (always run) and emulator integration tests, which skip
themselves unless Hatari, `vasmm68k_mot` and a TOS ROM are available.

### Installing

Download an archive from [Releases](../../releases). Each one contains PiST, the
`vasmm68k_mot` assembler, and an EmuTOS ROM, and unpacks ready to run:

```sh
tar xzf pist-*-linux-x86_64.tar.gz
./bundle/bin/pist your-program.s
```

You still need **Hatari** for the emulator: it is a large GPL dependency and is
not bundled. Everything else is included, so the IDE assembles and debugs out of
the box once Hatari is present.

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
| Pause a healthy running program | ✓ | ✗ | ✓ |
| Change breakpoints while running | ✓ | ✗ | ✓ |
| Swap disk images at runtime | ✓ | ✗ | ✓ |

The three gaps are all downstream of one thing: Hatari's control channel is compiled only on
POSIX systems (`HAVE_UNIX_DOMAIN_SOCKETS`), and it is the only way to command an *already running*
emulator. `PiST` detects this and omits the option rather than passing it and failing to start.
Everything else works everywhere, because debugger commands travel over stdin — the control socket
is starved whenever the debugger is stopped anyway, so it was never carrying the debug traffic.

Removing that gap is a small, well-understood upstream patch. It is written up, with the proposed
approach, in **[docs/FUTURE.md](docs/FUTURE.md)**.

## Emulator embedding

`PiST` embeds the emulator window where the platform allows it: X11, Windows and macOS all support
it, and Wayland is handled through XWayland. A detached-window mode is always available, and is the
only mode on Wayland without XWayland.



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

## Licence

`PiST` is free software under the **GNU General Public License, version 2 or later**
(see [LICENSE](LICENSE)). Contributions are welcome under the same terms.

Bundled or invoked third-party components keep their own licences. In particular `vasm` is *not*
free software (it permits unmodified, non-commercial redistribution, which is why the IDE never
patches it), and Qt is used under the LGPL. See [docs/PLAN.md](docs/PLAN.md) §7 for the full
breakdown and the obligations that follow.

## Documentation

- **[docs/PLAN.md](docs/PLAN.md)** — the design document: verified findings, architecture,
  the launcher rules, roadmap, risk register and licensing analysis
- **[docs/FUTURE.md](docs/FUTURE.md)** — deferred work, with the reasoning and the starting point for
  each item (notably: a portable emulator control channel, which is what Windows is missing)
- **[NOTICE](NOTICE)** — third-party components and the licence obligations that follow from them
