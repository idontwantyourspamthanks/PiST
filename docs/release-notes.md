<!--
# PiST — release notes

This file is the body of every GitHub release: `.github/workflows/release.yml`
publishes it via `body_path`, so the text is reviewed in-tree and cannot drift
from what the workflow and README actually do. Edit it here, not in the
workflow. Maintainer guidance lives in this comment so it does not ship in the
published body.

The "What's new" section covers only the version being released. Do not leave
older "What's new" sections in the file: the workflow publishes this whole
file, so they would appear on the new release. Previous notes stay in git
history. The download and install text below the changelog stays; it describes
the archives, not a past version.
-->

PiST — an IDE for Atari ST assembly development.

## What's new in 0.10.0

0.10.0 is the release that makes PiST explain itself: a scriptable debugger, a reference with a
worked example for every instruction and TOS call, a project-wide search, completion and a symbol
palette in the editor, watchpoints that survive a rebuild, and a demo project that is the tour of
all of it.

### New

- **Scripted stop hooks.** A Lua script runs at every debugger stop with a small API —
  `readmem`, `writemem`, `readreg`, `writereg`, `sym`, `log`, `continue` — so a harness, a trainer
  or a logging pass needs no rebuild. Attach one to the project (Run ▸ Stop hooks…) or to a single
  breakpoint; agents get the `hook` remote verb and `pist_hook`, and a `hook` event when a script
  runs. `sym()` resolves a label through the run's own bases, so a script survives the relocation
  GEMDOS performs on every launch.
- **Reference authoring.** Every one of the 123 instructions and 111 TOS calls now carries a worked
  `Example:` and a one-line `Typical use:`, shown in the dock that follows the cursor. The examples
  are assembled by vasm in the test suite, so the reference cannot drift from the assembler.
- **Find in Files.** `Ctrl+Shift+F` searches the whole project and lists hits grouped by file with
  their spans; F4 and Shift+F4 tour them. The scan is the editor's own, so both count and match
  identically.
- **Completion, the symbol palette and the hover card.** The editor completes mnemonics, directives
  and the build's labels and equates from one list shared with the highlighter; `Tab` asks for it on
  a word, `Ctrl+Space` from anywhere. `Ctrl+Shift+O` is a Go to Symbol jump list, and hovering a
  mnemonic, a TOS call or a label shows the reference's facts — or nothing at all where nothing
  honest can be said.
- **Watchpoint symbols.** A watchpoint can be named for a label (`counter.l`) and is resolved again
  on every run, because GEMDOS picks the load address afresh each time. The remote verbs are the set
  and clear pair `watchpoint <a|l>` and `clearwatchpoint [a|l]`, and the MCP tools follow.
- **Editing a floppy image's own contents.** In the Disk A and Disk B panes: Delete…, Rename… and
  New Folder…, each rewriting the image through the same confirm-and-rebuild gate the transfers use.
  A name the disk already shows is refused rather than numbered.
- **A welcome gallery and reworked dialogs.** The workspace opens empty with a gallery — recent files
  and demo projects on the left, the machine drawn behind — and New File, New Image, Project
  Settings and Setup were rebuilt around cards and a short heading.
- **The demo is the tour.** `demo/hello.s` is a complete GEMDOS program written so each of the above
  has something to bite on: labels and equates to complete, all three traps to hover, a word to watch
  by name, and a hook script that logs every stop. Open it from the toolbar's Demo button or
  File ▸ Open Demo Project — or do nothing: a first run with nothing to reopen opens it for you.

### Fixed

- The completion list is drawn inside the editor rather than in a popup window, so it appears on
  Wayland as it does everywhere else, and `Tab` on a word opens it.
- Completion offered nothing in a project that had never been built: the vocabulary is static and is
  now available from startup, with the build's symbols added on top.
- A demo doorway opened the `.pistproject` file as text instead of the program beside it.

### Worth knowing

- The demo files ship with every archive (`share/pist/demo`), and the welcome gallery finds them
  beside an installed binary as well as in a source checkout.
- An existing profile keeps its saved layout; the new default arrangement — Find in Files tabbed
  beside Project files — applies to a fresh profile.
- `docs/qa-0.10.0.md` in the source repository is a by-hand checklist for everything above: what to
  activate, what to look for, what to expect.

### Linux

Nothing else to install: assemble, run and debug straight away.

The AppImage — `PiST-<version>-x86_64.AppImage` — is self-contained: Qt, the
assembler, the linker, the emulator and the ROM are all inside it. The shell
glob below is deliberate: these notes are published verbatim as the release
body, and a literal file name here drifted from the artifact a release actually
shipped once already.

```sh
chmod +x PiST-*-x86_64.AppImage
./PiST-*-x86_64.AppImage your-program.s
```

`.deb` and `.rpm` packages carry the same content for those who prefer the
package manager. They are also the route that integrates with the desktop: a
menu entry, the icons and the AppStream metadata a software centre reads the
name, author and licence from. Everything they bundle lives in a private
`/usr/lib/pist/`, with only `pist` and `pist-mcp` linked into `/usr/bin`, so
none of it can be picked up by other applications — and the bundled Hatari,
vasm and vlink cannot shadow a user's own. An AppImage does not add itself to
the application menu and cannot; that integration is AppImageLauncher's or
`appimaged`'s job, or install a package.

Everything is built on Ubuntu 22.04, so it needs glibc 2.35 or newer (Ubuntu
22.04+, Debian 12+, Fedora 36+, and anything more recent).

### Windows and macOS

Windows: the `.zip` or the `.msi` — both include the emulator, and both carry
the Visual C++ runtime beside the executable, so no redistributable install is
needed. macOS: open the `.dmg` and drag PiST to Applications, or unpack the
`.tar.gz` — the app carries the emulator and its libraries, so there is nothing
else to install.

### What works

Write, assemble, run and debug: source-line and conditional breakpoints,
stepping, step-over, registers, a memory viewer, and labelled disassembly, with
the execution point marked in the editor and your caret left where you put it.
Multi-file projects assemble separately and link with the bundled vlink (add
sources in Project Settings).
Projects keep their settings — include paths, defines, target CPU, machine,
ROM, RAM and disk images — in a small JSON file beside the source.

The emulator's display, sound and input live in PiST's own panel (with the
bundled emulator): click the panel to grab the keyboard and mouse, F12 to
release.

### Known limitations
- **CI exercises the emulator integration on Linux and macOS** (both build the
  hatari-pist fork from source and run the emulator suites against both
  transports). Windows builds and unit-tests only: the official Windows Hatari
  is a GUI-subsystem binary whose debugger never answers over pipes, so the
  bundled Windows emulator — the same fork, built with MSYS2 ucrt64 — is
  **experimental**; reports from real Windows machines are particularly
  welcome.
- **A user-installed *stock* Hatari** (a source build supplies its own; every
  archive bundles the fork) has no media channel, so PiST shows the emulator in
  a separate window instead of the panel; on Windows it also cannot pause,
  change breakpoints while running, or swap disks at runtime, because Hatari
  compiles its control channel only on POSIX systems. The bundled fork is
  unaffected, and breakpoints and stepping work regardless.
- The interface is functional, and 0.10.0 is its first design pass rather than a finished one.

Run `pist --diagnose` to see which assembler, linker, emulator and ROMs PiST
can find, and the paths it searched.
