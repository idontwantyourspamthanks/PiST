# Future enhancements

Ideas that are understood well enough to describe but deliberately not built yet. Each entry
records *why* it isn't done, so a future decision has the context rather than just the idea.

---

## 1. A portable emulator control channel (upstream `--control-socket` on Windows)

**Status:** not started. **Blocks:** Pause, live breakpoint editing, and runtime disk swapping on
Windows.

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

- It requires carrying a **Hatari fork**, with rebase duty against every upstream release. The
  current design deliberately depends on stock Hatari, which keeps the integration surface small and
  the licence boundary clean.
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

## 3. DSP source-level debugging

**Status:** out of scope. Upstream's debugger drives the Falcon DSP with its own commands
(`dspreg`, `dspmemdump`, `dspbreak`, ...), and the ASM IDE surfaces those, but no portable
source-level DSP debugging exists: the dgis GDB stub has no DSP support at all, and GDB is
m68k-only. Anything deeper means going through HRDB or writing Falcon-specific tooling.

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

**Status:** partly delivered. Release artefacts already carry the assembler and EmuTOS, and the
Linux AppImage bundles Hatari 2.6.1 too. First-run setup and installers are still open.

Because `PiST` is free software, vasm's redistribution terms permit bundling it **unmodified** for
non-commercial use, and EmuTOS can ship as the default ROM — so a one-click install is legally
achievable. The plan (`docs/PLAN.md` §7) is to keep the repository free of non-free binaries and to
bundle everything a release artefact can carry: the Linux AppImage now does, Hatari included, while
the macOS and Windows archives still leave the emulator to the user (there is no MSYS2 Hatari
package for Windows, and no distribution package is a usable version — Ubuntu 22.04 ships 2.3.1 and
24.04 ships 2.4.1, against the 2.6.1 the project is verified on). A source build bundles none of it.
Original TOS ROMs stay user-supplied, always.

The pieces that do not exist yet: a first-run setup flow, a checksum-pinned download for the
platforms that do not bundle a tool, per-platform installers, and the dependency-notice generation
that the LGPL Qt, BSD Capstone and bundled Hatari/Readline obligations require.

---

## 6. Debug surface depth: editing, multiple panes, history, step-back, pause hint

**Status:** not started. Natural next layer on the debug views that exist now.

- **Register and memory editing.** Registers and the memory view are read-only today; an assembly
  developer wants to poke a value and keep going. Hatari's debugger supports it (`r d0 <val>`,
  `memwrite`), so this is UI work, not a transport problem. Needs care around when writes are legal
  (stopped only) and how an edit invalidates the disassembly/memory views.
- **Multiple memory panes.** One memory view today; watching two regions at once (a struct and the
  hardware registers, say) is a common need. Mostly a matter of letting the dump channels be
  addressed per-pane rather than one shared memoryDumpReady.
- **PC history and step-back.** A short ring of recent PCs, and stepping backward. Hatari has no
  reverse execution, so step-back would be reconstructive (restart + replay to just before) or a
  recorded history to navigate, not true reverse.
- **Pause hint on the embedded display.** When the debugger is stopped the embedded panel renders
  no frames and shows whatever was last drawn (or black), which reads as a freeze. A visible
  "paused" affordance would stop it being mistaken for a crash.

---

## 7. Panel aesthetics: tabs and movable panels

**Status:** delivered. Every dock is movable, floatable and closable, docks nest and tab within an
area, and the whole arrangement persists across runs (QSettings saveState/restoreState, round-trip
pinned by tst_gui::dockLayoutPersistsAcrossRestart). The default arrangement groups the debug views
(Registers/Disassembly/Stack/Hardware/Breakpoints) into one tabbed panel with the Emulator display
prominent above it, and Output/Memory tabbed at the bottom. The View menu lists every dock for
show/hide and has a Reset layout action. Remaining polish would be layout presets and a nicer
first-run default size balance, not the mechanism itself.

---

## 8. Emulator window resize correctness

**Status:** open bug, reported by a user. The embedded display does not track the dock cleanly: the
video stays the same size, so shrinking the dock pushes it out of view and expanding leaves ghost
trails. The fit-on-resize path (resize the reparented SDL window to fill the container, letting
Hatari rescale) needs to actually track the container and force a clean repaint, with no clipping
on shrink and no artifacts on grow.

---

## 9. LLM integration: chat sidebar, autocomplete, suggestions

**Status:** roadmap, deliberately unstarted. An agent-facing assistant built into the IDE: a chat
sidebar, inline autocomplete, and edit/refactor suggestions, against a configurable **endpoint and
API key** (so the user picks the provider/model rather than the IDE hard-coding one). Fits the
existing remote-control surface (the assistant can drive the IDE through it) and the project's
"hooks for agents" direction. Needs a settings page for endpoint/key/model, a sidebar panel, and a
clear line on what context is sent (open file, project, build errors) and what is not.
