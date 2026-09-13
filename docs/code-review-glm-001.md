# Code review — GLM-001

**Reviewed:** `master` @ 39b60d4 ("debug views: watchpoints, a stack view, and hardware registers")
**Date:** 2026-09-12
**Scope:** full source tree (`src/` ~12,400 lines), tests, CMake, CI, packaging scripts, docs
**Method:** read every source file; ran the full test suite locally (all 8 suites pass against
real Hatari 2.6.1 + vasm 1.x + vlink, 30.7 s); wrote and executed a reproduction test for the
top finding; verified a fix for it on a patched copy outside the repository.

---

## Summary

This is a well-engineered codebase with unusually strong documentation and test discipline. The
debug transport (the hardest part of the design) is a carefully reasoned state machine that
matches documented, verified Hatari behaviours; the parser layer is split into pure functions so
it is testable without an emulator; and cross-platform details (MSVC struct/class mangling,
`.exe` suffix discovery, session-dir deletion guards, short-write checks) are handled with a
level of care most projects never reach. `docs/PLAN.md`'s verified-fact ledger is a genuinely
good engineering practice, and the comments consistently record *why*, with references back to
the plan and to Hatari's own source.

But the review found one **verified, user-visible break in the core debug loop**: breakpoints set
before Run never arm, and the editor never follows the PC. The two integration points that were
supposed to wire the (correct, well-tested) pieces together are missing in the application —
while every individual piece passes its tests, and the end-to-end emulator suite manually
performs the wiring the app itself never does. This is exactly the gap the project's own test
philosophy warns about ("a green run does not overstate what was verified"): the tests
demonstrate the *components*, but the one test that launches a session through `MainWindow`
never sets a breakpoint, so the missing wiring was invisible.

Findings, in priority order:

| # | Severity | Finding |
|---|---|---|
| 1 | **P1** | Breakpoints set before Run never arm; editor never follows the PC — two missing links in `MainWindow` (verified by execution, fix verified) |
| 2 | P2 | `EmulatorHost::start()` does not reset all per-session state — cross-session framing corruption |
| 3 | P2 | Manual Stop leaves the whole UI in "stopped" state — no `stoppedChanged(false)`/`runningChanged(false)` ever fires |
| 4 | P2 | Every stop queues the memory/stack/hardware refresh three times — ~9 redundant debugger commands per stop |
| 5 | P3 | vlink warnings are recorded with Error severity (wrong capture group) |
| 6 | P3 | Stack view's "return address?" heuristic silently disabled without a data section |
| 7 | P3 | Watchpoint firing is never verified end-to-end against a live Hatari |
| 8 | P3 | `tst_remotecontrol` missing from the Windows console-subsystem fix-up list |
| 9 | Nits | Duplicate includes, duplicated CMake source entries, and a handful of small robustness items |

---

Inline comments for the top findings are also attached at the exact code locations; this document
is the full review.

---

## P1 — Breakpoints set before Run never arm; the editor never follows the PC

**Verified by execution.** A test that opens a looping program, sets a gutter breakpoint at
line 3, presses Run, waits for the entry stop, and resumes — the breakpoint never fires (the
program idles in its loop and no further stop ever arrives). The full repro lives in
`/tmp/pist-review-scratch/` (ephemeral; the essential steps are below) and is small enough to
become a permanent `tst_gui` case.

There are two independent root causes; both must be fixed.

### Root cause A: the arming chain is a race the attach sequence always loses

`MainWindow::onDebuggerStopped` (src/ui/MainWindow.cpp:1074–1104) queues `symbols prg`,
`info basepage`, `r`, `d`, then does:

```cpp
QTimer::singleShot(0, this, [this] {
    if (m_bases.isValid())
        armBreakpoints();
    else
        m_log->appendPlainText(
            tr("[breakpoints] no program base page yet; breakpoints will arm on the next stop"));
});
```

