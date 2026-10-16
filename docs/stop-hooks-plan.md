# Stop-hooks plan: scripted control at every debugger stop

*(File: `docs/stop-hooks-plan.md`.)*

The plan of record for **[glm-features.md §1](glm-features.md), scripted stop hooks** — a
user-supplied script that runs each time the debugger stops, can read and write memory and
registers, can log, and can decide to continue. Written 2026-10-17 on branch `v0.10.0`. Every claim
about the tree below was read from this branch; the parts that were not measured are listed in §15
rather than asserted.

FUTURE §10 held the idea and its analysis; glm-features §1 agreed it for development and recorded
Lua as the language choice. This document is the detail: the trigger point, the runner's discipline,
the API, the surfaces, the tests and the phases.

---

## 1. Problem

The debug loop is designed for a human at the keyboard: the debugger stops, the IDE renders the
state, the human edits a register or a byte, reads a dump, and continues. Every ingredient an
*automated* loop needs already exists in the tree — reads, writes, continue — and an external
program can drive them over the remote-control socket.

What is missing is the trigger. The remote protocol is request/response: a client that wants to
react to a stop must poll `state` until it notices one, and by then it has lost the stop it was
waiting for (`state` is a snapshot, not an edge). So the loop that trainers, harnesses and dynamic
analysis are made of — *at this stop, look at this, poke that, resume* — cannot be written today
without polling, and a poll is the wrong shape for it.

This is the emulator-scripting model FCEUX, Mesen and BizHawk standardised on, applied to PiST's
debug loop: the script lives outside the compiled program, so behaviour is extended without a
rebuild.

---

## 2. What exists (verified on this branch, 2026-10-17)

**The trigger.** Two sites emit `stoppedChanged(true)`, both in `src/emu/EmulatorHost.cpp`:

- `onPrompt()` (`:1054-1116`) — a prompt arriving with nothing in flight *is* the stop signal;
  `m_stopped = true; emit stoppedChanged(true); dispatchNext();` at `:1112-1114`.
- `handleStderrLine()` (`:1139-1161`) — the `"You have entered debug mode"` banner, the entry stop;
  the `m_awaitingEntryPrompt` guard is armed *before* the emit (`:1153-1161`).

There is exactly one production consumer: `MainWindow`'s handler
(`src/ui/MainWindow.cpp:1701-1746`), which toggles the session actions, publishes the watcher events,
and calls `DebugSessionController::onDebuggerStopped()`.

**The detailed stop.** The `stopped` event a watcher sees is published from
`MainWindow::onStateUpdated()` (`src/ui/MainWindow.cpp:4765-4769`) *once `state.regs.valid`*, with
`pc=0x…`, because the entry attach's standalone `info basepage` answer carries `pc = 0` and
publishing that told watchers the machine had stopped at address 0. It is the same code path that
installs the live bases and calls `armBreakpoints()`. The sink is `RemoteControl::publishEvent`
(wired in `src/main.cpp:200-201`), and the MCP shim relays it as a log notification.

**Writes while stopped.** `MainWindow::setMemoryByte` (`:4516`) and `setRegister` (`:4533`) gate on
`isStopped()`, log a refusal when not stopped, and go through the typed intents
`IDebugBackend::writeMemoryByte` / `writeRegister` (`src/emu/DebugBackend.h`) — never debugger text.
`MainWindow::debugCommand` (`:4476`) is free text and is *not* stop-gated.

**Reads.** Typed intents only: `requestMemoryDump()` → `memoryDumpReady(address, rows, tag)` (parsed
once, `src/emu/MemoryDump.h`), `dumpRegisters()` → `stateUpdated`, `readBasepage()`. There is **no
synchronous read and no typed single-byte read** — every read is a queued request whose answer
arrives on a signal.

