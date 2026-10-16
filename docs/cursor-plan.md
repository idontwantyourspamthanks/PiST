# Cursor plan: decoupling the edit caret from the program counter

Spec for restructuring how the editor reacts to the program counter during a
debug session. Written so that an agent (or human) can implement the whole job
from this document without re-deriving it. Design decided; implementation not
started.

## 1. Problem

User report:

> "when I press the play button, the editor moves the cursor to the beginning
> of my .s file"

### Why it happens today (verified facts)

`MainWindow::locationFromPc()` (`src/ui/MainWindow.cpp:4507-4555`) runs on
**every** machine-state stop, including the entry stop moments after Run —
see `MainWindow::onStateUpdated()` ending in the call at line 4431. The entry
PC maps through `m_programMap` to the program's first code line, which sits
near the top of the `.s` file, and the function then performs four actions
welded together:

1. Raises the tab of the file the PC landed in (line 4551), opening the tab
   when it is not already open (lines 4535-4542).
2. Sets the execution-line highlight — `CodeEditor::setCurrentExecutionLine`
   (line 4552). This is an extra-selection *marker*
   (`src/editor/CodeEditor.cpp:452-460`), not the caret.
3. **`CodeEditor::gotoLine()`** (line 4553) — the culprit: it moves the real
   text cursor, centres the view, and calls `setFocus()`, stealing editing
   focus (`src/editor/CodeEditor.cpp:630-638`).
4. Updates the session/caret chips (lines 4546, 4554).

So the report is not a bug but a deliberate design: "the editor follows the
program counter at every stop". The injury is that action 3 exists at all on
the entry stop, and that follow-and-editing share one cursor with no state in
which the marker is on one line and the caret is elsewhere.

### Relevant existing plumbing (verified)

- The status bar already anticipates decoupled cursors: when the caret and the
  PC differ while stopped, `MainWindow::updateCaretChip()`
  (`MainWindow.cpp:2554-2558`) renders `file: caret N · PC M`. The session chip
  (`MainWindow.cpp:2519-2539`) separately reports `Stopped — file:line`.
- The execution-line marker is cleared on session teardown
  (`clearSessionViews`, `MainWindow.cpp:366-374`) and on PC-unmapped paths
  (lines 4509-4528).
- The caret is additionally overloaded by: Run-to-Cursor
  (`runToCursor`, action at `MainWindow.cpp:1114`), gutter-click breakpoint
  toggling (`toggleBreakpointAtLine`, line 2416), the profiler's start line
  (`src/ui/ProfilerController.cpp:101`), the Go-to bar
  (`src/editor/CodeEditor.cpp:312-324`), and post-build error navigation
  (`MainWindow.cpp:1641`). All of these should mean the *edit* caret — which
  is an argument for decoupling, not against.
- `tests/tst_gui.cpp` integration tests assert both the execution marker and
  editor caret positions after real stops in several places (e.g. lines
  1229, 2723-2798 `editorLine` lambda). Some of those assert the
  caret-follows-PC behaviour this spec changes; they must be revisited, not
  blanket-updated (§8).

## 2. Goals and non-goals

Goals:

1. Pressing Run (and any *passive* stop: breakpoint hit, watchpoint hit) leaves
   the edit caret, the viewport scroll, and editor focus exactly where the
   user left them; the PC shows as a marker instead.
2. Deliberately chasing execution still gives the "caret walks the code"
   feel — it just requires the user to start the chase, instead of happening
   without consent.
3. No restore/undo machinery: the invariant *the edit caret is only ever moved
   by the user (or a user-initiated navigation)* replaces any
   save-position/restore-position logic.
4. Existing user-initiated navigations (Run-to-Cursor, error jump, Go-to bar,
   profiler) keep their meaning and behaviour, all targeting the edit caret.

Non-goals:

- No settings surface for follow behaviour (revisit only if experience fails;
  §6 fallback).
- No scrollbar minimap/PC tick, disassembly-view integration, or new marker
  visuals beyond §5.
- No change to the debug transport, remote-control protocol, or MCP shim.