A zero-delay timer fires on the *next pass* of the event loop. But `m_bases` is only set when the
`info basepage` response is parsed, which requires: dispatch → Hatari response → prompt → the
40 ms settle timer → `completeCurrent` → `parseBasepage` → `stateUpdated`. That is many event
loop turns and at least 40 ms away. Meanwhile `launchEmulator` reset `m_bases` unconditionally
(src/ui/MainWindow.cpp:1018), so at the entry stop the check is *always* false. The
`armBreakpoints()` call is dead code, and the log message's promise — "will arm on the next
stop" — is not implemented anywhere: `onDebuggerStopped` early-returns on `m_sessionArmed`
(src/ui/MainWindow.cpp:1076), and nothing else arms on a stop.

The user-visible symptom has a cruel twist: `BreakpointPanel` will eventually show such
breakpoints as *"no code on this line"*, because `armBreakpoints()` *is* reachable through
`toggleBreakpointAtLine` and friends — where it runs and resolves nothing (see root cause B).

**Fix (verified):** drop the timer and arm when the bases actually arrive — in
`onStateUpdated`, once `m_bases` is set from the `info basepage` response:

```cpp
if (m_sessionArmed && !m_breakpointsArmedThisSession)
    armBreakpoints();
```

with `m_breakpointsArmedThisSession` set inside `armBreakpoints()` and reset in
`launchEmulator` alongside the existing per-session resets.

### Root cause B: `ProgramLineMap::setLiveBases()` is never called by the application

```
$ grep -rn setLiveBases src/ tests/
src/build/ProgramLineMap.h:60          ← declaration
src/build/ProgramLineMap.cpp:39        ← definition
tests/tst_emulatorhost.cpp:617         ← the only caller in the tree is a test
```

`MainWindow::onStateUpdated` (src/ui/MainWindow.cpp:1389–1401) copies the live basepage into
`m_bases` but never hands it to `m_programMap`. `ProgramLineMap::lineFor` and `addressFor`
therefore return false forever (`m_resolved` stays false), with three consequences:

1. `planBreakpoints` resolves no line to an address — every breakpoint is "unresolved",
   zero `b` commands are issued (this is why fix A alone still failed my repro).
2. `locationFromPc` (src/ui/MainWindow.cpp:1475–1499) always falls through to
   `clearCurrentExecutionLine()` — **the "editor follows the program counter" behaviour in the
   README and demo never activates.**
3. Breakpoint conditions edited while stopped re-arm to nothing.

The pieces are all correct and tested: `LineMap` round-trips addresses in `tst_gui`,
`planBreakpoints` is covered in `tst_debug`, `ProgramLineMap` composition in `tst_link`, and
`tst_emulatorhost::sourceLineBreakpointFiresAndResolvesBack` proves the *whole loop* — by
building the map, calling `map.setLiveBases(bases)`, and arming **manually**, replicating exactly
the wiring the app never performs. The only `MainWindow`-level session test
(`tst_gui::runStartsAnEmulatorSession`) checks that the session starts and stops at entry; it
never sets a breakpoint, so the missing wiring is invisible to CI.

**Fix (verified):** add `m_programMap.setLiveBases(m_bases);` in `onStateUpdated` once the bases
are valid. With both fixes applied, my repro passes: the breakpoint set before Run fires at
exactly the expected address.

### Recommended regression guard

Add to `tst_gui`, next to `runStartsAnEmulatorSession`: set a breakpoint via
`toggleBreakpointAtLine` *before* `run`, resume after the entry stop, and assert a second stop
arrives (the breakpoint hit); assert `editor->currentExecutionLine() > 0` at the stop. That one
test would have caught both root causes.

---

## P2 — `EmulatorHost::start()` does not reset all per-session state

src/emu/EmulatorHost.cpp:276–295 resets `m_queue`, `m_promptCount`, `m_promptTarget`,
`m_awaitingEntryPrompt`, `m_stdoutText` — but not:

- `m_owedPrompts` — a step-over that timed out in session 1 leaves the count positive; session 2
  then silently swallows that many real prompts before dispatching anything (src/emu/EmulatorHost.cpp:443),
  delaying the first command by the whole entry sequence or worse.