**The transport's re-entrancy guards.** `EmulatorHost::dispatchNext()` is single-entry and defers
through `m_inStderr`/`m_inDispatch`/`m_dispatchPending` (`:495-512`), and its comment names the exact
case this feature will create: *"the pre-drain in the body can pull a prompt whose handlers
(`stoppedChanged` consumers queueing the entry attach) call back into `dispatchNext()` /
`processStderrData()` while this frame is still on the stack"*. `processStderrData()` is single-entry
too (`:589-604`) and returns early when already inside itself. `stoppedChanged(true)` is emitted from
inside both of these loops; its handlers run synchronously.

**The resume discipline.** `EmulatorHost::resume()` (`:789-820`) drains the queue and keeps only
entries whose `Pending::keepOnResume` is set — arms (`b …`) and `profile …` control commands, decided
for free text by `survivesResume()` (`src/emu/DebugBackend.h:78-83`). `c` is written only once the queue is
empty (`finishContinue()`, `:781-786`). That is invariant 5's resume-not-drop rule.

**The seams.** `DebugSessionController` (`src/ui/DebugSessionController.h`) owns the session's
lifecycle flags, the entry-stop attach, the arming gate and the step-out state, and reaches the
window only through a `Host` of callbacks — including `log` ("one line on the console"). The five
seams are constructed in `MainWindow`'s constructor and are drivable with no window at all.
`Host::log` is the console append (`m_log->appendPlainText`, the `QPlainTextEdit` at
`src/ui/MainWindow.cpp:2226-2228` in `consoleDock`).

**Configuration.** `.pistproject` is JSON, `kProjectFileVersion = 2`
(`src/project/ProjectSettings.cpp:25`), with nested `build` / `emulator` objects and paths resolved
relative to the project directory (`resolveRelativeToProject`, `:145-160`). `SettingsDialog` has the
list-of-paths UI patterns (`m_sources`, `m_includePaths`, `src/ui/SettingsDialog.cpp:175-222`) and the
browse helpers (`browseAssembler`, `addSource`, `addIncludePath`).

**The agent surface.** Verbs are one table, `control::kVerbs` (`src/control/ControlProtocol.h`),
dispatched by the `if`/`else-if` chain in `RemoteControl::execute()`
(`src/control/RemoteControl.cpp:402-820`); the operation itself lives on `control::ControlHost`,
implemented by `MainWindow`. The MCP shim declares tools in `McpServer::tools()`
(`src/control/mcp/McpServer.cpp:757-1040`) and maps each to a verb string in `handleToolsCall()`
(`:466-600`).

**Build and packaging.** Exactly one build-time dependency: Qt 6 (`CMakeLists.txt:34`). No
FetchContent, ExternalProject, `add_subdirectory` of vendored code and no submodules anywhere; the
external tools (vasm, vlink, Hatari, EmuTOS) are fetched by CI shell steps with pinned sha256 and are
never committed (`docs/PLAN.md:1088`). CI builds against Qt 6.8.1 and today selects only the
`qtmultimedia` module (`.github/workflows/ci.yml:117`, `.github/workflows/release.yml:255`);
`Qt6::Qml` is not linked anywhere. Licence
bookkeeping is `NOTICE`, `packaging/copyright` (DEP-5, layout-agnostic path patterns) and
`packaging/notices/*.txt`, with the shipped set **derived from the built artifact's linked libraries**
by `packaging/collect-notices.py` (`KNOWN_LIBS` at `:45`, `linked_libraries()` at `:72`,
`bundled_library_names()` at `:89`). The project installs no runtime libraries of its own; Qt's are
deployed by `qt_generate_deploy_app_script` into `lib/`.

**Tests.** Unit suites are declared in the root `CMakeLists.txt` through `pist_add_test()`
(`:565-568`); offscreen integration suites skip themselves unless the emulator, assembler and a ROM
are present (`tests/tst_emulatorhost.cpp:491-546`), and `PIST_REQUIRE_EMULATOR=1` turns a skip into a
failure. `tests/tst_emulatorhost.cpp:977-1070`
(`sourceLineBreakpointFiresAndResolvesBack`) is the model for a real-session assertion: assemble,
map, wait for the entry stop, arm, resume, then assert a debugger-visible effect at the second stop.

