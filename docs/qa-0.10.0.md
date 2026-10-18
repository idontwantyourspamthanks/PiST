# Verifying 0.10.0 by hand

*(File: `docs/qa-0.10.0.md`.)*

A checklist for someone who wrote the 0.10.0 features and now wants to check them in a live session.
One section per feature, each with **Activate**, **Look for**, **Expect** and **Not yet / by
design**. Every menu path, shortcut, verb, tool name and expected message below was read from the
source; where the feature sheet or the README reads differently from the code, the code is quoted
and the difference is named in one sentence.

---

## Preamble: what a session needs

| Need | Why |
|---|---|
| **Hatari**, ideally the `hatari-pist` fork | runs the built `.PRG`; the media channel that replaces window embedding lives in the fork. `./run.sh` builds it from `../hatari-pist` when it is absent, or honours `PIST_HATARI=<binary>`. |
| **A TOS ROM** | Hatari will not boot without one, and the debug transport produces no stop without a running machine. `PIST_TOS_DIR=<dir>` points the tooling at a directory of ROMs; the project setting does the same per session. |
| **`vasmm68k_mot` on the `PATH`** | assembles the project. The symbol table and line map it emits are what features 2, 3, 4 and 5 lean on, so a debug session that cannot build is not worth starting. |
| **A real display for `./run.sh`** | the application itself. The automated suites do not need one. |

Build and run:

```sh
./run.sh                 # builds if needed, then opens demo/hello.s
./run.sh path/to/x.s     # opens the given source instead
```

Tests: build first, then run the suites offscreen. **Every test run must carry
`QT_QPA_PLATFORM=offscreen`** — a test that opens a modal dialog is a real window on a desktop
session and hangs the run until someone clicks it away.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

---

## 1. Scripted stop hooks

A Lua script runs at every debugger stop, with the machine held stopped: it can read and write memory
and registers, log to the console and decide to continue.

### Activate

- `Run ▸ Stop hooks…` (the action is `stopHooksAction`, titled `Stop &hooks…`).
- In the dialog: tick `Run the script at every stop`, then point `Script:` at a `.lua` file or press
  `New script…`. `New script…` writes `<source>.hooks.lua` beside the current source, opens it in a
  tab and switches the hook on; `Browse…` picks an existing file.
- The same setting is the `hooks` object in `.pistproject` (`scriptPath`, `enabled`, `autoContinue`).
- One breakpoint can override the project's script: right-click the line in the gutter and choose
  `Stop-hook script…`. An empty path there means the project's hook.
- Agent side: the remote verb `hook` answers a JSON block, the MCP tool `pist_hook` reports the same
  record in `structuredContent`, and a `watch` client receives a `hook` event after each `stopped`
  when a script ran.

### Look for

- `[hook]` lines in the console dock (no `> ` echo — a script typed nothing).
- The dialog's `Last run:` line: `Last run: none yet this session.` before any run, then the run's
  own sentence.
- The `.lua` tab painted with Lua keywords, `--`/`--[[ ]]` comments and `[[ ]]` strings, rather than
  the m68k rules.

### Expect

The script runs at every published stop, including the entry stop and a single step. `continue()`
(or `resume()`) is the only thing that asks to resume; with `Let the script continue without asking`
(`hooks.autoContinue`) off, that ask is logged
(`the script asked to continue; hooks.autoContinue is off, so the machine stays stopped`) and the
machine stays stopped. Inside the script:

| call | behaviour |
|---|---|
| `readmem(addr, len)` | a byte-exact Lua string; `len < 1` or `len > 4096` raises |
| `readreg(name)` | the stop's own register when it is valid, otherwise a typed read |
| `writereg(name, value)` | writable: `d0`–`d7`, `a0`–`a7`, `pc`, `sr`; `usp`/`isp` are readable but a write is refused |
| `writemem(addr, value)` | one byte through the typed intent; the runner waits for its completion before any continue |
| `sym(name)` | the address the label has this run; raises on an unknown name, an equate or a name with no source position — never returns 0 |
| `log(msg)` / `print(msg)` | one `[hook] ` line in the console |
| `continue()` / `resume()` | recorded, then issued after the script returns and its queue is empty |

