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

**Status:** agreed; **L1, L2 and L3 delivered** on `feature/stop-hooks` (2026-10-17).
[docs/stop-hooks-plan.md](stop-hooks-plan.md) is the plan of record: the decisions (D1–D8), the
engine measurements, the agent surface and its one departure from this sheet, and the phase status.
Absorbs `docs/FUTURE.md` §10 (which this entry supersedes; §10's analysis of what already exists was
verified then and re-verified here).

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

**Status:** delivered 2026-10-17 on `feature/find-in-files`. The verification
below is what the tree looked like before it: search *was* per-document only,
which is the state this feature changed.

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

### What was built, and the decisions it took

- **The scan is shared, not copied.** `editor/FindScan.{h,cpp}` holds
  `FindOptions`, `kMaxMatches` (2000) and `findScan()`; the editor's find bar
  (`CodeEditor::collectMatches`) and the dock both call it now, so "the same
  semantics" is a property of the code rather than a promise. The whole-word
  rule moved there too — it was always a filter on Qt's match
  (`QTextDocument::FindCaseSensitively` is the only flag Qt is given), and it
  now filters for both callers.
- **The walk is chunked on this thread, not threaded** — a departure from the
  sketch above. No dock in the tree walks a directory off the GUI thread (the
  one `QThread` is a modal setup dialog that cannot be cancelled), and
  `Qt6::Concurrent` is not linked; a 10 ms tick budget keeps the window
  responsive, and cancelling then becomes a state change rather than a race —
  which is what makes "a re-run while typing a new query" obviously correct
  rather than merely tested. The hit cap stops the walk and the summary admits
  it, the same way the editor's counter does.
- **What it will not read, it counts.** `isSearchableFile()` is this feature's
  own question — "would a user search inside this?" — and deliberately *not*
  MainWindow's `suffixIsKnownBinary`, which answers a different one (is this a
  text document to open out of a disk image?, where `.s` and `.asm` are
  refusals, since assembly builds from the hard drive). Executables, objects,
  disk and still-image formats, ROM images and vasm listings are skipped by
  suffix; a NUL byte settles anything whose name says nothing; hidden
  directories are not walked; a file too large to be source is left alone. The
  header states hits, files *searched* and files skipped — "1 hit(s) in 401
  file(s) searched" is the honest sentence, and the first wording ("in 401
  files") read as though every one of them had a hit.
- **F4 is claimed, not bound twice.** F4/Shift+F4 already tour the Problems
  pane, and a second shortcut on one key makes Qt report "ambiguous shortcut
  overload" and fire *neither* — measured in a throwaway program, with the dock
  bound that way doing nothing when F4 was pressed in it. The dock instead
  claims the key (`ShortcutOverride`, then `KeyPress`, in an event filter on
  its list — the idiom `CodeEditor` uses for Esc and Enter) while it has focus;
  everywhere else F4 remains the diagnostics tour.
- **Navigation marks the match, not just the line.** `CodeEditor::selectSpan()`
  is the one new editor entry point: a row carries the match's column and
  length, and activating it opens the file, goes to the line and selects
  exactly that span.

### Tested

`tst_find` (new, no window and no emulator) pins the shared scan's counting
rules, its cap, and the walk's skip list, cap, summaries and cancellation
contract. `tst_gui` pins the dock against a fixture tree: rows grouped by file
with their line numbers, the status line's counts, F4 crossing into the other
file and selecting the match, F4 belonging to the diagnostics tour when the
editor has the focus, and a second query leaving nothing of the first behind.
Two bugs came out of writing them, both fixed rather than accommodated: a
negative `from` to `QString::lastIndexOf` means "from the end", which made the
first line of every file read as its last, and the status wording above.

---

## 4. Editor completion, symbol palette, hover card

**Status:** delivered 2026-10-17 on `feature/completion`. The verification below
is the state it was written against: the console had completion and the editor did
not, which is the asymmetry this feature removed.

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

### What was built, and the decisions it took