- `m_stderrBuffer`, `m_stderrAtDispatch` — a stale buffer (with a trailing `> ` on non-readline
  builds, i.e. Windows) is prepended to the new session's stderr. If the stale
  `m_stderrAtDispatch` is larger than the fresh buffer, stderr prompts are ignored until the
  buffer grows past the stale watermark — commands hang until the 10 s timeout, on exactly the
  platform (Windows) that relies on the stderr prompt.
- `m_socketBuffer` — a partial embed-size report from session 1 can be concatenated with
  session 2's first report and parsed as junk.
- `m_settleTimer` — not stopped in `stop()`; a pending settle can fire into the new session.
  Harmless today only because `completeCurrent` checks `m_haveCurrent`.

The test suite never runs two sessions on one `EmulatorHost` instance (every test constructs a
fresh host), so none of this is exercised. Fix: move all transport state resets into a single
`resetTransport()` called from `start()`, and stop the settle timer in `stop()`.

---

## P2 — Manual Stop leaves the UI and remote clients in "stopped" state

`EmulatorHost::stop()` (src/emu/EmulatorHost.cpp:361–378) calls `m_process->disconnect(this)`
*before* `kill()`, which disconnects the `finished` handler — the only code that emits
`stoppedChanged(false)` and `runningChanged(false)` (src/emu/EmulatorHost.cpp:334–344).
Consequences of pressing Stop (toolbar), `stop` over remote control, or closing the window:

- Step/Step Over/Continue stay enabled; `isStopped()` stays true. Clicking Step then yields
  "No emulator session is running." in the status bar.
- The status label keeps its last text ("Stopped in debugger").
- Remote-control clients are never told the session ended: `sessionRunningChanged(false)` is
  emitted by a handler wired to `runningChanged` (src/ui/MainWindow.cpp:95–107), which never
  fires. `m_sessionArmed` also stays true (same handler resets it).

The tests call `host->stop()` directly and never assert UI state afterwards, so this is
invisible to CI. Fix: emit `stoppedChanged(false)`/`runningChanged(false)` in `stop()` (or
disconnect only *after* the process has finished, and kill first).

---

## P2 — Every stop refreshes the memory/stack/hardware views three times

`EmulatorHost` emits `stateUpdated` once per *parsed response*: `parseRegisters`
(src/emu/EmulatorHost.cpp:824), `parseBasepage` (:841) and `parseDisassembly` (:875) each emit
it. `MainWindow::onStateUpdated` (src/ui/MainWindow.cpp:1407–1417) reacts to *every* emission by
queueing a memory dump, a stack dump, and `info <subject>` — so the four commands of the attach
sequence (or the three of a refresh after Step) fan out into 9 extra debugger commands, 3
identical copies of each. At the 40 ms settle per command, that is roughly half a second of pure
queue churn added to every stop, and the same writes duplicated to Hatari.

Fix options: emit `stateUpdated` once per *refresh batch* (have `refresh()` own the composition
and parse into `m_state`, emitting after the last command), or debounce in `onStateUpdated`
(e.g. a 0-delay single-shot that coalesces the three arrivals). The first is cleaner and also
makes `MachineState` updates atomic for any future listener.

---

## P3 — vlink warnings are recorded with Error severity

src/build/BuildService.cpp:333–334. The located linker regex puts the severity word in a
**non-capturing** group, so `captured(1)` is the numeric code:

```cpp
R"(^(?:Fatal error|Error|Warning) (\d+):\s*...)"     // group 1 = (\d+)
...
d.severity = located.captured(1).startsWith(QLatin1String("Warning"))   // always false
```

Every located vlink diagnostic is therefore an Error, including warnings; the Problems pane and
the red-wave underline overstate them. (The `plainRe` branch right below captures the word and
is correct.) Fix: capture the word — `^(Fatal error|Error|Warning) (\d+):` — and shift the group
indices; add a `tst_parsers` case with a vlink *warning* line, since the existing tests pin only
errors.

---