---

## 3. Goals

1. **G1** A configured script runs on every stop, after the machine is genuinely stopped and its
   state is known — not before.
2. **G2** The API is small, typed, and honest: every refusal names itself, and nothing reports a
   guess.
3. **G3** A script can continue, and cannot wedge the session or corrupt the debugger's framing.
4. **G4** Both consumers are served: the human (Run menu, `.pistproject`, the console) and the agent
   (a verb, an MCP tool, the stop event).
5. **G5** The engine choice is a measurement, not a preference — L0 produces numbers and a decision
   goes in the ledger.

## 4. Non-goals

- **No per-breakpoint scripts in v0.** Naming which breakpoint fired needs work of its own (§13, L2);
  the sheet's ordering is deliberately reversed here, and §5 D1 gives the reason.
- **No scripted UI.** A script cannot create docks, widgets, panes or documents. Its whole output is
  lines on the console and stops it asks for.
- **No file, network or process API in the script.** The documented API is the function list in §7
  and nothing else, whatever the engine's standard library happens to offer (§5 D6).
- **No multi-file scripts, `require`, modules or a shared cache** in v0. One file, loaded per stop.
- **No new read primitives.** A script uses the reads that exist (memory dumps, registers); no
  single-byte typed read, no disassembly API, no history API in v0.
- **No hooking anything but stops.** Not build completion, not session end, not the running edge.
- **No script debugger.** An error names file and line; there is no stepping through a script.
- **Nothing here touches watchpoints, symbol breakpoints or lesson content** — sheet §5 and §2 own
  those.

---

## 5. Decisions

**D1 — The trigger is the *published stop event*, not raw `stoppedChanged(true)`.**
The sheet's sketch ("on `stoppedChanged(true)`, evaluate the configured script") is a batch early.
At that moment the register and memory truth does not exist yet: `MainWindow`'s own handler *queues*
the attach (`symbols prg`, `info basepage`, `r`, `d`) from there, and at the entry stop the debugger
is still printing its dump (`m_awaitingEntryPrompt`). A script fired there would read the previous
stop's registers and write into a stop whose dump has not finished — exactly the class of mistake
MAJ-14 already cost the transport. The hook therefore fires where the detailed stop is published:
`MainWindow::onStateUpdated()`, once `state.regs.valid`, beside the watcher event
(`src/ui/MainWindow.cpp:4765-4769`).

**D2 — The runner executes from a queued invocation, outside the transport's emit stack.**
Scripted reads need to wait for a response, which means a nested event loop — the same shape
`RemoteControl`'s blocking verbs use. But a nested loop spun *inside* `completeCurrent()`'s
`emit stateUpdated(...)` cannot ever see its answer: `processStderrData()` returns early while
`m_inStderr` is set (`:589-604`), so the response it waits for can never be parsed. That is a hang,
not a stall. Queued invocation (`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` or an
equivalent 0-timer), not a synchronous call, and not a *timer race* in the arming sense: the machine
is held stopped, so there is no prompt to lose — the reason invariant 5 bans a zero-delay timer for
arming does not apply here, and the queued hop is what buys a safe nested loop.

**D3 — Reads may block; writes must complete before a continue.**
`resume()` prunes everything without `keepOnResume` (`:789-820`), and a memory or register *write* is
not marked keep. So a write queued in the same stack frame as a `continue()` would be dropped
silently. Marking writes keep is worse, not better: written after `c`, the debugger is running, and
Hatari defers the command to the *next* stop — the write would silently apply at the wrong time.
Rule: each scripted read and write waits for its own command to complete (with a timeout), and
`continue()` is **recorded** by the API and issued only after the script returns, i.e. after
everything it queued has finished, through the same path the UI's Continue uses.