- **One vocabulary, shared with the highlighter.** `editor/Vocabulary.{h,cpp}`
  holds the two word lists — the mnemonics (the 68000 reference plus the
  68020+/FPU names vasm accepts and the reference deliberately leaves out) and
  the directives — and `AsmHighlighter` now builds *both* of its patterns from
  them. Completion therefore offers exactly what the editor paints. The
  directive list had existed only as a regex inside the highlighter, and a second
  hand-written copy is what drifted by nineteen entries the first time this was
  tried for mnemonics.
- **One candidate model.** `editor/CompletionModel` is `CompletionItem` (text,
  kind, definition, one line of detail) and the ranking: prefix matches before
  substring ones, case-insensitively, model order within each, a limit. The
  window builds the list once per build — the static vocabulary plus the build's
  labels and equates with their file:line — and pushes it into every editor's
  popup and into the palette. The hover asks the same list what a word *is*,
  which is what keeps the three surfaces from disagreeing about the assembler.
- **One cursor resolver.** `editor/CursorReference` is the dispatch the reference
  dock used to inline: an OS call in context wins, a trap whose function number
  is unknown is trap context, otherwise the word under the position. The dock and
  the hover card both call it, so a line cannot mean one thing in the panel and
  another in a tooltip. The dock's word now comes from `asmlex`'s token alphabet
  instead of a private loop, so a label keeps its underscore — a small fix the
  shared rule brought with it.
- **The popup is driven by hand, from `ConsoleInput`'s conventions.** QCompleter
  supplies the model, the popup and the filtering; the prefix, the keys and the
  placement are ours. Two quirks cost a measurement each: a completer attached
  with `setWidget()` to anything that is not a `QLineEdit` **hides its own popup
  on the next keystroke** — it cannot read the word being typed, decides the
  prefix is empty and hides — so its widget filter is removed; and its generic
  popup placement is the *widget's* bottom edge, so the popup is moved under the
  caret on every show.
- **The palette is a jump list, not a report.** **Ctrl+Shift+O** (Search ▸ Go to
  Symbol…) opens a non-modal list of the build's labels and equates with their
  definitions — only symbols with a definition, because a row that cannot jump is
  noise — filtered as you type. Enter takes the row and closes it, and the
  filter's Up/Down walk the list. It works before a run: the definitions come from
  the build, not from the live bases.
- **The hover refuses to guess.** The app-level event filter answers
  `QEvent::ToolTip` for an editor viewport from the resolver: a mnemonic gets the
  reference's summary and flags, an OS call its name, summary, prototype and
  arguments, a label its definition. A directive, an extra mnemonic the reference
  does not carry, and a word nothing knows produce **no card at all**, and the
  event is not swallowed, so a widget's own tooltip still works.
  `MainWindow::referenceCardAt()` is that card as a function — the handler's
  caller and the tests' — because a `QToolTip` label's text is not ours to drive
  (it keeps the previous text for a moment after `hideText()`; measured).

### Tested

`tst_completion` (new, no window and no emulator) pins the vocabulary's two
families (names, lowercase, no duplicates, the extras present), the ranking
(prefix before substring, case-insensitivity, the limit, the empty prefix) and
the resolver (an OS call wins; an unresolved trap is context; a word keeps `.`
and `_`; out of range is nothing). `tst_gui` pins all three surfaces against real
widgets: the popup offers the word at the caret and Tab/Return replace it, while
a comment offers nothing and a word already typed in full offers nothing back;
the palette lists a build's symbols, filters, and Return opens the definition's
line; and the hover shows the mnemonic, the OS call and the label — and shows
nothing for a directive or a word nothing knows.

---

## 5. Watchpoint symbols

**Status:** delivered 2026-10-17 on `feature/watchpoint-symbols`. The verification
below is the state it was written against: a watchpoint could only be a raw address,
which GEMDOS re-picks every run — the interface a *breakpoint* got, the watchpoint
never did.

### What exists

