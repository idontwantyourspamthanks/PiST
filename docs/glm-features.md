# The next feature sheet

*(File: `docs/glm-features.md`.)*

The agreed next feature sheet, decided 2026-10-16. Where `docs/FUTURE.md` records
ideas and why each was deferred, this document records the ones that were then
**agreed for development**, with the same discipline: what exists in the tree
today (verified against the source, with paths), the design, and how to start.

One idea was examined and **binned** rather than agreed; it is recorded here so
the reasoning survives the conversation that binned it. Pointers in
`docs/FUTURE.md` direct its §9, §10 and the L4 remainder of §14 to this file.

The six agreed features, in the order they are spec'd below:

| # | Feature | Prepares for |
|---|---|---|
| 1 | Scripted stop hooks (runs at every debugger stop) | harnesses, trainers, dynamic analysis, agents |
| 2 | L4 reference authoring (`Example:` / `Used in:` / `Typical use`) | lessons, the reference dock, agents |
| 3 | Find in Files | multi-file projects, day-one expectations |
| 4 | Editor completion, symbol palette, hover card | editing speed, beginners |
| 5 | Watchpoint symbols | watchpoints that survive GEMDOS relocation |
| 6 | Floppy image residue: Delete / Rename / New Folder inside a disk | disk A/B being a real workspace |

---

## 0. Binned: an in-IDE assistant sidebar

**Status:** binned, deliberately. The idea was FUTURE §9: a chat sidebar with
inline autocomplete against a configurable endpoint and API key.

The MCP route won and should keep winning. The argument as recorded: most of
the people who will drive PiST with an agent — and most of the people who built
it — live in the terminal, not in this IDE's docks. `pist-mcp` already exposes
the full capability to whatever client *they* already run (Claude, Cursor,
anything speaking stdio MCP): structured JSON replies, pushed events on
stop/resume so nothing polls, resources, prompts, tool hints. An embedded chat
duplicates that customer and cannot win the comparison: the sidebar would be
the IDE's UI budget spent competing against clients that are someone else's
product. It also re-raises the exact questions §9 knew it had to answer first —
what context is sent where, key storage, vendor choice — for a surface that no
longer needs to exist.

What the IDE *should* keep doing is what it already does deliberately: keep the
remote verbs and MCP tools small, typed and rich enough that an external agent
is a first-class user. New capability (several entries below are exactly this)
should grow the verbs, not a chat widget. If a sidebar ever returns, this
section is the argument it has to beat.

---

## 1. Scripted stop hooks

**Status:** agreed; not started. Absorbs `docs/FUTURE.md` §10 (which this entry
supersedes; §10's analysis of what already exists was verified then and
re-verified here).

### What exists

Every ingredient is present; this is a glue feature, not plumbing:

- Debug state writes while stopped: registers and memory are editable from the
  views (`MainWindow::setRegister` / `MainWindow::setMemoryByte` → the backend's
  typed `writeRegister` / `writeMemoryByte` intents), gated on the stop state.
- The trigger: `EmulatorHost::stoppedChanged(true)` fires on every stop —
  breakpoints, watchpoints, stepping, pause.
- Breakpoint and watchpoint models are address-resolvable in advance
  (`src/debug/Breakpoint.cpp` `planBreakpoints()`, `src/debug/Watchpoint.h`).
- An external program can already script memory edits while a session is
  stopped, over the remote-control socket, but only by polling `state` to
  *notice* the stop — the protocol is request/response and has no push that says
  "run this now that we broke".

### The design

Let the user supply scripts that run each time the debugger stops, can inspect
and edit memory and registers, and can decide to continue. This is the
FCEUX/Mesen/BizHawk emulator-scripting model applied to PiST's debug loop: the
script lives outside the compiled program, so behaviour is extended without a
rebuild.

