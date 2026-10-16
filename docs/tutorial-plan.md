# Tutorial plan: PiST as a teaching aid

Spec for adding a teaching layer to PiST: a lesson format, a runner, a panel, and the
explain-in-place surfaces that turn the IDE's expert tools into something a person can
learn 68000 assembly *with* rather than merely *in*. Written so that an agent (or human)
can implement it without re-deriving the design; §7 and §13 are the parts that change
existing code.

Design decided (2026-10-16); implementation not started. Every claim about the current
tree below was verified by reading the source or by driving a running build (§1.2).

## 1. Problem

PiST's surfaces are those of a working tool for someone who already knows the machine: a
register pane, a memory viewer, labelled disassembly, watchpoints, a stack view, hardware
registers, a profiler, and a searchable instruction/OS-call reference. Together they are
excellent *at* the point where a person knows what they are looking at.

They are also, today, reachable all at once and in no order. Nothing in the IDE says what
to do first; nothing says why a line matters; nothing connects "the program faulted" to
"here is what a bus error is, and here is why `$ffff8240` needs `Super(0)` first". A
beginner's first five minutes are spent finding out that F7 assembles, F5 runs, and that
the emulator panel takes the keyboard with a click — facts the tool states in four
different places and never as a sequence.

### 1.1 What a lesson must not become

Two failures are worth naming up front, because both are tempting:

- **A web page in a dock.** Prose that describes the IDE will drift from the IDE. If a
  lesson step cannot be checked, it is documentation wearing a tutorial's clothes.
- **A macro player.** A script that presses buttons teaches nothing; the user watches.

The design below takes the middle path: prose that *teaches* is authored in the lesson,
and every claim a lesson makes about the machine is **machine-checked** by the runner, so a
lesson that stops being true fails a test rather than misleading a reader.

### 1.2 A verified defect in today's front door

The welcome view is the only guided entry the IDE has, and its demo rows are broken. This
is the cheapest real teaching win available, so it is Phase L0 (§13).

`WelcomeView::demoProjects()` (`src/ui/WelcomeView.cpp:173-213`) lists the three shipped
demos and returns each row's path as the **`.pistproject`** path
(`hello.pistproject`, `keyred.pistproject`, `spritedemo/sprites.pistproject`). Clicking a
row emits `openRequested(path)`, wired straight to `MainWindow::openPath`
(`src/ui/MainWindow.cpp:563-565`), and `openPathImpl` has **no `.pistproject` branch**
(`src/ui/MainWindow.cpp:3231-3262`): the path falls to the final `else` and
`addEditorTab` (`:3248`), so the project *JSON* opens as a text document. The
`loadProjectForSource()` call that follows (`:3260` →
`settings::projectFileFor()` → `ProjectSettings.cpp:192-199`) happens to resolve back to
the same `.pistproject` file, so the settings load, but they load with
`sourceFile` pointed at the JSON.

Verified by execution, 2026-10-16, against an offscreen build driven through PiST's own
remote-control socket (`src/control/RemoteControl.cpp`; `README.md` → "Remote control"):
`open demo/hello.pistproject` followed by `tabs` returned

```json
[{"current":false,"path":"/home/ryan/Code/pist/demo/hello.s"},
 {"current":true, "path":"demo/hello.pistproject"}]
```

and `read` on the current document returned the project JSON, not a source. So the door
labelled **"Start with a demo project"** opens a configuration file in a text editor.
`MainWindow::openProject()` — which does this correctly, deriving the implied `.s` from
the project file's name (`src/ui/MainWindow.cpp:3412-3432`) — is a separate path that the
welcome view does not call, and `tst_gui`'s doorway test
(`welcomeViewDoorwaysFollowTheState`, `tests/tst_gui.cpp:6972-7021`) asserts only that the
rows point at files that exist, never that clicking one opens a source.

### 1.3 Where a learner actually gets stuck

Each of these is a fact the IDE knows and does not teach at the moment it is needed:

| The learner hits | What is true | Where PiST keeps it |
|---|---|---|
| "Where do I type?" | A source is a file on disk, assembled beside a `.PRG`; there is no scratch buffer | `docs/untitled-plan.md`; stated nowhere in the UI |
| "Why is nothing on the screen?" | The emulator display appears in the panel only once a session is up, and takes the keyboard on a click (F12 releases) | panel placeholder text (`EmulatorDisplayWidget.cpp:89,109,130`) |
| "Why did my program die?" | Writing `$ffff8240` in user mode is a bus error; the ST needs `Super(0)` | the keyboard and image-include *templates* explain it in comments |
| "Which register holds the key?" | `Cconin` returns it in `D0` | `OsCallRef` holds `d0: %1.` — in a dock the learner has not opened |
| "How do I stop at a line?" | Breakpoints are `file:line`, set in the gutter | the breakpoint panel's empty hint (`MainWindow.cpp:2764-2765`, `BreakpointPanel.cpp:33`) |

## 2. What already teaches (the reuse base)

The teaching layer is mostly *sequencing* material that exists; almost nothing here needs
inventing.

- **The instruction and OS-call reference is already curriculum.** `src/editor/InstrRef.cpp`
  holds 123 instructions and `src/editor/OsCallRef.cpp` 110 TOS/GEMDOS/BIOS/XBIOS calls as
  hand-written tables, with trap, opcode, prototype, returns, availability and stack bytes
  for the calls, and flags for the instructions. `InstructionRefView`
  (`src/ui/InstructionRefView.cpp`) fronts them with a filter, a detail pane, an
  **"Insert binding into editor"** action and a canonical binding generator
  (`src/editor/OsCallBinding.{h,cpp}`). The editor has a clickable strip
  (`MainWindow.cpp:532-540`) that follows the caret and shows the current line's
  instruction or OS call within ±10 lines (`followCursorReference`, `MainWindow.cpp:2977`).