A script error prints `[hook] <file>:<line>: <message>` and leaves the machine stopped. A file that
cannot be read logs one refusal, runs nothing and publishes no `hook` event. A runaway script is cut
off at the budget and reported as a timeout. Two globals describe the stop: `reason` is
`"breakpoint"`, `"step"` or `"unknown"`, and `at` is the fired breakpoint's `"file:line"` or `nil`
(a pause, an exception and a watchpoint all report `"unknown"`). The `hook` event's status token is
`ok`, `timeout`, `error` or `norun`, and `message` (free text) comes last so everything after
`message=` belongs to it.

### Not yet / by design

One script per project: no list of scripts, no `require`, no modules, no shared cache. No file,
network or process API — Lua's `io`, `os`, `debug` and `package` are never opened. The hook fires
only at stops, never at build completion, session end or the running edge, and it is not a script
debugger (an error names file and line; there is no stepping through a script). A hook that is off,
or has no script configured, runs nothing **and publishes nothing** — a skip is not a run, which is
what `last.ran` false and the absence of a `hook` event both mean.

---

## 2. Reference authoring

Every entry of the instruction and TOS-call reference carries a worked example and a typical-use
line, shown in the dock that follows the cursor.

### Activate

- Put the caret inside an instruction, or on a `trap #1`/`#13`/`#14` line feeding a call; the
  reference dock follows the caret.
- The `Open the instruction reference` strip button raises the same dock.

### Look for

The detail pane below the entry list:

- an instruction shows `Flags: …`, then the typical-use sentence, then the worked example;
- an OS call shows the prototype, `d0: …`, `Stack: … bytes` (with `Available: …` when known), the
  typical-use sentence, then the example, and `Calling with: …` only while the row the arguments were
  resolved for is the selected one.

The example keeps the indentation it was authored with, so it reads as it would in a source file.

### Expect

Every entry of both tables carries a typical use and a worked example, and the gate is a test rather
than a promise. `tst_reference` requires, of every entry: a non-empty summary, example and typical
use; every example *assembles* under `vasmm68k_mot -quiet -m68060 -Ftos` inside the wrapper the test
documents (`start`, `msg` and `buffer` are the only context an example may assume); an instruction's
example starts with its own mnemonic and a call's makes that call's own trap; a typical use is one
line of at most 120 characters ending in a full stop. 123 instructions and 111 TOS calls are
authored; lookups resolve `addq.l`, `ADDQ` and `move.w` alike, and every entry resolves from its own
spelling.

### Not yet / by design

The dock prints no `Example:` or `Typical use:` headings. The feature sheet names those fields, but
the code stores `example` and `typicalUse` and renders them as plain lines, the sentence above the
fragment, so no such label is on screen. The truth of the prose is not machine-checked: the gate
asserts the shape and that the examples assemble, not that a sentence about a flag effect or a call
is correct — that is the author's and a reviewer's job, and a test that pinned wording would be
re-pinned by whoever changed the wording. `Used in:` (which lessons use a mnemonic) was dropped with
the teaching-layer rethink and is neither shown nor asserted.

---

## 3. Find in Files

A project-wide search dock beside Problems, with F4/Shift+F4 touring the hits.

### Activate

- `Search ▸ Find in Files…` (`Ctrl+Shift+F`), or the `Find in Files` dock tabbed beside `Problems`.
- Type into the query. `Aa` is `Match case`; `Word` is `Match whole words only`. The defaults are
  case-insensitive and not whole-word — the same defaults as the editor's find bar, because the scan
  is the same one.

### Look for

- Results grouped by file: a file row reading `N hit(s)`, and a row per hit carrying the line number
  and the line itself with the match highlighted.
- The status line: `N hit(s) in M file(s) searched`, with ` — more follow` when the 2000-hit cap
  stopped the walk and `· K file(s) skipped` when files were left out.

### Expect

Activating a hit (double-click, or Return) opens the file, goes to the line and selects exactly the
match's span. `F4` and `Shift+F4` tour the hits — wrapping — while the dock's list or query has
focus. The walk skips executables, objects, disk and still-image formats, ROM images and vasm
listings by suffix; a NUL byte settles anything whose name says nothing; hidden directories are not
walked; a file too large to be source is left alone. Typing a new query cancels the walk in progress,
so a re-run leaves nothing of the first behind. The header states hits, files *searched* and files
skipped, so "1 hit(s) in 401 file(s) searched" reads as the honest sentence it is.