- **Language: Lua** (recorded as the choice in FUTURE §10's shape). Tiny,
  MIT-licensed, embeddable on all three platforms, and the standard the
  emulator-tools audience already knows. The zero-third-party alternative, Qt's
  own `QJSEngine`, stays the fallback if packaging fights back on any platform.
- **Trigger shape.** Named scripts on named breakpoints first, then a
  run-on-every-stop script. Overloading every stop with a script from day one
  makes a stray `continue()` dangerous.
- **API, kept small:** `readmem(addr, len)`, `writemem(addr, value)`,
  `readreg(name)`, `writereg(name, value)`, `continue()`, and `log(msg)` into
  the console dock. A script runs to completion while the transport is at its
  `> ` prompt, so its writes are ordinary debugger commands — legal exactly then.
- **The hard rule it must respect** is the channel split and prompt framing
  (AGENTS.md rules 2–4): scripts only ever run *after* a framed prompt, never
  against a running machine, and they must not queue debugger commands that
  outlive the stop (rule 4's resume-not-drop requirement is the precedent to
  follow for anything the script queues).

### How to start

A minimal v0 needs no new transport: on `stoppedChanged(true)`, evaluate the
configured script file with an API object backed by the existing
`setMemoryByte` / `setRegister` / `debugCommand` slots. Vendor Lua; document the
API in the README; ship one integration test that stops at a line and asserts a
scripted write landed (`tst_emulatorhost` is the pattern for a real-session
test, not a mock).

**The plan of record is [docs/stop-hooks-plan.md](stop-hooks-plan.md)**, and it
refines two points above. The hook fires on the *published* stop — after the
register batch, not on the raw `stoppedChanged(true)` event, where the state is
still a batch stale and the entry stop may still be printing its dump — and it
runs at steps too, which needed a flag of their own because `s`/`n` are not
running→stopped transitions. The engine is settled, and it is this entry's own
choice: **Lua 5.4.8**, pinned by checksum and linked statically. Both candidates
were measured first (the plan's L0): Qt's `QJSEngine` costs 8,770,048 bytes of
extra shipped libraries and can only be interrupted from a second thread, where
Lua's count hook stops a runaway script in-thread and adds ~345 KiB. The one Lua
cost this entry's fallback clause did not anticipate is bookkeeping rather than
packaging — a statically linked library is invisible to the artifact scan that
derives the shipped licence set, so its MIT notice is named explicitly and that
exception is written down.

---

## 2. L4 reference authoring

**Status:** agreed; the L0–L3 teaching layer is in the tree (see
`docs/tutorial-plan.md`, `lessons/*/lesson.pil`, `src/lessons/`). This is the
remainder FUTURE §14 left unstarted: `Example:` / `Used in:` / `Typical use`
across the whole reference.

### What exists

- The reference data is code, not external files: `src/editor/InstrRef.cpp`
  (the 68000 instruction table), `src/editor/OsCallRef.cpp` (GEMDOS / BIOS /
  XBIOS calls), surfaced by `src/ui/InstructionRefView.cpp` in a dock that
  follows the cursor and names the call a `trap #1` is making and with what.
- The entry types are deliberately terse: `InstructionInfo` carries `mnemonic`,
  `summary`, `flags` — and "the reference would rather say nothing than state a
  flag effect that is wrong" (`src/editor/InstrRef.h`). The L4 fields must keep
  that discipline.
- The lessons' own design rule applies here and is the reason to author
  carefully: **a claim that stops being true must fail a test rather than teach
  something wrong** (tutorial-plan §1.1).

### The design

Extend the entry types with three fields — `Example` (a minimal fragment,
labelled with the context it assumes), `Used in` (which lessons use it, linking
the reference to the teaching layer they live in), and `Typical use` (one
sentence of when). No LLM text, no markdown, done deliberately so it also serves
the remote/MCP surface.

Two disciplines make this trustworthy:

1. **Author in data, verified by test.** A new unit test walks every entry and
   asserts each `Example:` (a) exists for entries claimed to have one, (b)
   parses under the reference's own lookup rules — case-insensitive, size
   suffix ignored — and (c) where the fragment is self-contained, actually
   assembles with vasm in CI. That last check is the lesson principle applied to
   the reference: an example that stops being assemblable fails the suite rather
   than shipping as prose.
2. **Never state a wrong thing to be complete.** If a field's honest answer is
   "none of the lessons", the test says so — an empty `Used in` on an entry no
   lesson touches is a fact, not a gap.

### How to start

On `InstrRef.cpp` first (the 233 mnemonics), then `OsCallRef.cpp`; the detail
pane in `src/ui/InstructionRefView.cpp` renders the new fields. FUTURE §14's
"more `.pil` lessons" remains open and unblocked: lessons may now lean on
`Used in`, which is the cross-link that makes the reference and the teaching
layer one system.

---

## 3. Find in Files

**Status:** agreed; does not exist. Verified 2026-10-16: search today is
per-document only — the editor's find bar with Ctrl+F / Ctrl+H, F3 and
Shift+F3, match-case and whole-word options, and a hit counter that admits its
cap (past 2000 it reads "N of more than 2000" rather than counting on).

### The design

A dock in the Problems pane's group: a query line, a scope (the project
directory; per-file is what the editor already does), and results grouped by
file, line number, and the matching span highlighted — double-click opens at
the line, F4 / Shift+F4 tours them, consistent with the build-diagnostics that
already navigate against exact source lines.

Match the editor's find semantics exactly — same case-insensitivity default,
same whole-word option, same honest counter — because a tool that counts
differently in two places looks like it is lying in one of them. Skip binary
and generated inventory (`.pim`, `.prg`, disk images, ROMs) by extension, and
say in the results header what was skipped and how many files were searched —
the same honestly-capped style the counter above uses.

### How to start

A `FindInFiles` model + a worker that walks the project directory on a thread,
one file at a time, cancellable (a re-run while typing a new query must cancel
the previous one). Anchor the dock beside Problems (`src/ui/`), reuse the
editor's highlight and the F4-tour pattern from `BuildService`. Test with
`tst_gui` (offscreen): results appear for a fixture tree, the tour opens the
right file and line, cancel races are not visible to the user.

---

## 4. Editor completion, symbol palette, hover card

**Status:** agreed; the console has this and the editor does not — which is
backwards. Verified 2026-10-16:

- The **debugger console** (`src/ui/ConsoleInput.cpp`) has full `QCompleter`
  symbol completion with a popup and history — over the debugger's command and
  symbol models.
- The **editor** (`src/editor/CodeEditor.cpp`) has Ctrl+click to open an
  `include` or jump to a label, and the reference dock that follows the cursor
  — but no completion, no palette, no hover.
- The symbol table the console completes against is in-process already (the
  symbols dock lists every label and equate with its address once the program
  runs; `EmulatorHost::symbolsJson()` serves it over the remote protocol).

### The design

Three surfaces over the *same* model:

1. **Tab completion** in the editor: labels, equates, mnemonics and directives
   from one completion model built from the reference tables (which already
   resolve `addq.l`, `ADDQ` and `Move.w` identically — reuse
   `pist::instructionRef()`'s rules) plus the build's symbol table. Devpac
   syntax is the target; nothing completion offers should look, in a Devpac
   source, like a suggestion from another assembler.
2. **Symbol palette** (Ctrl+Shift+O): a filter-as-you-type jump list over the
   project's labels and equates — the same model, presented for navigation
   rather than completion. Live base addresses are not required for this one:
   definitions' file:line come from the build's symbol data and the line maps,
   so it works before the program ever runs.
3. **Hover card**: the same data the reference dock shows, as a tooltip at the
   cursor — a mnemonic gets `summary` + `flags`, a TOS call gets the
   call/argument name and meaning, a label gets its definition "file:line".
   The rule to keep: resolve to *nothing* rather than to a guess (the reference
   already refuses labels and directives; the hover must inherit that).

### How to start

The completion model first (editor + palette are two views of one model), then
hover. Reuse `ConsoleInput`'s completer wiring conventions — it was written
with the ownership and popup quirks already understood and documented in-place.

---

## 5. Watchpoint symbols

**Status:** agreed; current state verified against the source, and it is
exactly the gap it looked like.

### What exists

- `src/debug/Watchpoint.h`: a watchpoint *is* a raw `quint32 address` plus a
  width byte, rendered into Hatari's conditional-breakpoint form
  `b ($addr).w ! ($addr).w` — Hatari has no data watchpoints (upstream's `w` is
  compiled out), so the IDE arms change-tracking breakpoints and the debugger's
  change-notation evaluates as "fire when the value differs from its previous
  value". (Caveat worth remembering in any UI: the same value written again does
  not fire, and a read never does.)
- Breakpoints already know how to take a symbol: the remote
  `breakpoint <n|label>` verb resolves a label to its definition's file:line
  and toggles there, replying with where it landed and the address once live
  bases exist (`RemoteControl.cpp`, the `breakpoint` handler, via
  `EmulatorHost::toggleBreakpointAtLabel`).
- The symbol table that can resolve label → address is already in the
  debugger's hands (`symbols` verb, `EmulatorHost::symbolsJson()`), and
  breakpoints only arm after `info basepage` delivers the live bases
  (AGENTS.md rule 4 — `setLiveBases()` → `armBreakpoints()`).

### What is missing

A watchpoint cannot be named for what it watches. Every one of its users has to
already know the load address, which GEMDOS re-picks every run — for
breakpoints the file:line model and label forms exist precisely so nobody has
to; for watchpoints, the interface a *breakpoint* got, the watchpoint never
did.

### The design

- **Resolve at arm time, IDE-side.** The arm plan (`planBreakpoints()` already
  carries the arming discipline) expands a named watchpoint through the build's
  symbol table into the numeric address the `b (... ! ...)` command needs; the
  user-facing label keeps the symbol name (`watch counter.l`, not
  `watch $12596.l`).
- **Re-resolve every run.** Same reason breakpoints are file:line and never
  raw addresses: an address captured last run is wrong this run. Symbol
  watchpoints must be part of the re-arm after each entry stop, or they will be
  the most confidently-wrong panels in the IDE.
- **Refuse clearly.** A name that resolves to nothing, or to a non-address
  value, is an error naming the symbol — the honest-reply style the breakpoint
  verb already has ("ok; line %1 emits no code…").
- **Every interface gains the name:** Run ▸ Add watchpoint asks for
  `label|address`, the remote verbs grow `watchpoint <addr|label>` /
  `setbreakpoint`-style set/clear (never toggle) forms, and the matching MCP
  tools follow — this is one of the two entries on this sheet (with §3) that
  visibly grows the agent surface, which is the point of the sheet.

### How to start

In the watchpoint model: accept a name alongside the address, expose a
resolution step for the arm plan to call. `tst_debug` asserts the model +
resolution; `tst_remotecontrol` asserts the verb's reply shape against a
symbol-bearing program; the integration suite exercises it against a real
session like the existing breakpoint arming does.

---

## 6. Floppy image residue: Delete / Rename / New Folder inside a disk

**Status:** agreed for its residue only — the bulk of this feature is delivered
and is worth recording as verified, because the sheet owed it a check.

### What exists (verified 2026-10-16)

Files inside a Disk A / Disk B image are managed, end to end:

- The whole-file primitive is `floppy::updateImage(imagePath, additions,
  removals, error)` (`src/build/FloppyImage.cpp`), with `readFileRaw` /
  `listRaw` for reading. The writer stages a new image over the user's file, so
  a damaged archive is replaced, not corrupted further — and that rebuild is
  *silent* only for an image PiST already laid out (`looksLikeCanonical720k`'s
  720 KiB mkfs.fat check in `FileBrowser::confirmFloppyRewrite`); anything else
  is asked about first, because a rebuild discards whatever made it different.
- The three transfer directions are one algorithm: host → disk, disk → host,
  disk → disk (`src/build/FloppyTransfer.{h,cpp}` — copies, moves, drag-and-drop
  and the clipboard route all funnel through it, with the confirm/resync
  preamble sequenced once so a drive cannot change mid-question).
- **Edit in place:** a file opened out of a disk is saved back by *replace* —
  the entry exists on the disk, so the update passes the entry in `removals`
  as well as its new content in `additions` (`MainWindow.cpp`, the save path
  behind the floppy edit session).
- The browser pane's context menu offers Copy / Cut / Paste over disk entries;
  eject and change buttons behave like real drive latches rather than silently
  mutating a file Hatari is reading mid-run.

So the sheet's original phrasing — "add/edit/remove files inside an image" — was
already true. What does not exist is the *rest of the menu*: while a file sits
inside a disk, the pane cannot **Delete…** the entry, **Rename…** it, or create
a **New Folder** inside the image. The only way to drop an entry today is to
copy it out with remove-source (a move) or paste-over; and the only way to
organise a disk's layout is to organise the host folder before export.

### How to start

All three operations are `updateImage` calls in fancy hats: delete is the
`removals` argument alone (already proven by the save path); rename is a
read + remove + add under the new name, passing the original entry forward
via the field `updateImage` already carries for carried-forward entries;
New Folder is a directory item with no contents. Every one goes through
`floppy::Transfer`'s confirm/resync preamble — whose `confirmRewrite` is
exactly the `looksLikeCanonical720k` gate above — so the drive-latch semantics
already in `FileBrowser` stay honest, and a rewrite is asked for before it
happens, exactly as the transfers do.

Add the three actions to `FileBrowser::onFloppyContextMenu`, disabled when the
drive is empty or nothing is selected, and assert them in `tst_gui` offscreen:
fixture image, delete → row gone, rename → row re-named, folder → row present;
then `readFileRaw`/`listRaw` confirm the image's truth matches the pane's.