## 3. The design: placements vs. movements, ownership state

One session-scoped boolean and one class of asymmetry.

### 3.1 State

- `m_pcFollow` (session-scoped, `bool`), meaning *the caret's position is
  owned by the PC*.
- `m_pcPlaced` (session-scoped, `bool`): whether the PC has been placed in
  this session yet.

Reset both in `clearSessionViews()` (`MainWindow.cpp:366`) alongside
`m_stoppedLine` — a new session starts fresh.

### 3.2 The asymmetry

**Placements do not move the caret; movements do.** The first PC observation
in a session is a *placement*; every PC change after that is a *movement*.

- **Placement** (first stop of a session: entry stop, or first breakpoint hit
  when stop-at-entry is off): set the execution marker, update chips, set
  `m_pcPlaced = true`. The caret, scroll, focus, and tab layout are untouched.
  This is the reported bug, fixed by construction.
- **Movement** (every stop after the first):
  - If `m_pcFollow == false` → marker only, plus chips. Nothing else moves.
  - If `m_pcFollow == true` → move the caret to the PC line, reveal it, and
    raise the tab containing it (§4). The `m_pcFollow` release logic must not
    fire on our own move — see §4.2.

### 3.3 Ownership transitions

- **Release (user claims the caret):** any caret-position change originating
  from a user gesture or a user-initiated navigation — typing, clicking,
  arrow keys, Go-to bar, post-build error jump. Implementation: the follow
  path performs its caret move under a loop-back guard flag so its own
  `cursorPositionChanged` does *not* release; every other transition releases
  (`m_pcFollow = false`).
- **Engage (user hands the caret to the PC), explicit only:**
  - Clicking the session chip (`m_statusSession`) while stopped, or
  - Clicking the execution marker's gutter wedge on the PC line, or
  - The Run menu action `Follow execution` (new).
  Engage is transient navigation: it moves the caret to the current PC line
  now (revealing/rasing as §4) and sets `m_pcFollow = true`. The next user
  caret move releases again, re-establishing the invariant that only the user
  moves the caret while editing.

Rationale for engage-not-jump-always: after engage, the next PC movement
brings the caret along anyway, so the user may re-engage without leaving their
editing position immediately; but every engagement affordance listed above is
itself something you'd expect to *go to* what you clicked, so engage also
performs the jump. Documented, unambiguous both times.

Why placement-exemption is necessary (do not simplify it away): with a purely
geometric hitch test ("caret == previous PC line → follow"), the entry stop
no longer moves the caret, so after it the caret and PC diverge once and for
all — stepping would then never follow, losing the desirable behaviour. The
placement/movement distinction preserves stepping with no preferences and no
restore state.

### 3.4 Summary table

| Event | Caret | Scroll/focus | Marker | Tab | Chips |
|---|---|---|---|---|---|
| Run pressed → first stop | untouched | untouched | set at PC | untouched | updated |
| Later stop, released | untouched | untouched | moved to PC | untouched | updated |
| Later stop, following | moved to PC | reveal per §4 | moved to PC | raised if PC in another file | updated |
| PC unmapped (library, `info basepage` pc=0) | untouched | untouched | cleared | untouched | updated |
| Session ended (Stop/crash/quit) | untouched | untouched | cleared on all editors | untouched | "Not running" |
| Any user caret move | as user did | as user did | kept | — | caret chip |
| Engage affordance | moved to PC | reveal | kept/refreshed | raised | updated |

## 4. Sub-behaviours

### 4.1 Scroll policy

- Following stops: if the PC line is already fully visible, do not scroll;
  otherwise centre it. Never scroll when the caret is released — the marker
  may sit off-screen; the session chip carries the location.
- PC-follow-driven scrolling and rendering must not steal focus. Today
  `gotoLine()` calls `setFocus()`: that call stays for user-initiated
  navigation (Go-to bar, error jump) and must not execute on the PC follow
  path. Split the behaviour when implementing; keeping `gotoLine` as the
  user-action entry and giving the follow path its own reveal logic is the
  expected shape.

### 4.2 Release guard