## P3 — Stack view's return-address heuristic depends on a data section existing

`MainWindow` passes the *data* base where `StackView::setStackDump` expects the *text end*
(src/ui/MainWindow.cpp:371 → src/ui/StackView.cpp:79):

```cpp
m_stack->setStackDump(sp, response, m_lastState.textBase, m_lastState.dataBase);
// StackView: value >= textBase && value < textEnd  →  "return address?"
```

It works as an approximation while a data section follows text, but with no data section
(`dataBase` 0 or equal to text) the range is empty and the "return address?" annotation silently
disappears. The real text extent is available — the listing's `(start-end)` header, already
parsed into `LineMap` — so pass that (or the map's resolved text span) instead.

---

## P3 — Watchpoint firing is never verified end-to-end

`Watchpoint::command()` builds the self-inequality breakpoint `b ($addr).w ! ($addr).w`
(src/debug/Watchpoint.h:28–32) on the documented-but-subtle premise that Hatari's change
tracking compares against the previous evaluation. That premise is only ever string-tested:
`tst_remotecontrol` pins input validation, and no emulator test arms a watchpoint and asserts a
write breaks in. Given that this is the project's only watchpoint mechanism, and its own
definition of done is "verified against the real thing", add an emulator test mirroring
`sourceLineBreakpointFiresAndResolvesBack`: arm a `.w` watch on a label, resume, store a
*different* value, assert the stop — and ideally also assert a write of the *same* value does
not stop, pinning the documented caveat.

---

## P3 — `tst_remotecontrol` missing from the Windows console-subsystem fix-up

CMakeLists.txt:472 lists `tst_parsers tst_tosrom tst_debug tst_settings tst_gui tst_emulatorhost
tst_link` in the `WIN32_EXECUTABLE FALSE` loop — `tst_remotecontrol` is not there. It links
`Qt6::Widgets` via `qt_add_executable`, so it is GUI-subsystem on Windows and its output is
detached: the exact silent-failure mode the loop exists to prevent (recorded in
docs/PLAN.md §"Verified by CI"). One word to fix.

---

## Nits

- **src/ui/MainWindow.cpp:32–33, 37–38** — `<QFileInfo>` and `<QMenuBar>` are each included twice.
- **CMakeLists.txt** — duplicated source entries: `BreakpointPanel.cpp` ×2 and `FileBrowser.cpp` ×3 in
  `tst_gui`; the LineMap/LinkMap/ProgramLineMap trio twice in `tst_debug`; `Toolchain.cpp` twice in
  `tst_settings`. CMake silently deduplicates, but it hides review noise and suggests copy-paste drift.
- **src/build/BuildService.h:124** — `bool m_running = false;` is unused (`isRunning()` uses `m_process`).
- **src/ui/MainWindow.h:141–143** — orphaned doc comment ("Per-session working directory…") left above
  `canEmbedDisplay()`; `makeSessionDir()` below it has none.
- **src/ui/MainWindow.cpp:581–586** (`openProject`) — the implied source path is computed by stripping
  `kProjectSuffix` characters from the chosen path; picking a file through the "All files" filter
  produces an arbitrary truncation. Guard on the actual suffix before stripping.
- **src/ui/MainWindow.cpp:1344–1354** (`editBreakpointCondition`) — right-clicking *Edit condition…* on a
  line with no breakpoint creates one via `toggleBreakpointAtLine` and then opens the dialog; cancelling
  the dialog leaves a condition-less breakpoint behind. Create only after `accepted`.
- **src/ui/MainWindow.cpp:1230–1234** (`addWatchpointAddress`) — calls `armBreakpoints()` unconditionally;
  with no session running, `EmulatorHost::command` emits "No emulator session is running." once per armed
  command into the log/status bar. Gate on `m_host->isRunning()`.
- **src/ui/MemoryView.cpp:206–208** — the status line reports `rows.size() * kRowBytes` bytes, overcounting
  when the final row is short (Hatari rounds dumps up to rows, so this is usually invisible, but the number
  can be wrong after a truncated dump).
- **src/editor/CodeEditor.cpp:198–206** (`gotoLine`) — calls `setFocus()`; combined with
  `locationFromPc` running on every `stateUpdated`, the editor steals focus three times per stop
  (see P2 fan-out). Also worth skipping the recenter when the target line is unchanged.
- **src/ui/MainWindow.cpp:71–73, 189** — `toolchain::findAssembler()` (which may spawn vasm twice to probe
  a version) and `probeHatari()` (two more processes) run synchronously in the `MainWindow` constructor;
  with a hung binary on PATH this stalls startup for up to ~20 s. Typically fast; worth moving off the
  constructor eventually.
- **src/emu/EmulatorHost.cpp:389–396** — bounding `m_stdoutText` to 64 KiB can drop prompt occurrences:
  `before` is counted on the untrimmed text and `after` on the trimmed one, so a swallowed prompt stalls the
  queue until the 10 s timeout. Rare (needs 64 KiB of stdout between reads), but the trim could count prompts
  in the dropped prefix and subtract.
- **src/ui/MainWindow.cpp:1289–1298** — breakpoints are keyed by base name (`QFileInfo::fileName()`), so two
  linked modules with the same file name in different directories collide in the panel and gutter. Edge case;
  worth a comment at minimum.
- **demo/hello.pistproject** — untracked and not ignored (the demo's `.prg`/`.lst` are correctly ignored).
  Either commit it (project files "travel with the project" per the README) or it stays as permanent
  `git status` noise.
- **src/control/RemoteControl.cpp:96–132** — the nested `QEventLoop` for `build`/`run` admits reentrant
  commands from any client while it spins, and a modal `QMessageBox` from the launch path (e.g.
  "Emulator not found") blocks automation inside the loop until the 120 s timeout. Localhost-only and
  opt-in, so the posture is fine; but for an agent-facing protocol, consider suppressing modal dialogs while
  a remote session is connected and reporting the failure on the socket instead.

---

## What is done well (worth keeping as the project grows)

- **The transport state machine** (prompt framing, entry-dump wait, owed prompts, stderr settle
  window) is the hardest part of the design and it is handled with unusual care — every subtle
  rule cites the observation that produced it (docs/PLAN.md §9's ledger is referenced from code
  comments with `file:line` precision into Hatari itself).
- **Test honesty.** Skips are distinguishes from passes (`PIST_REQUIRE_EMULATOR`), integration
  tests run the real Hatari and vasm rather than mocks, and failing tests dump the captured
  emulator dialogue (tst_emulatorhost `cleanup()`).
- **Pure-function separation** (`planBreakpoints`, `LineMap`, `LinkMap`, `parseMemoryDump`) makes
  the tricky address arithmetic testable without an emulator — which is also why this review
  could pin the P1 so precisely: every layer *around* the bug is already proven.
- **Cross-platform diligence** is empirical, not assumed: the MSVC struct/class mangling note in
  EmulatorHost.h, `QStandardPaths::findExecutable` for `.exe` discovery, `QStandardPaths::TempLocation`
  instead of `/tmp`, the console-subsystem test fix-up, and the AGL/Qt macOS pin all come with the
  failure that motivated them.
- **Failure-path care** in small places: short-write checks on settings save and editor save,
  deleting a stale `.prg` before building, refusing `removeSessionDir` outside the session base,
  and the linker-missing pre-flight in `MainWindow::build()`.

## Suggested next steps, in order

1. Land the P1 fix (chained arming + `setLiveBases` wiring) and add the `tst_gui` regression
   test that sets a breakpoint before Run and asserts the hit plus the execution line.
2. Reset all transport state in `EmulatorHost::start()` (P2 #2).
3. Emit stop/running changes on manual `stop()` (P2 #3).
4. Coalesce the per-response `stateUpdated` fan-out (P2 #4) — this will also make stepping feel
   noticeably snappier.
5. The P3s and nits as drive-by fixes; the watchpoint emulator test (P3 #7) is the one with real
   value beyond tidiness.