### Not yet / by design

F4 belongs to this dock only while the dock has focus. The README reads as though F4 always tours the
results; the code claims the key in an event filter on the dock's own list and query, so with the
editor focused `F4` remains `Next Diagnostic` — a second shortcut on one key would make Qt fire
neither. The walk is chunked on the GUI thread, not threaded, so a very large tree is *responsive*
rather than parallel. Past the cap, the reported total is a floor, not a count.

---

## 4. Completion, symbol palette, hover card

The candidate list drawn inside the editor under the caret, a Go to Symbol palette, and a hover card.

### Activate

- In the editor: `Ctrl+Space` asks for the list from anywhere; `Tab` on a word asks for it too; a
  further `Tab` takes the highlighted candidate.
- `Ctrl+Shift+O`, or `Search ▸ Go to Symbol…`, opens the `Go to Symbol` palette.
- Hover a word with the pointer.

### Look for

- The candidate list drawn *inside* the editor, under the caret — a child of the viewport, not a
  separate window that the compositor could place elsewhere.
- The palette's `Symbol` / `Kind` / `Defined at` columns and its `Symbol name` filter.
- A tooltip at the pointer.

### Expect

The completion list follows the word as it grows, refiltering on each keystroke, and closes when the
word ends (a non-word key, a click, `Esc`). `Up`/`Down` walk it, wrapping; `Tab` and `Return` take
the highlighted candidate (or the first when the list was just opened). The candidates are mnemonics,
directives and the build's labels and equates — exactly the vocabulary the highlighter paints, from
one model. A word already typed in full offers nothing back.

The palette is non-modal and filters as you type; `Up`/`Down` walk the rows, `Return` opens the
definition's line. It works before a run: the definitions come from the build's symbol data and line
maps, not from live bases. An empty palette says why (`No symbols yet — build the project to list its
labels and equates.` or `No symbol matches “<filter>”.`).

The hover card, resolved the same way the dock resolves the caret, shows:

| over | card |
|---|---|
| a mnemonic | `mnemonic — summary`, then `Flags: …` when the reference knows them |
| a TOS call in context | `name — summary`, the prototype, and `Calling with: …` when arguments were resolved |
| a label or equate | `<word> — defined at <file>:<line>` |

### Not yet / by design

Completion offers nothing inside a comment (a comment is prose), at the start of a line `Tab` still
indents, and a word the vocabulary does not know offers nothing — the list is not a dump of every
word. The hover refuses to guess: a directive, an extra mnemonic the reference does not carry, and an
unknown word produce **no card at all**, and the event is not swallowed, so a widget's own tooltip
still works. The palette lists only symbols with a definition, because a row that cannot jump is
noise — a symbol with no source position (a command-line define) still completes by name, with no
file or line, and is not offered as a row to jump to.

---

## 5. Watchpoint symbols

A watchpoint may be named for a label and is resolved again on every run, so it survives the load
address GEMDOS picks afresh each time.

### Activate

- `Run ▸ Add watchpoint…` opens a dialog asking for `Address or label to watch, with optional
  .b/.w/.l width (e.g. counter.l, $12345.w):`.
- Remotely: `watchpoint <a|l>` sets one (optional `.b`/`.w`/`.l`); `clearwatchpoint [a|l]` clears the
  one the address or label names, or every watchpoint when the argument is empty.
- MCP: `pist_watchpoint` takes an `address` or a `label`; `pist_clearwatchpoint` is marked
  destructive.

### Look for

- A `[watchpoint] watch counter.l` line in the console.
- A row in the Breakpoints panel whose state column is first `pending (run to resolve)` and then
  `armed` (or the resolved address in hex).
- Remote replies: `ok; watch counter.l — resolved at the next stop` before the bases are live,
  `ok; watch counter.l at $…` once they are, and `ok; cleared N` from `clearwatchpoint`.

### Expect