- `src/debug/Watchpoint.h`: a watchpoint *is* a raw `quint32 address` plus a width
  byte, rendered into Hatari's conditional-breakpoint form `b ($addr).w ! ($addr).w`
  — Hatari has no data watchpoints (upstream's `w` is compiled out), so the IDE arms
  change-tracking breakpoints and the debugger's change-notation evaluates as "fire
  when the value differs from its previous value". (Caveat worth remembering in any
  UI: the same value written again does not fire, and a read never does.)
- Breakpoints already know how to take a symbol: the remote `breakpoint <n|label>`
  verb resolves a label to its definition's file:line and toggles there, replying
  with where it landed and the address once live bases exist (`RemoteControl.cpp`,
  via `EmulatorHost::toggleBreakpointAtLabel`).
- The symbol table that can resolve label → address is already in the debugger's
  hands (`symbols` verb, `EmulatorHost::symbolsJson()`), and breakpoints only arm
  after `info basepage` delivers the live bases (AGENTS.md rule 4 — `setLiveBases()`
  → `armBreakpoints()`).

### What was built, and the decisions it took

- **The model carries the name, never the address.** `Watchpoint` gains
  `symbol` (empty for a raw address) and `resolved`; `label()` names what was asked
  for (`watch counter.l`) and `command()` still names an address. A named
  watchpoint's address is *never written back into the model*: the arm plan's
  resolved copy is what the panel is handed for that run, which makes the
  "confidently wrong address from last run" failure structurally impossible rather
  than merely avoided.
- **One resolution rule, called at arm time.** `planWatchpoints()` (beside
  `planBreakpoints()` in `debug/Breakpoint.{h,cpp}`) resolves a name through
  `labelAddress()` — the same function a hook script's `sym()` and the remote
  `breakpoint <label>` verb already use — so a label means the same thing wherever
  it is named. The reason strings are phrased by the caller, as `labelAddress()`'s
  contract requires: `[watchpoints] watch counter.l: no symbol named 'counter' (has
  the project been built?)`.
- **Re-resolved on every arm, and armed where the bases are.** A raw address is
  armable before the program has said where it was loaded, so the pre-bases path
  still arms those; a *name* is not, so it is listed as pending there and armed by
  the post-bases path, exactly like a source breakpoint. Nothing about a name
  survives a run.
- **Everything that names a watchpoint gained the name.** `addWatchpointAddress()`
  (Run ▸ Add watchpoint's dialog) takes `label|address` with an optional
  `.b`/`.w`/`.l`; the remote verbs are the set/clear pair `watchpoint <a|l>` and
  `clearwatchpoint [a|l]` — never a toggle, so arming the same watch twice is two
  watchpoints and clearing is explicit — and the MCP tools follow
  (`pist_watchpoint` with `address` or `label`, and `pist_clearwatchpoint`, marked
  destructive like the other state-rewriting calls).
- **The refusals are sentences about the symbol.** A name that resolves to nothing,
  to an equate, to a symbol with no source position, or to one that emits nothing is
  refused naming it and saying which of those it is; a name whose bases are not live
  is *not* refused — it is pending, and the panel says so. A label that exists wins
  over a hex-looking token, so `watch deadbeef` watches the label when the program
  has one; the width suffix owns a trailing `.b`/`.w`/`.l`, which is the one spelling
  a label ending in those cannot use.

### Tested

`tst_debug` pins the plan with no emulator: a raw address passes through resolved, a
name resolves to its label's live address through the same fixtures `sym()`'s test
uses, an unknown name and an equate are refused with reasons naming them, and before
the bases arrive a name is unresolved while a raw address still arms. `tst_gui`
pins the text form and the panel without an emulator: a label is stored as a name
with `resolved == false`, a symbol nothing knows and an equate are refused by name,
the panel shows it as pending, and `clearWatchpoints` reports one removed and then
refuses. `tst_remotecontrol` pins the verb's replies — the raw address form, a
label's *pending* answer before any stop has reported the bases, the refusal naming
an unknown symbol, and `clearwatchpoint`'s count and second-call refusal — and
`tst_emulatorhost` proves the label path against the machine: the plan resolves
`val2` through the build's symbols and the session's own bases to the address the
program's data really has, and that command is what stops the run. `tst_mcp` pins
the two tools and the catalog.

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