- **The New File templates teach in comments.** `kGemdosHello`, `kKeyboardInput` and
  `kImageInclude` (`src/ui/FileTemplates.cpp:16,47,85`) are annotated listings: the first
  names the F-keys and suggests the breakpoint worth setting ("one on the
  `move.w #7,-(a7)` line"); the second explains why hardware registers need `Super(0)`;
  the third documents the `.dat` block map, the mask-then-planes blit idiom and the export
  dialog's file map.
- **The demo projects are worked examples.** `demo/hello.s` is a GEMDOS
  `Cconws`/`Crawcin`/`Pterm0` program whose header teaches the build/run/breakpoint loop and
  suggests watching `D0`; `demo/keyred.s` proves the input and audio paths in one screen;
  `demo/spritedemo/sprites.s` is a full bitplane-export pipeline with a 30-line header
  documenting the data layout.
- **`mediademo.sh` is already a lesson script** — a four-step printed playbook
  (`mediademo.sh:36-52`) that is the closest thing in the tree to a tutorial step list, and
  a model for tone: short, imperative, one action each.
- **Explanations at the point of use exist, unsequenced:** the setup dialog's intro on why
  PiST never patches a third-party tool (`SetupDialog.cpp:57-62`), the build settings' note
  on separate compilation and the TOS entry-module rule (`SettingsDialog.cpp:193-198`), the
  console input's command hint `r, d, m $12596 20` (`MainWindow.cpp:2241`), the per-pane
  empty-state hints (`RegistersView.cpp:105`, `MemoryView.cpp:44,67`,
  `EmulatorDisplayWidget.cpp:89,109,130`), and the transient layout nudges
  (`MainWindow.cpp:887,1752`).
- **The automation needed to drive a lesson already exists.** `src/control/RemoteControl.cpp`
  exposes 29 verbs — `open`, `build`, `buildrelease`, `run`, `stop`, `step`, `stepover`,
  `continue`, `breakpoint`, `symbols`, `readmem`, `disasm`, `setreg`, `setmem`,
  `watchpoint`, `screenshot`, `console`, `state`, `statejson`, `problems`, `profile`,
  `watch`/`unwatch`, `help`, `quit` — with a push channel: `watch` makes the socket a
  watcher and the IDE publishes `event stopped pc=0x…` and `event running`
  (`RemoteControl.cpp:216-241`, `MainWindow.cpp:1708-1716,4768-4772`). `src/control/mcp/`
  re-exposes 28 of them as MCP tools, plus `pist://console`, `pist://state` (subscribable)
  and `pist://document` resources and two prompts (`diagnose-build`, `find-hot-loop`).

## 3. Goals

1. A person who has never written 68000 assembly can go from a fresh install to a running,
   stepped, understood program without leaving the IDE and without reading a web page.
2. Every factual claim a lesson makes about the machine is checked by the runner against
   the machine, so lessons cannot rot into misinformation.
3. The same lesson runs three ways: interactively in the IDE, headlessly in `ctest`, and —
   later — through the remote-control protocol by an external agent. One format, one
   runner, no second implementation.
4. The IDE explains itself where the user is stuck, not in a separate manual: the reference
   dock gains worked examples, the problems pane gains "what this means", and each panel
   can explain itself.
5. Nothing in the teaching layer violates the project's rules: no patching, linking or
   config-writing for a third-party tool (`AGENTS.md` rule 1), breakpoints still armed only
   after `info basepage` (`AGENTS.md` rule 4), and every emulator-dependent lesson skipping
   cleanly when no emulator is present (rule 7).

## 4. Non-goals

- **Not an LLM tutor.** `docs/FUTURE.md` §9 is the chat-sidebar/autocomplete item; it needs
  an endpoint and a key and is a different feature with a different failure mode. A lesson
  must work offline, deterministically, with no network. A tutor, when it lands, should
  *consume* lessons as its script rather than duplicate them (§16).
- **Not a general scripting language.** Lesson steps are a fixed keyword set (§6), not a
  shell. Anything that needs real logic is a Python/bash driver over the control protocol.
- **Not a manual.** The prose that *states* facts stays in `README.md`, `docs/ARCHITECTURE.md`
  and `docs/PLAN.md`; lessons teach, and link. A lesson that restates a fact another
  document owns is a second copy of it (§14).
- **Not translated.** The tree is English-only and has no translation infrastructure — 1073
  `tr()` call sites, no `.ts`/`.qm`, no `qt_add_translations` (`HelpSurface` survey,
  2026-10-16). Lesson prose is user-facing text and will use `tr()` like everything else,
  but shipping lessons in another language is not part of this.
- **Not a command palette, and not a redesign of the panels.** §12 adds the discoverability
  that is missing; it does not restructure the docks.

## 5. Decisions made (2026-10-16)

1. **A lesson is data, not code.** Lessons are text files in the tree (§6).
2. **One runner, two callers.** The runner drives the IDE through the `control::ControlHost`
   interface that `RemoteControl` already uses (`ControlHost.h:48-215`) — in-process for the
   panel and the tests, and (Phase L3) over the socket for an external agent.
3. **Every step may carry a checkpoint**, and a step that cannot be checked must say so
   (`nominal`). Checkpoints are what make this a test rather than a document.
4. **The panel is a dock** (`lessonDock`), not a mode of the welcome view: it must stay
   visible beside the editor and the debug panes while a session runs, and the welcome view
   disappears the moment a document opens (`updateCentralPage`, `MainWindow.cpp:996-1008`).
5. **A `Learn` layout preset** joins Editing/Debugging/Sprite
   (`applyLayoutPreset`, `MainWindow.cpp:2607`; menu at `:2448`) — the arrangement the
   lessons assume, so "look at the registers" is a click and not a docking exercise.
6. **Five lessons ship in v1** (§9), each built from material that already exists, plus the
   front-door fix (§1.2) as the first phase.
7. **Lesson sources are owned by the lesson**, created through the New File transaction
   from a template where one fits (`NewFileSpec::create`, `NewFileSpec.cpp:126-146`), so
   nothing is copied out of `demo/` and an installed app has everything it needs.
8. **Distances are checked, not measured**: no time estimates, no "should take 10 minutes";
   lesson length is a step count.

## 6. The lesson model

A lesson is one directory under `lessons/`:

```
lessons/hello-tos/
    lesson.pil          the steps
    (sources, if the lesson owns files rather than creating them)
```

`.pil` is line-oriented text, deliberately not JSON: a lesson is prose-first and must be
comfortable to write and review by hand, and the parser stays small enough to test
directly. Front matter is `key: value`; steps begin at `step <title>` and hold indented
directives:

```
title:   Hello, TOS
level:   first steps
needs:   assembler, emulator, rom
learn:   what a GEMDOS trap is; how a breakpoint stops the machine; where a call leaves its answer
source:  template "GEMDOS hello" as hello.s

step  Assemble the file
  teach  F7 assembles hello.s beside it and reports problems in the pane below.
  do     build
  expect problems == none

step  Stop before the program asks for a key
  teach  Breakpoints are file:line, never addresses: GEMDOS relocates the program on
         every run, so the line is the only stable name for it. Line 16 is the Crawcin
         call, which is the breakpoint the template's own header suggests.
  do     breakpoint 16
  do     run
  expect stopped
  expect at hello.s:16

step  Stop again where the answer arrives
  teach  Crawcin is trap #1, function 7, and it leaves what you typed in D0. Line 18
         runs immediately after it returns, so a breakpoint there can see D0.
  do     breakpoint 18

step  The keyboard belongs to the emulator
  teach  Click the emulator panel to hand it the keyboard and press any key; F12 gives
         the keyboard back. A lesson cannot press it for you, and any key is correct.
  nominal the key press is the user's, and every key is a valid answer
  do     continue
  expect stopped
  expect at hello.s:18

step  Read the answer
  teach  D0 holds your key. Watch it in the Registers panel, then step on to Pterm0.
  expect reg.d0 >= 1
```

Line numbers above are the shipping template's own (`kGemdosHello`,
`src/ui/FileTemplates.cpp:16-45`, whose line 16 is the Crawcin `move.w #7,-(a7)` and
line 18 the `addq.l #2,a7` after it).