A watchpoint asked for by name stores the *name* and is resolved again on every arm through the same
rule a breakpoint and a script's `sym()` use; its address is never written back into the model, so a
stale address from last run cannot reappear. A name whose bases are not live yet is **pending**, not
refused: it arms at the stop that reports the program's bases, exactly like a source breakpoint. A
label that exists wins over a token that merely looks like hex, so `watch deadbeef` watches the label
when the program has one. A name that resolves to an equate, to a symbol with no source position, or
to one that emits nothing (and nothing after it does either) is refused naming it. `clearwatchpoint`
of a name that matches none replies `error No watchpoint matches '<x>'.`, never a quiet `ok`.

### Not yet / by design

A watchpoint fires only on a *change*: writing the same value again, or a read, does not trigger it.
Hatari has no data watchpoints, so a watch is armed as the self-inequality conditional breakpoint
`b ($addr).w ! ($addr).w`. The width suffix owns a trailing `.b`/`.w`/`.l`, so a label that itself
ends in one of those cannot be named here; every other spelling survives. The refusal sentences
differ by reader — the arm-time log reads `[watchpoints] watch counter.l: …`, while the model and
dialog phrase their own — and the wording itself is presentation, pinned by no test.

---

## 6. Floppy-pane edits

Delete, Rename and New Folder inside a mounted disk image, in the file browser's Disk A / Disk B
panes.

### Activate

Right-click inside the `Disk A` or `Disk B` pane; the menu is `Copy` / `Cut` / `Paste`, then
`Delete…` / `Rename…` / `New Folder…`.

### Look for

- Delete asks `Delete <entry> from the disk?` — a set is named by its count, `Delete N entries from
  the disk?`.
- Rename asks `New name:`; New Folder asks `Folder name:`.
- Afterwards the row in the pane has changed and the image on disk agrees (a re-list shows the new
  state).

### Expect

`Delete…` removes the selected entry, a chosen folder taking its whole subtree. `Rename…` reads the
entry's bytes out and re-adds them under the new root in the *same* `updateImage` call that drops the
old paths — one atomic rebuild, so a folder and its contents move together and a failure leaves the
disk as it was. `New Folder…` creates an empty directory in the directory that was clicked.
`Rename…` is enabled only when exactly one entry is selected (renaming a set is not something a pane
can mean); Delete and Rename need a selection, New Folder needs only a disk. A name already shown on
the disk is refused with `<name> is already on this disk.` and the image left untouched — a paste
numbers a collision, but a rename must not silently become `NAME(2)`. An empty name, or one
containing `/`, is refused by the dialog before the writer is asked. A disk that is not PiST's own
canonical 720 KiB layout asks before it is rebuilt.

### Not yet / by design

The collision test compares the spelling the pane shows. An entry whose *stored* 8.3 name field
differs from its displayed one (a real disk's lower-case or accented field) can still collide, and
the writer then numbers the addition — documented at the function rather than discovered later.

---

## Running the suites that cover the same ground

| Feature | Suites |
|---|---|
| §1 stop hooks | `tst_stophooks` (engine and runner, no emulator), `tst_gui` (the menu, the dialog, the starter), `tst_remotecontrol` (the `hook` verb), `tst_mcp` (the tool), `tst_emulatorhost` (a script against a real session) |
| §2 reference | `tst_reference` (the authoring gate and the assembly), `tst_oscall` (the trap scanner and the call table) |
| §3 Find in Files | `tst_find` (the shared scan, the walk, the cap and the skips), `tst_gui` (the dock) |
| §4 completion / palette / hover | `tst_completion` (vocabulary, ranking, resolver), `tst_gui` (the three surfaces) |
| §5 watchpoint symbols | `tst_debug` (the arm plan), `tst_gui` (the panel), `tst_remotecontrol` (the verbs), `tst_emulatorhost` (the label against a machine), `tst_mcp` (the tools) |
| §6 floppy residue | `tst_gui` (the three operations against a fixture image) |

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R stophooks --output-on-failure   # one suite
```

`tst_gui`, `tst_remotecontrol` and `tst_emulatorhost` run offscreen and `QSKIP` unless Hatari,
`vasmm68k_mot` and a TOS ROM are present — set `PIST_TOS_DIR=<dir>` and put the tools on `PATH`;
`PIST_REQUIRE_EMULATOR=1` turns a skip into a failure. Keep `QT_QPA_PLATFORM=offscreen` on every run.