**D4 — Engine: `QJSEngine` (`Qt6::Qml`) is the default candidate; Lua is the alternative, and L0
measures both before the pick is written down.**
The sheet chose Lua and named `QJSEngine` as the fallback "if packaging fights back". The evidence
says it does:

- The tree has no FetchContent, no submodule and no committed third-party code; every external
  dependency is fetched by CI shell steps with a pinned checksum. Vendoring Lua means inventing a
  fourth acquisition mechanism and applying it on three platforms.
- The licence set is **derived from the linked artifact** (`packaging/collect-notices.py:45,72,89,265`). A
  statically linked Lua is invisible to that derivation, so its notice would have to be hand-added
  and the release-time check could not verify it — the exact silent-gap shape this project's rules
  exist to avoid.
- `Qt6::Qml` is a shared module inside the deployment path already used for the rest of Qt, whose
  notice is generic over the Qt version. The expected cost is a `find_package` component and possibly
  one `modules:` line per workflow.

The costs are real and are what L0 measures: the installed size QtQml adds, and whether
`QJSEngine::setInterrupted()` can stop a runaway script safely (the function exists in Qt 6.8; its
safe usage is a spike item, §15 Q2). Lua's case stays on record — the audience already writes it
(BizHawk/Mesen), its count hook interrupts a runaway loop in-thread with certainty, and it is small.
If L0 finds QJSEngine cannot be interrupted or costs too much to ship, Lua wins and this section says
so with numbers instead of a preference.

**D5 — One script per project, opt-in, in `.pistproject`.** Keys `hooks.scriptPath` (relative to the
project directory, like `includePaths`), `hooks.enabled`, `hooks.autoContinue`. A list of scripts and
per-breakpoint bindings are L2/L3; one script keeps the API and the failure modes small enough to be
honest about.