Directives:

| Directive | Meaning |
|---|---|
| `teach` | prose shown for this step; the only required directive |
| `do` | an action the runner performs (`open`, `goto`, `breakpoint`, `build`, `run`, `stop`, `step`, `stepover`, `continue`, `readmem`, `disasm`, `explain`) |
| `see` | where in the UI the result appears; a hint, not an action |
| `expect` | a checkpoint the runner evaluates (§7.2) |
| `nominal` | explicitly "no machine check here", with the reason |

`do` verbs are the **existing** control verbs wherever one exists, and three new ones
(§7.1). This is deliberate: a lesson step should be a thing the IDE can already be told to
do, and adding a verb for the lesson is a signal that the verb was worth having anyway.

## 7. The runner and the control surface

### 7.1 What is missing

Everything a lesson needs to *talk* to the machine exists. Three things it needs in order
to *point* — and one it needs in order to *assert* — do not:

| Need | Today | Where it should live |
|---|---|---|
| Move the editor to a line or label and mark it | `CodeEditor::gotoLine` exists and is used by the caret-follow path (`CodeEditor.cpp:630-638`); no control op reaches it | a `ControlHost` op; the lesson runner calls it |
| Create a file from a template | `NewFileSpec`/`FileTemplates` do this for the New File dialog; no control op | reuse the transaction in-process |
| Raise the reference dock on a topic | `raiseInstructionRef()` (`MainWindow.cpp:3051`) and `showOsCall`/`showInstruction` exist; no control op | a `ControlHost` op taking a topic |
| Evaluate a checkpoint | `statejson` (registers, PC) and `readmem` (bytes) return blocks; nothing compares them | the runner, against the block replies |

Phase L1 needs the three ops as `ControlHost` methods only, so panel and tests share one
path. Phase L3 exposes them as remote verbs and MCP tools — and this is worth doing on its
own merit: an external agent today can set a breakpoint on `file:line` but cannot *show* a
user that line, which is a gap in the agent story too.

### 7.2 Checkpoints

`expect` is evaluated against a snapshot the runner already has:

| Form | Meaning | Source |
|---|---|---|
| `expect stopped` / `running` | session state | the `stopped`/`running` events, or `statejson` |
| `expect reg.d0 == 9` | one register equals a value | `statejson` |
| `expect pc == message+0` | the PC resolves to a symbol plus offset | `symbols` + `statejson` |
| `expect mem 0x1000 4 == de ad be ef` | memory contents | `readmem` |
| `expect problems == none` | last build produced no diagnostics | `problems` |
| `expect at hello.s:24` | the editor is on a line, and the PC maps to it | the editor, plus the PC→line map the caret path already uses |