`locationFromPc`'s caret move sets a member flag (e.g.
`m_pcFollowInProgress`) before `setTextCursor`, cleared after; while it is
set, a one-shot connection/latch swallows the `cursorPositionChanged` that the
move itself produces. Verify the guard against both editors' paths when the
PC moves *between* files: the caret move happens on the target editor after
-tab-raise, and only that editor's `cursorPositionChanged` must be guarded.

### 4.3 Tab raising and opening

- Following: PC in a file not currently raised → raise it; open it when closed
  (existing behaviour, lines 4541-4542, kept on the follow path only).
- Released: do **not** raise, do **not** open. The marker is still applied to
  that editor if it is open, so switching back shows the marker; otherwise the
  chips carry the information.
- Placement (first stop): never raises or opens — overrides everything in
  this subsection.

### 4.4 Existing navigations keep the edit caret

Unchanged and explicitly out of the follow system: Run-to-Cursor (which after
this change reads honestly as "run until *my* line"), gutter breakpoint
toggles, error-line jump, profiler start line, Go-to bar. If any of them is
triggered while following, they are user actions and therefore release the
caret (§3.3) in the same stroke.

### 4.5 Compatibility notes

- Remote-control/MCP waits for `program counter moved to line N` key off the
  session's stop event and the execution *marker* (see comment at
  `MainWindow.cpp:358-365`, which fixed exactly that class of race). They must
  never be re-pointed at caret position. After this change, marker movement
  continues on every mapped stop, so nothing in the protocol contract moves.
- `m_stoppedFile`/`m_stoppedLine` keep meaning "the PC's last mapped place",
  regardless of caret ownership; chips and the marker both derive from them as
  today.

## 5. Marker visibility

The existing full-width execution-line background
(`CodeEditor.cpp:452-460`, `m_theme.executionLine`) stays as THE PC indicator.
Necessary refinements:

- The marker must read as "the program is here", not as a stray selection.
  Requirement: when the caret is on a different line, the marker stays
  visually distinct from the caret's line/current-line painting. A gutter
  wedge (▶) in the lane left of the line numbers on the marker line is the
  preferred addition; it doubles as the engage affordance of §3.3. Exact
  visual/side is left to implementation review against the existing theme
  (`m_theme`), but the wedge must not collide with blame-lane or breakpoint
  dot geometry (`CodeEditor.cpp:640-~660`).
- Marker clearance on session end already exists and must keep working for all
  open editors (`clearSessionViews`).

## 6. Rejected alternatives

Recorded so the next person does not re-litigate them from the report alone.

1. **Status quo** ("follow at every stop"). The reported injury; falsified by
   the mixed event: asynchronous stops yank the caret out from under the
   keyboard while typing.
2. **Never follow; marker only.** Loses the genuinely liked "caret walks the
   code while stepping" behaviour; converts every step into manual arrowing.
3. **Follow always + restore caret at session end + a "go back" button.**
   Rejected on four counts: (a) it fixes the aftermath, not the injury — the
   yank still happens at every stop; (b) it permits the worst case, typing at
   the moment a stop arrives, where input lands on the wrong line before any
   restoration occurs; (c) restoration must record position, anchor, and the
   original tab (the PC can roam into includes), diverges when the file is
   edited during the run, and costs a second navigation state for strictly
   less protection than "the caret is only ever moved by the user"; (d) the
   back button exists to repair damage the design itself inflicts. The one
   good half — an explicit, deliberate re-engage affordance — survives in
   §3.3.
4. **Per-cause rules** (follow on steps, not on breakpoint hits). Equivalent
   expressive power to this design but with a hidden state machine tied to
   how each stop was caused; harder to explain, test, and debug than
   ownership that the user can see and flip in one gesture.
5. **A settings toggle.** Fallback only: if, after living with the hitch
   behaviour, either direction surprises, `View → Follow execution` becomes
   the toggle and §3.3 stays as the default. Do not ship preferences
   speculatively.

## 7. Implementation sketch

Files/symbols expected to change (names are guidance, not contract):