**D6 — The API surface is the documentation, and the standard library is not.**
Even where the engine would allow more (Lua's `io`/`os`/`debug`/`package` if Lua is chosen), the
documented and supported surface is §7's function list; the README says exactly what is reachable. A
script is the user's own code on the user's own machine, so this is not a sandbox claim — it is a
"the docs are true" claim.

**D7 — `continue()` has a spelling problem in JavaScript.** `continue` is a reserved word, so the
name cannot be a bare global; `QJSEngine` would expose it as `globalThis.continue` with `resume()`
as the alias. The README will document whichever the pick requires. (A small cost of D4's default,
recorded here so it is not discovered late.)

---

## 6. The script model

- **The file.** One script in the project, referenced from `.pistproject`. Loaded and evaluated
  per stop; no engine survives a run, so a script cannot leak globals into the next stop and a
  runaway one leaves nothing behind.
- **The run.** On every stop, *including the entry stop* — that is where a trainer pokes an input
  before the program's own code reads it. By the time the runner executes, the attach has completed
  and the bases are live (they are installed in the same handler that publishes the stop, and the
  runner is queued behind it). The script's own commands therefore queue *behind* the attach's and
  the arming commands, which is the correct order: FIFO, and the arms keep their resume guarantee.
- **The stop's identity.** The runner exposes the stop's `pc`. It does **not** claim a reason
  (`breakpoint` / `step` / `pause`): the transport detects a prompt, and step-vs-breakpoint is not
  distinguishable from it today. A `reason` field arrives with L2's hit identification, not before —
  reporting a guess would be worse than reporting nothing.
- **Quiet by default.** The runner logs nothing per stop. It logs a script's own `log(msg)` and every
  error, both prefixed `[hook]`. A watchpoint-heavy run must not fill the console with hook lines.

## 7. The API (v0)

```
readmem(addr, len)      -> bytes   (len bytes as a string; refuses len < 1 or len > kMaxRead)
readreg(name)           -> number  (the stop's cached state when it is valid, else a typed read)
writemem(addr, value)              (one byte, via writeMemoryByte)
writereg(name, value)              (via writeRegister)
log(msg)                           (one line on the console, prefixed "[hook] ")
continue()                         (recorded; issued after the script returns — see D3)
```

- **Addresses** are numbers. **Register names** are exactly the set `writeRegister` accepts
  (the registers view's set), matched case-insensitively; an unknown name raises an error naming it.
  Whether `pc` is in that writable set is a test question, not an assumption (§15 Q4).
- **`pc` comes from the stop's own state**, so reading it costs no round-trip; that state is the
  machine's truth at the stop, which is what makes answering from it honest rather than an
  optimisation that can be wrong.
- **Every refusal raises**, naming the call and the reason: unknown register, out-of-range length, no
  session, machine not stopped. The runner reports it as `[hook] <file>:<line>: <message>` and leaves
  the machine stopped.
- **`sym(name)`** (label → address, resolved through the symbol table and the live bases) is L2, not
  v0. When it arrives it raises on an unresolvable name rather than returning 0.

## 8. The runner and the transport discipline

The rules, in the invariants style — these are correctness requirements, and each has the failure it
prevents:

- **R1 — Trigger only on the published stop** (D1). Prevents reads that are a batch stale and writes
  into an unfinished entry dump.
- **R2 — Run queued, never inside the transport's emit stack** (D2). Prevents the nested loop that
  can never see its response.
- **R3 — Every scripted intent goes through `IDebugBackend`'s typed intents**, never a raw process
  write — the MAJ-21 rule, unchanged: `writeMemoryByte`, `writeRegister`, `requestMemoryDump`,
  `dumpRegisters`, and the host's `resume()`. No caller builds debugger text.
- **R4 — Each read and write waits for its own completion, with a timeout.** On timeout the runner
  reports it, abandons the script, and leaves the machine stopped. It never continues on an unknown
  state.
- **R5 — `continue()` is deferred until the script returns and its queue is empty**, then issued
  through the same path the UI's Continue uses, so the arms queued at the entry stop survive
  (invariant 5). The runner never writes `c` itself, and `hooks.autoContinue` only decides whether
  the *script's* `continue()` is honoured without asking. A script that does not call `continue()`
  never resumes anything.
- **R6 — One run at a time.** A run-in-progress guard makes a second invocation (a manual Continue
  racing the queued hop) a logged refusal rather than a nested run.
- **R7 — The runner refuses when the machine is not stopped**, and says so once. A stop can end
  between the publication and the queued run; the runner checks rather than assumes.
- **R8 — Nothing backend-specific.** The runner speaks intents, so native and HRDB both work; L2
  proves it on the fork, because the sheet's model was verified against native only.

## 9. Configuration and the human surface

- **`.pistproject`**: a `hooks` object with `scriptPath`, `enabled`, `autoContinue` (D5). Adding keys
  is backward-compatible with version 2 — an older PiST ignores an unknown object rather than
  refusing the file, which is the behaviour to confirm in L1 (§15 Q3).
- **Run ▸ Stop hooks…** (`m_actStopHooks`, beside `m_actAddWatchpoint`) opens a small dialog built
  the way `SettingsDialog`'s fields are: an enabled checkbox, the script path with a Browse… button
  (`QFileDialog::getOpenFileName`), an auto-continue checkbox, and a read-only line showing the last
  run's outcome. It persists through `ProjectSettings` + `files::write()` — never a bare `QFile`
  write (invariant 16).
- **The console**: `[hook]` lines through the existing `Host::log` closure. They carry no `> ` echo:
  that prefix means "a command was typed here", and a script typed nothing.
- **README**: a *Stop hooks* section listing the API verbatim, the registers accepted, and the honest
  limits — no file API, a runaway script is interrupted, an error leaves the machine stopped.

## 10. The agent surface (L3)

The sheet's direction is to grow the verbs rather than build a chat widget. L3 ships the minimum
that makes an agent a first-class consumer: a `hook` verb (a block reply: enabled, path, and the last
run's outcome — ok, or the error's file:line and message), the matching `pist_hook` MCP tool, and a
`hook` field on the published `stopped` event so an agent learns a script ran and what it did. It is
gated on L1/L2 being in real use, not designed further here.

---

## 11. Failure, limits and honesty

| Condition | Behaviour | Who learns |
|---|---|---|
| Script file missing or unreadable | One refusal logged per session; the session is untouched | Console |
| Script raises | `[hook] <file>:<line>: <message>`; the machine stays stopped | Console |
| Runaway script | Interrupted at the budget; the engine is discarded; logged as an error naming the budget | Console |
| A read or write times out | The script is abandoned; the machine stays stopped | Console |
| Not stopped when the run starts | One logged refusal, no run | Console |
| Write refused (not stopped) | The backend's existing refusal (already logged) surfaces as the script's error | Console |
| `readmem` beyond `kMaxRead` | Raises; nothing is queued | Console |
| Symbol unresolvable (L2) | Raises naming the symbol; never 0 | Console |

The README's API list is the contract; if a capability is not in it, the docs do not claim it.

## 12. Testing

- **`tst_stophooks`** — a new unit suite (always runs, no emulator). The runner is a seam, so it is
  constructed against a fake `IDebugBackend` with no window at all. Cases: a read answers from a valid
  cached state without a round-trip and queues one when the state is invalid; **write-then-continue
  ordering** (the fake records that the write completed before `resume()` — the R5 bug is silent, so
  only a test catches it); a script error is reported with file and line and does not resume; a
  runaway script is interrupted; a non-stopped run is refused; a missing file yields one refusal; an
  out-of-range `readmem` raises.
- **`tst_emulatorhost`** — the sheet's acceptance test, on a real session:
  `sourceLineBreakpointFiresAndResolvesBack` (`:977-1070`) is the pattern. Stop at a line, have the
  script write a byte at a known address, then read that address back with `requestMemoryDump()` and
  assert the value; a second case has the script `continue()` and asserts the program reaches a
  second breakpoint.
- **`tst_gui`** (offscreen) — the Run ▸ Stop hooks… action exists and the dialog round-trips
  `.pistproject` (save → reload → same values); the console receives the `[hook]` line
  (`debugConsoleText()` backs the assertion, as the remote `console` verb does).
- **`tst_hrdb`** (L2) — one stop-hook case against the fork, skipping without `$PIST_HRDB_HATARI`.

None of these asserts wiring, forwarding or "it did not throw": each fails on a plausible bug —
stale state, silent drop, wedged session, wrong stop.

## 13. Phasing

**L0 — engine spike (throwaway, ~1 day).**
Measure, do not decide: the installed size QtQml adds to the Linux artifact; whether the `modules:`
inputs need it named (`.github/workflows/ci.yml:117`, `.github/workflows/release.yml:255`); whether a
runaway script can be interrupted
(and by what mechanism); whether a nested event loop at a queued post-stop invocation really can
perform a synchronous `readmem` on a real Hatari session. Acceptance: a script logs a line on a real
breakpoint stop, and the numbers are recorded in `docs/PLAN.md` §9 and §5 D4 of this document.
Nothing from L0 is merged except the measurements.

**L1 — the runner and the human surface (v0).**
`StopHookRunner` as a sixth seam (its own `Host`: backend, the stop's state, symbols, the console,
resume), the trigger wiring at the stop publication, the §7 API, the `.pistproject` keys, Run ▸ Stop
hooks…, the console prefix, the README section. Acceptance: `tst_stophooks` + the
`tst_emulatorhost` case + the `tst_gui` round-trip pass; a real session where a scripted write
visibly changes a value read back from the debugger; the full suite green offscreen.

**L2 — symbols, cause, and per-breakpoint scripts.**
`sym()`; the stop *cause* (the armed breakpoint or watchpoint whose resolved address equals the PC,
with the honest caveat that two identical addresses are one stop); a script name on a `Breakpoint`,
carried to the backend by the existing arming plan (`planBreakpoints()`, `armBreakpoints()`); the
`tst_hrdb` case. This is where the sheet's "named scripts on named breakpoints" lands.

**L3 — the agent surface** (§10): the `hook` verb, the MCP tool, the `hook` field on the stop event,
the README §Remote control entry.

Each phase ends by recording what it measured in `docs/PLAN.md` §9 — the engine numbers and the HRDB
behaviour in particular, both of which contradict nothing today but are not yet verified.

## 14. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| A nested loop spun inside the transport's emit stack hangs | High | R2; `tst_stophooks` asserts the runner is invoked queued |
| A scripted write is dropped by `resume()`, silently applying never or at the wrong stop | High | R5; the ordering assertion in `tst_stophooks` |
| A runaway script wedges the IDE | High | The run budget and engine discard; L0 measures the mechanism |
| A script reads a batch-stale state | Medium | D1's trigger; the cache is consulted only when valid (R4) |
| Console noise on every stop | Low | Silent unless the script logs or errors |
| QtQml's size or deployment surprises on one platform | Medium | L0 measures all three artifacts before L1 |
| HRDB's stop edges and `needsStop` gating differ from native's | Medium | Typed intents only (R8) + the L2 `tst_hrdb` case |
| The script API drifts from the remote verbs' semantics | Medium | The README API list is the contract; L3's verb reports the same facts |
| Users expect hooks while running | Low | Documented, and refused honestly by R7 |

## 15. Open questions (answer by measuring, not by reasoning)

1. **Engine** — §5 D4. Lua or `QJSEngine`, decided by the L0 numbers.
2. **Interruption** — can `QJSEngine::setInterrupted()` be driven safely from a watchdog while
   `evaluate()` runs on the UI thread? The function exists in Qt 6.8; the safe pattern is unverified.
   If it cannot, the fallback is running the engine on a worker thread with an interrupt — a real
   complication, and one of the things that would move the decision to Lua.
3. **Unknown keys in `.pistproject` version 2** — confirm an older PiST ignores the `hooks` object
   rather than refusing the file.
4. **The writable-register set** — exactly which names `writeRegister` accepts, and whether `pc` is
   among them; `writereg("pc", …)` refuses if it is not, rather than pretending.
5. **Correlating a hook with an agent's view** — is the published `stopped` event enough for an agent
   to attribute a run, or does L3 need a stop id? (Deferred to L3.)
6. **QtQml deployment** — does `qt_generate_deploy_app_script` pick up the module on all three
   platforms without extra install rules?

## 16. Relationship to other documents

- **[glm-features.md §1](glm-features.md)** — the agreed feature; this is its plan of record.
- **[FUTURE.md §10](FUTURE.md)** — the original analysis, superseded by glm-features §1 and kept.
- **[PLAN.md](PLAN.md) §3.3** (the transport) and **§5** (the launcher rules) — the invariants this
  document's §8 leans on: the channel split, prompt framing, `info basepage` arming, writes only
  while stopped. **§9** is where the measurements land.
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — the seam pattern (`DebugSessionController` and siblings)
  the runner follows, and the debug-surface description of the typed intents.
- **PLAN.md §11** — branching, commit messages and the definition of done this work follows.

## 17. Status

Planned 2026-10-17, branch `v0.10.0`. **L0 not started. Nothing in this document has been executed**;
its claims are source readings (cited above) and two external facts (the Lua licence and
`QJSEngine::setInterrupted`'s existence in Qt 6.8). The engine decision is deliberately left open
until L0 produces numbers.