A failed checkpoint is **advisory in the panel** (a lesson must never trap the user mid-step)
and **fatal in `tst_lessons`** (§13, L1): the lesson's own test run treats every `expect` as
an assertion. That asymmetry is the whole design: the user gets a gentle "D0 is 7, the step
expected 9 — the emulator version or the trap number may have moved", and CI gets a red run.

Sequencing constraint, from the launcher rules: a lesson that sets a breakpoint must do it
**before** `run`, and the runner must never add one during the boot-to-entry window —
breakpoints arm only after `info basepage` arrives (`AGENTS.md` rule 4, `docs/PLAN.md` §5
rule 4). The listener must also not assume a stop it did not see: the pre-base window is a
state lessons can be written into, so the runner asserts `stopped` on the event rather than
on the command's return.

## 8. The Lesson panel and the `Learn` layout

A dock, `lessonDock`, beside the console:

```
┌─ Lesson ───────────────────────────────────────────────────────┐
│ Hello, TOS                              3 of 7  ▸ ▸            │
├────────────────────────────────────────────────────────────────┤
│ Watch a GEMDOS call return                                     │
│                                                                │
│ Crawcin is trap #1, function 7, and it leaves                  │
│ what you typed in D0. Line 18 runs just after                  │
│ it returns, so a breakpoint there sees D0.                     │
│                                                                │
│ [ Show me ]   [ Why? ]                                         │
├────────────────────────────────────────────────────────────────┤
│ ✓ D0 holds the key you pressed                                 │
│ [ Back ]   [ Next ]   [ Restart lesson ]                       │
└────────────────────────────────────────────────────────────────┘
```

Top to bottom: the step list with ticks for passed steps and the lesson's position; the
current step's `teach` prose; the two step actions; then the checkpoint verdict and the
navigation. The verdict line is the only place a checkpoint reports, and a `nominal` step
says so there instead ("nothing to check here — watch the screen").

- **Show me** executes the current step's `do` — for a user who wants to watch the IDE work
  before doing it themselves.
- **Why?** raises the reference dock on the step's topic (§10), which is the join between
  the lesson and the reference tables.
- The verdict line is the only place a checkpoint reports; a `nominal` step says so
  ("nothing to check here — watch the screen").
- The whole panel is driven by the runner, not by a second code path: pressing **Show me**
  and running the headless test call the same function.
- `WelcomeView` gains one row above the demos: **"Learn 68000 assembly"**, which opens the
  Lesson dock on lesson 1. This is the only welcome-view change; the doorway defect (§1.2)
  is fixed in the same phase.

## 9. The first five lessons

Chosen so that each is buildable from material already in the tree, and each ends with
something the user can *see*.

1. **First run** (`first-run`). No tools: what vasm, Hatari and EmuTOS are and why PiST
   never patches them (`SetupDialog.cpp:57-62` is the source), the guided setup, the
   checksum that is shown before anything downloads. Ends at readiness = ready
   (`refreshWelcome`, `MainWindow.cpp:1009`). Needs no emulator.
2. **Hello, TOS** (`hello-tos`). Creates `hello.s` from the GEMDOS hello template; assemble;
   run to the entry stop; breakpoint before `Crawcin`; read `D0` after the key; step
   through `Pterm0`. This is `demo/hello.s` as a step list, and its header comments already
   carry the prose.
3. **The hardware and the keyboard** (`keyboard-hardware`). From the keyboard template and
   `demo/keyred.s`: why user-mode access to `$ffff8240` is a bus error, `Super(0)`,
   `XBIOS Setcolor`, and the emulator panel's grab (click to capture, F12 to release) — the
   one interaction the user cannot discover from a menu. Needs a real emulator.
4. **Sprites and the export** (`sprites`). From `demo/spritedemo/`: the `.pim` sheet, File ▸
   Export Bitplanes, the export dialog's file map, the mask-then-planes blit, `incbin`.
   This is where the image editor and the assembler meet, and nothing currently explains it.
5. **Debugging a fault** (`fault`). A small purpose-written source that deliberately
   bus-errors and divides by zero, so the lesson can teach reading the stop, the
   disassembly, the stack pane and the memory viewer while the machine is broken — the
   skill the current UI assumes.

Each lesson names its `needs` in front matter and is skipped (not failed) when they are
absent.

