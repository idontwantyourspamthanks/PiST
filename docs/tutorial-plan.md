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
9. **Teaching text names actions, never keys.** The shipped teaching text hard-codes one
   scheme today and is wrong under the other: `demo/hello.s:6-11` and the GEMDOS hello
   template (`src/ui/FileTemplates.cpp:16-23`) say "F8 toggle breakpoint … F10 step, F11
   step over", which is the `pist` scheme; under `common` (`appearance::shortcutScheme()`,
   default `pist`, `src/ui/Appearance.cpp:345`) step is F11, step over is F10 and toggle
   breakpoint is F9 (`applyShortcutScheme`, `src/ui/MainWindow.cpp:2739-2761`). So:
   - generated text (lesson prose, New File templates) carries tokens —
     `{key:step}`, `{key:toggle-breakpoint}` — resolved from the live actions when the text
     is rendered or the file created, so it always names the key that is actually bound;
   - committed sources (`demo/hello.s`) cannot be resolved at runtime, so they name the
     actions and point at Settings ▸ Appearance for the key, keeping only the bindings that
     are scheme-independent (F7 build, F5 run, Shift+F9 clear).
10. **The fix ships with a test that clicks a demo row.** §1.2's defect survived because
   `welcomeViewDoorwaysFollowTheState` (`tests/tst_gui.cpp:6972`) asserts only that each
   row's tooltip names an existing file and then clicks a *recent source* row, never a demo
   row — so nothing exercised the path a newcomer takes first. `welcomeDemoRowOpensTheSource`
   is that pin: it clicks a demo row and requires the source to open.

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
| `do` | an action the runner performs (`open`, `goto`, `breakpoint`, `clearbreakpoint`, `build`, `buildrelease`, `run`, `stop`, `step`, `stepover`, `continue`, `readmem`, `disasm`, `explain`) |
| `see` | where in the UI the result appears; a hint, not an action |
| `expect` | a checkpoint the runner evaluates (§7.2) |
| `nominal` | explicitly "no machine check here", with the reason |

`do` verbs are the **existing** control verbs wherever one exists, plus the few that
pointing and checking need (§7.1). This is deliberate: a lesson step should be a thing the
IDE can already be told to do, and adding a verb for the lesson is a signal that the verb
was worth having anyway.

## 7. The runner and the control surface

### 7.1 What is missing

Everything a lesson needs to *talk* to the machine exists. What it needs in order to
*point*, to *set* a breakpoint repeatably, and to *assert* does not:

| Need | Today | Where it lives now |
|---|---|---|
| Move the editor to a line and show it | `CodeEditor::followExecutionLine` exists (the caret-follow path, `CodeEditor.cpp:687`) | `ControlHost::navigateToLine` — the caret moves, the keyboard does not |
| Create a file from a template | `NewFileSpec`/`FileTemplates` do this for the New File dialog; no control op | `ControlHost::createFileFromTemplate`, the same transaction the dialog runs; accepts a template's label ("GEMDOS hello") or a short id ("gemdos") |
| Raise the reference dock on a topic | `raiseInstructionRef()` (`MainWindow.cpp:3051`) and `showOsCall`/`showInstruction` exist; no control op | `ControlHost::raiseReference`, by mnemonic or OS-call name |
| Set or clear a breakpoint without toggling | `toggleBreakpointAtLine`/`AtLabel` **toggle**; a lesson that re-runs a step would *remove* the breakpoint it just added, and one that moves its breakpoint on (a loop's `dbf` line, then the line the loop exits to) could never clear the first | `ControlHost::setBreakpointAtLine`/`setBreakpointAtLabel` — they take `on`, check the model first, and correct a toggle that did the opposite |
| Evaluate a checkpoint | `statejson` (registers, PC) and `readmem` (bytes) return blocks; nothing compares them | the runner, against the block replies |
| Know where the program is | the PC→line map the caret path already uses (`m_programMap`), unreachable from outside | `ControlHost::pcSourcePosition`, which is what an `at file:line` checkpoint is evaluated against — and the missing half of "set a breakpoint at `file:line`" for an agent |

Phase L1 needed the six as `ControlHost` methods only, so panel and tests share one path.
Phase L3 exposes them as remote verbs and MCP tools.

### 7.1.1 Waiting for the machine (added after the first implementation)

Three facts about the control surface shape the runner's polling, and each was measured
rather than assumed:

- **`stateJson()` changes the instant the machine resumes.** `running` is the emulator
  process and `stopped` is the debugger's stop flag; they move independently, and the
  object is already different before the machine has stopped anywhere new. So "the object
  changed" is not a stop signal.
- **A step from a stopped machine emits no running/stopped transition at all**, so there is
  no event to wait for (§15's open question 6, answered: polling).
- **The runner therefore waits for a stopped machine holding a register batch whose PC is
  not the one it was on.** That is the only condition that means "the machine has stopped
  *somewhere else*", and it is also what makes `expect stopped` mean something after `run`:
  a session that has only just come up reports `stopped` before any register batch exists,
  so the gate is `stopped && a batch with a PC`, and the boot to the entry stop passes it
  when it actually arrives.

The first implementation polled for "the JSON changed", which returns immediately on
resume: the expects then read the *previous* stop's PC and a lesson reported
`expected at hello.s:16, found hello.s:11` — the entry, not the breakpoint. The lesson was
right in both directions; the runner was wrong. `tst_lessons` caught it, which is the
design working: the checkpoints are not decoration.

### 7.2 Checkpoints

`expect` is evaluated against a snapshot the runner already has:

| Form | Meaning | Source |
|---|---|---|
| `expect stopped` / `running` | session state | the `stopped`/`running` events, or `statejson` |
| `expect reg.d0 == 9` | one register equals a value | `statejson` |
| `expect pc == message+0` | the PC resolves to a symbol plus offset | `symbols` + `statejson` |
| `expect mem 0x1000 4 == de ad be ef` | memory contents | `readmem` |
| `expect problems == none` | last build produced no diagnostics | `problems` |
| `expect at hello.s:24` | the machine is stopped on that source line | `pcSourcePosition` (§7.1) |

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

A dock, `lessonDock`, tabbed with the console and **hidden until asked for** — the factory
Editing layout keeps the bottom strip small, and the welcome doorway and the `Learn` preset
are what bring it up:

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
- `WelcomeView` gains a **"Learn 68000 assembly"** doorway in its actions row, which opens
  the Lesson dock on the lesson the IDE opens with and starts it. It is hidden when no
  lessons are found — an installed build that ships none — so it never offers a dead end.
  This is the only welcome-view change; the doorway defect (§1.2) is fixed in the same
  phase.

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

A lesson's program is assembled **outside the tree**. A template lesson's starter file is
created in the user's own data directory, and a lesson that ships a source — or reuses a
demo — runs a *copy* of it there, because the build writes a `.prg` and a listing beside
whatever it assembles: assembling in place would write into the source tree, and an
installed bundle's lessons directory may not be writable at all. `at file.s:16` still names
the same file, because the copy keeps the name.

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
pattern → plain-language explanation + the reference entry or doc section that covers it.
Keyed on the wording the parsers already pin in `tests/tst_parsers.cpp:91-155`, so every
entry is anchored to a real string rather than an invented one:

| Diagnostic (as the tool prints it) | What it means |
|---|---|
| `error 10 in line 2 of "bad.s": number or identifier expected` | vasm could not read an operand: a stray quote, a missing `#`, or a label that is not defined |
| `error 3004: section attributes <r> not supported` | the section flags are not ones the Atari/`tos` output module accepts |
| `fatal error 3008: output module doesn't allow multiple sections of the same type ()` | two sections of the same kind; the fix is a single `section` or a different one (`docs/PLAN.md` §4.3) |
| `Error 36: main.o (CODE+0x4): Reference to undefined symbol helper.` | a symbol is used but defined nowhere: check the spelling, an `incbin`'d file, or the sources listed in Project Settings |
| `error 1 in line 1 of "bad.s": unknown mnemonic <kMax>` | what the assembler read in the instruction field is not an instruction — in Motorola syntax a label must start at column 0, so an indented label or equate is read as a mnemonic |

The table lives beside the reference tables, in the same style
(`src/editor/DiagnosticRef.{h,cpp}`), and a diagnostic with no entry shows nothing rather
than a guess. Because a vasm message embeds a file name and line, the match is on the
message *pattern* (the leading code, or the stable wording after it), not on whole-string
equality.

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

**L0 — the front door (no new features).** *Landed.* Fix §1.2: a demo row opens the project
it names. The recent-files rows were already correct (they pass a source path); only the
`.pistproject` case needed the implied-source derivation `openProject()` already uses, so
a row opens `hello.s` and the project's settings load with it. `openProject()` is split
into the dialog and `openProjectPath()`, and the welcome view's `openRequested` now goes
through `openPathOrProject()`.

**L1 — the model, the runner, the panel.** *Landed.* `src/lessons/` (the `.pil` parser, the
checkpoint evaluator, the runner over `ControlHost`); the six new `ControlHost` ops (§7.1),
two of which only *set* a breakpoint; the `Lesson` dock and the `Learn` layout preset; the
welcome doorway; the first lesson end to end against a real ST/TOS session; and
`tst_lessons`, which parses every shipped lesson, evaluates checkpoints against constructed
snapshots, and runs two lessons headless against a real session — the emulator-dependent
ones skipping exactly as the other integration suites do (`AGENTS.md` rule 7) and failing
under `PIST_REQUIRE_EMULATOR`.

The polling mechanism (§7.1.1) was found by that test rather than designed in advance, and
it is the one part of the runner that is not obvious: "the state changed" is not a stop.

**L2 — the content.** *Landed, except §10's breadth.* All five lessons (§9) ship, with
`fault.s` for the debugging one; the "What this means" table (§11) is in the problems pane.
§10's `Used in:`/`Example:` per reference entry is deferred to L4 in its own right: it is
233 entries of authoring, not a wiring job, and the lessons do not depend on it.

**L3 — external parity.** *Landed.* Six verbs — `goto`, `newfile`, `setbreakpoint`,
`clearbreakpoint`, `reference`, `where` — and six MCP tools (`pist_goto`,
`pist_new_file`, `pist_set_breakpoint`, `pist_clear_breakpoint`, `pist_reference`,
`pist_where`), so an agent can point at a line, make a starter file, ask where the program
is, and set or clear (not toggle) a breakpoint. The
agent gap this closes was real: it could set a breakpoint at `file:line` but could not show
a user that line, or tell where the PC was.

**L4 — breadth.** `Typical use`/`Common mistake` and `Used in:`/`Example:` across the
reference tables; further lessons (interrupts, VBL, blitter, GEMDOS file I/O, a Mega
STE/Falcon variant).

## 14. Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| A lesson rots and teaches something false | The worst outcome for a teaching feature | Checkpoints (§7.2) checked by `tst_lessons`; `nominal` is explicit and reviewed |
| Prose duplicated from the docs drifts | Two copies of a fact, one of them wrong | Rule in §4: lessons teach and link, docs state; a lesson names its source doc for factual claims |
| The reference tables are hand-maintained, and `Used in:` adds another thing to keep true | A dead link in the dock | A test asserting each named path exists in-tree |
| Lesson steps fight the launcher rules | Breakpoints armed too early silently do nothing (`AGENTS.md` rule 4) | §7.2's ordering constraint: breakpoint before run; never during the boot window |
| A lesson cannot run without tools, so CI skips the interesting ones | Rot goes unnoticed | Split lessons by `needs:`; the emulator-free lessons must still pass everywhere, and the emulator ones run on the Linux/macOS CI legs that build the fork |
| Scope: 233 reference entries is a second content project | The teaching layer never finishes | Explicitly phased (§13 L2/L4); v1 covers lessons-touched entries only |
| Teaching text hard-codes one scheme and lies under the other | A tutorial that tells a user to press F10 for step is wrong on the Common scheme | Decision 9: generated text carries `{key:…}` tokens resolved from the live actions; committed sources name actions and keep only scheme-independent keys |

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

**L0–L3 landed** (2026-10-16): the front-door fix, the lesson format and its parser, the
runner with the six new control ops, the Lesson dock and the `Learn` preset, the welcome
doorway, five lessons, the diagnostic-explanation column, and the remote verbs and MCP
tools. L4 — the reference tables' `Example:`/`Used in:`/`Typical use` across all 233
entries, and further lessons — is deliberately unstarted.

§1.2's defect was verified by execution before the fix (the control-socket probe quoted
there) and the fix is pinned by `welcomeDemoRowOpensTheSource` (`tests/tst_gui.cpp`), which
clicks a demo row and requires the source to open — the row used to open the project file
instead, and no test had ever clicked one. §7.1.1's polling rules were
verified against a real session: the first implementation of the runner failed its own
lesson with `expected at hello.s:16, found hello.s:11`, and `tst_lessons` is what said so.