- `src/ui/MainWindow.cpp/h`
  - New members: `m_pcFollow`, `m_pcPlaced`, `m_pcFollowInProgress`.
  - `clearSessionViews()`: reset the two state members.
  - `locationFromPc()`: restructure per §3.2/§4 — placement check first;
    released path touches marker + chips only; following path moves the caret
    under the release guard; engage affordances (§3.3) wired to the session
    chip click, the gutter wedge, and a new Run-menu action.
- `src/editor/CodeEditor.cpp/h`
  - `gotoLine()` keeps user-action semantics (caret + centre + focus).
  - New follow-path entry (caret move without focus, scroll-only-if-invisible)
    or a parameterised variant; whichever keeps the diff smallest.
  - Gutter wedge painting for the execution line; click → engage signal
    (analogous to the existing `gutterClicked`).
- Tests: §8. Docs: §9.

## 8. Test plan

Rules of the house apply: only where a plausible bug would fail; assert
observable behaviour; integration tests run offscreen and skip cleanly
without Hatari/vasm/TOS (`PIST_TOS_DIR`, `PIST_REQUIRE_EMULATOR=1` makes
skips fail in CI); prove debug-loop behaviour against a real emulator run,
not a mock.

Targets (tst_gui unless stated):

1. **First stop leaves the caret.** Press Run, wait for the entry stop to
   place the marker (`currentExecutionLine()`), assert the editor caret
   sits where the test parked it and that scroll position/focus did not
   change. This is the regression for the user report.
2. **Following stop moves the caret.** Engage, then step a few times; assert
   the caret tracks the marker's lines (replacing the sites in tst_gui that
   currently assert this unconditionally — lines ~2723-2798 and any others
   found by searching for `textCursor().blockNumber()` after stop waits).
3. **Passive stop does not move a claimed caret.** Park the caret somewhere
   unrelated (post-placement), let a breakpoint hit, assert marker moved and
   caret did not.
4. **User caret move releases.** After engagement, move the caret (e.g.
   `setTextCursor` through a real key sequence or click); assert a
   subsequent stop leaves the caret. Cross-file variant: PC roams into an
   included file; assert no tab raise/open while released.
5. **Engage affordances.** Chip click / wedge click / menu action each move
   the caret to the PC and set following; a following stop then tracks.
6. **Focus discipline.** During a following stop, the editor must not steal
   focus (assert focus widget) — PC navigation is a background event.
7. **Unmapped PC clears the marker** (existing paths, lines 4509-4528) and
   leaves the caret alone.
8. **Marker cleared on session end for every open editor** (existing
   behaviour, keep a regression test if none covers multiple tabs).

## 9. Documentation updates (required)

- This file is the spec of record for the feature.
- `docs/ARCHITECTURE.md`: the debug-flow/module narrative describing
  `locationFromPc` ("the editor follows the program counter into whatever
  file it lands in") must be rewritten to the placement/movement/ownership
  model.
- `docs/PLAN.md`: this reverses recorded verified behaviour ("the editor
  follows the PC at every stop" is cited in code comments). Record the change
  and the reasoning in the §5/§9 ledger as mandated — contradictions of the
  plan go in the ledger, not silently absorbed.
- `README.md`: user-facing sentence in the debugging section: editing
  position is never disturbed by the emulator; execution location is shown
  as a marker, and `Follow execution` (Run menu / session chip / gutter
  wedge) hands the caret over deliberately.
- Code comments at `MainWindow.cpp:4530-4534` and wherever else "the editor
  follows the program counter" is stated get rewritten in the same pass.

## 10. Open questions

1. Exact engagement affordance set: keep all three of §3.3, or chip-click +
   menu only until the wedge proves itself visually?
2. Marker styling when the caret is not on it (does `executionLine` alone
   read unambiguously?) — blocked on visual review, see §5.
3. Whether `Run to Cursor` should additionally re-engage following (it is a
   deliberate "meet my caret" act) — default for now: it does not; revisit
   after living with the feature.
4. Any `tst_remotecontrol` assumptions about caret/editor focus during
   controlled sessions — audit alongside the test work in §8.