## 10. Turning the reference dock into a teaching surface

The reference tables are the largest teaching asset in the tree and are currently a lookup:
you must already know the mnemonic to search for it. Three additions, in cost order:

1. **`Used in:`** — a field naming the demo or lesson that exercises the entry
   (`demo/hello.s`, `lessons/fault/`). Cheap to author, and it turns the dock into a
   doorway onto a working program. A test asserts every named path exists in the tree, so
   a rename cannot leave a dead link.
2. **`Example:`** — a minimal, runnable snippet per entry, inserted by the button that
   already inserts a canonical binding (`osCallBinding`). For GEMDOS calls the binding
   generator already emits the argument sequence (`OsCallBinding.{h,cpp}`); the example
   adds the surrounding `_start:`/`Pterm0` so the snippet assembles standalone.
3. **`Typical use` and `Common mistake`** — one line each for the instruction entries, where
   the traps are real and well known: `move.w` vs `move.l`, `dbf` counting `n+1` times,
   `trap #1` (GEMDOS) vs `trap #14` (XBIOS), a byte write to `$ffff8240` needing a word
   access. This is the difference between a reference and a tutor, and it is authorable in
   bulk because these entries are already hand-maintained data.

Authoring 233 entries' worth of examples is a large job; v1 covers only the entries the
five lessons touch, and the rest is deliberately deferred (§13, L2/L4).

## 11. Explaining failures where they happen

The problems pane already carries `File`/`Line`/`Message`
(`src/ui/MainWindow.cpp:2223`) filled from vasm/vlink diagnostics (`docs/PLAN.md` §4.1).
Add a **"What this means"** column fed by a small, hand-written table from diagnostic
pattern → plain-language explanation + the reference entry or doc section that covers it:
"`operand size mismatch` — this instruction's operands disagree on size; see `move.w`",
"`undefined symbol` — the label is not in this file and not `incbin`'d", "a GEMDOS file
name cannot contain `*?:\"<>|`". The table lives beside the reference tables, in the same
style (`src/editor/DiagnosticRef.{h,cpp}`), and a diagnostic with no entry shows nothing
rather than a guess.

Two constraints: the explanation must not offer to *fix* anything by writing to a
third-party tool's files (`AGENTS.md` rule 1), and it must not repeat the raw message —
only what the message means and where to look.

## 12. Help, shortcuts and "what is this panel?"

The menu bar has no `Help` menu: it is Desk (About), File, View, Search, Build, Run
(`MainWindow.cpp:1786-1883`). Add one:

- **Lessons…** — raises the Lesson dock.
- **Instruction & OS-call reference** — raises `instructionRefDock`.
- **Keyboard shortcuts** — a table **generated from the live actions**, because the
  shortcut scheme is switchable at runtime (`applyShortcutScheme`, `MainWindow.cpp:2739`;
  the two schemes are named in `SettingsDialog.cpp:409-420`) and a hand-written list would
  be wrong half the time.
- **What is this panel?** — one paragraph per dock, from a single table keyed by the dock's
  `objectName` (the same identifiers `isDebugDockName` already lists,
  `MainWindow.cpp:218`). Reachable also from each dock's title-bar context menu.

A command palette is deliberately left out of v1: it is a larger change to the action
system than the whole of the teaching layer, and the Help menu plus the lesson panel cover
the discoverability gap this plan exists to close.

## 13. Phasing

**L0 — the front door (no new features).** Fix §1.2: a demo row opens the project it names.
The recent-files rows are already correct (they pass a source path); it is only the
`.pistproject` case that needs to route through the existing implied-source derivation
(`MainWindow.cpp:3418-3432`), so the row opens `hello.s` with the
project's settings loaded. Test: clicking a demo row lands on a `CodeEditor` whose
`filePath()` is the `.s`, with the project's settings loaded — extending
`welcomeViewDoorwaysFollowTheState` (`tests/tst_gui.cpp:6972`), which today never clicks a
row. Ships independently; it is a bug fix, and it is the single largest teaching win per
line changed.

