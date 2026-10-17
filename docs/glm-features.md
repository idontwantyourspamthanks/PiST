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

### What was built

- **Three operations on `floppy::Transfer`**, beside the transfers that already live
  there: `removeEntries()` (a `removals`-only `updateImage` call; a chosen folder takes
  its subtree, which is the writer's own rule), `renameEntry()` (the entry's bytes are
  read out and re-added under the new root in the *same* `updateImage` call that drops
  the old paths — one atomic rebuild, so a folder and its contents move together and a
  failure leaves the disk as it was), and `makeDirectory()` (an `isDirectory` item with
  no contents).
- **The transfers' own confirm/resync preamble.** All three go through
  `confirmTargets()`, so the `looksLikeCanonical720k` gate asks before somebody else's
  disk is rebuilt, and a drive that moved on while the question was up is noticed rather
  than written. The listing is taken deliberately *after* the question, for the reason
  the transfers do the same: a dialog runs an event loop, so a directory listed before
  it can already be stale.
- **A name the disk already shows is refused, not numbered.** `updateImage` numbers an
  addition whose 8.3 name is taken — right for a paste, wrong for a rename, which would
  silently become `NAME(2)`. The new `NameTaken` outcome carries the name and the
  browser phrases it. The comparison is the spelling the pane shows; an entry whose
  *stored* name field differs from its displayed one (a real disk's lower-case or
  accented field) can still collide, and the writer then numbers the addition —
  documented at the function rather than discovered later.
- **The menu** (`FileBrowser::onFloppyContextMenu`): Copy/Cut/Paste, then Delete… /
  Rename… / New Folder…. Delete and Rename need a selection, Rename needs *one* entry
  (renaming a set is not something a pane can mean), and New Folder lands in the
  directory that was clicked, exactly as Paste does. Delete asks first — it is not a
  copy that can be undone by repeating it — while Rename and New Folder take their name
  from a dialog that refuses an empty name, or one containing `/`, before the controller
  is called at all.

### Tested

`tst_gui`, offscreen: a fixture image mounted on A:, delete a file → the row is gone
*and* `listRaw` no longer lists it; rename a file → the pane and the image both show the
new name, the old one is gone, and `readFileRaw` returns the same bytes; New Folder →
the entry exists as a directory in `listRaw` (an empty folder is still an entry); and a
folder whose name is already on the disk is refused with the image untouched.