**L1 — the model, the runner, the panel.** The `.pil` parser and its unit test; the runner
over `ControlHost` with the three new ops (§7.1); the checkpoint evaluator (§7.2); the
Lesson dock, the `Learn` layout preset and the welcome row; lesson 2 end to end; and
`tst_lessons`, which runs every lesson headlessly (skipping emulator-dependent ones as the
other integration suites do — `AGENTS.md` rule 7) and fails on any unmet `expect`.

**L2 — the content.** Lessons 1, 3, 4 and 5; the "What this means" table (§11); `Used in:`
and `Example:` for the entries the lessons touch (§10).

**L3 — external parity.** The three ops gain remote-control verbs and MCP tools, so an agent
can run a lesson and can show a user a line; `nominal` steps are upgraded to checkpoints
where a check has since become possible.

**L4 — breadth.** `Typical use`/`Common mistake` across the instruction table; further
lessons (interrupts, VBL, blitter, GEMDOS file I/O, a Mega STE/Falcon variant).

## 14. Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| A lesson rots and teaches something false | The worst outcome for a teaching feature | Checkpoints (§7.2) checked by `tst_lessons`; `nominal` is explicit and reviewed |
| Prose duplicated from the docs drifts | Two copies of a fact, one of them wrong | Rule in §4: lessons teach and link, docs state; a lesson names its source doc for factual claims |
| The reference tables are hand-maintained, and `Used in:` adds another thing to keep true | A dead link in the dock | A test asserting each named path exists in-tree |
| Lesson steps fight the launcher rules | Breakpoints armed too early silently do nothing (`AGENTS.md` rule 4) | §7.2's ordering constraint: breakpoint before run; never during the boot window |
| A lesson cannot run without tools, so CI skips the interesting ones | Rot goes unnoticed | Split lessons by `needs:`; the emulator-free lessons must still pass everywhere, and the emulator ones run on the Linux/macOS CI legs that build the fork |
| Scope: 233 reference entries is a second content project | The teaching layer never finishes | Explicitly phased (§13 L2/L4); v1 covers lessons-touched entries only |

## 15. Open questions

1. **Where do lessons live for an installed build?** The repo-developer case is easy
   (`WelcomeView::demoProjects()` already searches cwd then upwards for `demo/`), but the
   app bundle needs the lessons in its share directory. Not yet decided — likely the same
   `bundledDataSearchPaths()` mechanism the ROM lookup uses
   (`src/emu/Paths.h:29`, `Paths.cpp:168`).
2. **Should `expect` be able to compare against a screenshot** (`screenshot` exists)?
   Tempting for lesson 3 (a red screen), but pixel comparison across emulator versions and
   the host's video driver is a flake factory. Leaning no.
3. **Is the Lesson dock the right home**, or should a lesson be a *mode* that owns the
   layout while it runs? The `Learn` preset is the compromise; revisit if the dock fights
   the layout on a small screen.
4. **How much prose is too much?** The house style is terse. A lesson is the one place
   where longer prose is warranted, and the right ceiling has not been found.
5. **Should checkpoints be authorable as "close enough"** (e.g. `reg.d0 >= 1`) for values
   that legitimately differ across TOS versions? Probably yes for a documented set of cases.

## 16. Relationship to other documents

- **`docs/FUTURE.md` §9 (LLM integration)** — a different feature with a different failure
  mode. A tutor would consume `.pil` lessons as its script and its context; it does not
  replace them, and no lesson may require it.
- **`docs/FUTURE.md` §6 (debug surface depth), §10 (scripted memory editing)** — the
  runner uses `cmd`, `readmem` and `watchpoint`, and would benefit from §10's user scripts.
- **`docs/PLAN.md` §5 (launcher rules)** — binding on the runner: breakpoints after
  `info basepage`, and no third-party file is ever written (rule 1, rule 6).
- **`docs/PLAN.md` §6 (phases)** — L0–L4 are new work in the existing phase scheme, not a
  replacement for any of it.
- **`docs/untitled-plan.md`** — the document model this plan assumes (a document is a file;
  the welcome view is the zero-document state).
- **`docs/ARCHITECTURE.md`** — lessons cross-link it rather than restating how the build,
  launch or debug transports work.

## 17. Status

Design decided 2026-10-16; nothing implemented. §1.2's defect was verified by execution
that day (the control-socket probe quoted above) — the fix is Phase L0 and does not depend
on any decision the rest of this document makes.
