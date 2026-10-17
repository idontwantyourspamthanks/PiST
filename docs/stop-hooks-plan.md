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
(`src/ui/MainWindow.cpp:1734-1789`), which toggles the session actions, publishes the watcher events,
and calls `DebugSessionController::onDebuggerStopped()`.

**The detailed stop.** The `stopped` event a watcher sees is published from
`MainWindow::onStateUpdated()` (`src/ui/MainWindow.cpp:4939-4949`) *once `state.regs.valid`*, with
`pc=0x…`, because the entry attach's standalone `info basepage` answer carries `pc = 0` and
publishing that told watchers the machine had stopped at address 0. It is the same code path that
installs the live bases and calls `armBreakpoints()`. The sink is `RemoteControl::publishEvent`
(wired in `src/main.cpp:200-201`), and the MCP shim relays it as a log notification.

**Writes while stopped.** `MainWindow::setMemoryByte` (`:4690`) and `setRegister` (`:4707`) gate on
`isStopped()`, log a refusal when not stopped, and go through the typed intents
`IDebugBackend::writeMemoryByte` / `writeRegister` (`src/emu/DebugBackend.h`) — never debugger text.
`MainWindow::debugCommand` (`:4650`) is free text and is *not* stop-gated.

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
`src/ui/MainWindow.cpp:2260-2262` in `consoleDock`).

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
never committed (`docs/PLAN.md:1096`). CI builds against Qt 6.8.1 and today selects only the
`qtmultimedia` module (`.github/workflows/ci.yml:117`, `.github/workflows/release.yml:255`);
`Qt6::Qml` is not linked anywhere — and it stayed that way: the engine that landed is Lua (D4),
not Qt's. Licence
bookkeeping is `NOTICE`, `packaging/copyright` (DEP-5, layout-agnostic path patterns) and
`packaging/notices/*.txt`, with the shipped set **derived from the built artifact's linked libraries**
by `packaging/collect-notices.py` (`KNOWN_LIBS` at `:50`, `linked_libraries()` at `:88`,
`bundled_library_names()` at `:105`). The project installs no runtime libraries of its own; Qt's are
deployed by `qt_generate_deploy_app_script` into `lib/`.

**Tests.** Unit suites are declared in the root `CMakeLists.txt` through `pist_add_test()`
(`:649-652`); offscreen integration suites skip themselves unless the emulator, assembler and a ROM
are present (`tests/tst_emulatorhost.cpp:494-550`), and `PIST_REQUIRE_EMULATOR=1` turns a skip into a
failure. `tests/tst_emulatorhost.cpp:987-1094`
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
(`src/ui/MainWindow.cpp:4939-4949`).

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

**D4 — Engine: Lua 5.4.8, pinned by checksum and linked statically. Decided by measurement.**
The sheet chose Lua and named `QJSEngine` as the fallback "if packaging fights back". Both were
spiked as throwaway programs before this was written, and the numbers decided it:

| | Lua 5.4.8 | QJSEngine (Qt 6.8.1) |
|---|---|---|
| Shipped size | **+345 KiB** (`liblua.a` is 661 KiB, linked statically) | +8,770,048 B — exactly `libQt6Qml.so` (6,611,968) + `libQt6Network.so` (2,158,080); no QML plugin or import tree is needed (verified under `bwrap` with both trees masked) |
| Stopping a runaway script | count hook: same-thread, 20/20 aborts, 0.23–0.36 ms | only from another thread: 500.122 ms wall for a 500 ms budget, 0.051 ms latency, 8/8 — a same-thread `QTimer` **provably cannot fire**, measured firing 1452 ms after the 1952 ms script it was meant to stop |
| Where it comes from | one 374,332-byte tarball, sha256 `4f18ddae…`, 34 `.c` files, no build system | one Qt module — but a **dev** module: this machine's distro Qt has `QmlPlugins/` and no `Qt6QmlConfig.cmake`, and the official 6.8.1 kit lacks Multimedia instead, so no installed Qt here could build it without a change |
| API fit | `readmem` returns a byte-exact string; `continue()` is not a reserved word | an array of byte values (a JS string is UTF-16 and loses any byte above 0x7F); `continue` is reserved, so `globalThis.continue()` |

Two corrections to this document's earlier draft, which had preferred QJSEngine:

- *"Vendoring Lua invents a fourth acquisition mechanism"* was wrong. Every external dependency here
  is already fetched by a pinned URL + sha256 (vasm, vlink, Hatari, EmuTOS); `file(DOWNLOAD …
  EXPECTED_HASH …)` in `CMakeLists.txt` is that same rule applied to a linked library. The mechanism
  is new to the *build*, not to the project.
- The Lua-specific cost is narrower, and is stated rather than hidden: a **statically linked**
  library is invisible to `packaging/collect-notices.py`, which derives the shipped licence set from
  what the artifact loads (`:50,88,105,281`). Lua's MIT notice is therefore hand-listed and the
  release check cannot catch it going stale; the honest fix is a static-component rule in that script
  beside the notice text, and the script's own prose records why the artifact scan cannot see it.

What Lua costs: it is the first non-Qt code linked into the binary (today it links Qt and nothing
else — no Capstone; that is Hatari's external disassembler), and configure now fetches it. Offline
and packaging builds pass `-DPIST_LUA_TARBALL=<file>`; the checksum is verified either way.

**D5 — One script per project, opt-in, in `.pistproject`.** Keys `hooks.scriptPath` (relative to the
project directory, like `includePaths`), `hooks.enabled`, `hooks.autoContinue`. A list of scripts and
per-breakpoint bindings are L2/L3; one script keeps the API and the failure modes small enough to be
honest about.

**D6 — The API surface is the documentation, and the standard library is not.**
Lua's `io`, `os`, `debug` and `package` are never opened: the interpreter is created with `base`,
`table`, `string` and `math` and nothing else, so §7's function list *is* the whole reachable
surface. A script is the user's own code on the user's own machine, so this is not a sandbox claim —
it is a "the docs are true" claim.

**D7 — `continue()` is spelled as the sheet wrote it.** Lua reserves nothing here, so `continue()`
works as a bare global; `resume()` is bound to the same function because the surrounding prose uses
that word. The base library's `print` is rebound to the console line for the same reason a GUI app
cannot trust stdout to exist — a script that prints is not a script that lost its output.

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
  A single step counts too. `s` and `n` are not running→stopped transitions, so `stoppedChanged`
  never fires for them — `MainWindow::step` says exactly that — which is why the session controller
  carries a step flag beside its stop flag, consumed by the register batch that answers the step.
  *Step out* and *run to cursor* arm a breakpoint and resume, so they arrive as ordinary stops and
  need no flag of their own.
- **The stop's identity.** The runner exposes the stop's `pc`. It does **not** claim a reason
  (`breakpoint` / `step` / `pause`): the transport detects a prompt, and step-vs-breakpoint is not
  distinguishable from it today. A `reason` field arrives with L2's hit identification, not before —
  reporting a guess would be worse than reporting nothing.
- **Quiet by default.** The runner logs nothing per stop. It logs a script's own `log(msg)` and every
  error, both prefixed `[hook]`. A watchpoint-heavy run must not fill the console with hook lines.

## 7. The API (v0)

```
readmem(addr, len)      -> a string of len bytes (refuses len < 1 or len > 4096)
readreg(name)           -> an integer (the stop's own registers when valid, else a typed read)
writemem(addr, value)              (one byte, via writeMemoryByte)
writereg(name, value)              (via writeRegister)
log(msg)                           (one line on the console, prefixed "[hook] ")
continue()                         (recorded; issued after the script returns — see D3)
```

- **Addresses and values are integers**, and Lua's own checks refuse a non-number by naming the
  function and the argument; the range checks (`0..$FFFFFFFF`, `1..4096`, `0..255`) name the range.
  `readmem`'s result is a **Lua string**, which is byte-exact: `string.byte` reads a byte out of it
  and nothing passes through UTF-8 on the way.
- **Register names** are exactly the set `writeRegister` accepts (the registers view's set), matched
  case-insensitively; an unknown name raises an error naming it. `usp` and `isp` are readable but not
  writable — Hatari cannot set them by name, which is why the registers view leaves those cells
  read-only too — and the refusal says so.
- **`pc` comes from the stop's own state**, so reading it costs no round-trip; that state is the
  machine's truth at the stop, which is what makes answering from it honest rather than an
  optimisation that can be wrong. A scripted write makes the copy stale, and the next read then makes
  a real round-trip instead of answering from it.
- **`print` is bound to the console line**, and `resume` to the same function as `continue` (D7).
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
  the way `SettingsDialog`'s fields are: an enabled checkbox, the script path with Browse… and
  **New script…**, an auto-continue checkbox, and a read-only line showing the last run's outcome.
  *New script…* is the authoring path, because asking someone to invent a file elsewhere and then
  find it again is the friction this feature exists to remove: `MainWindow::createStopHookScript()`
  writes `hello.s` → `hello.hooks.lua` beside the source (through `files::write()`, never a bare
  `QFile` — invariant 16) from a starter whose comments *are* the API reference, opens it in an
  editor tab, and switches the hook on. An existing script is adopted, never overwritten, and an
  empty setting offers the conventional path when one already sits beside the source — the naming
  rule exists so nobody has to be told where a project's hook is. The rule also recognises its own
  output: with the script itself focused (it is "the source" by `buildSourcePath()`'s rule, whatever
  is in front of you), the path *is* that script rather than a source to name another after.
  Underneath it stays a value dialog: no create-handler, no button, which is how a test drives it
  with no window behind it, and why the creation itself is a public `MainWindow` operation the suite
  calls directly rather than something buried in a modal.
- **The editor**: a `.lua` file is painted by `LuaHighlighter` rather than `AsmHighlighter` —
  keywords, `--` and `--[[ ]]` comments, short and long strings, numbers, and the hook API names,
  all in the existing `EditorTheme` colours. `CodeEditor::installHighlighter()` picks by path and
  re-picks on `setFilePath()`, because a rename can change the extension; exactly one of the two
  highlighters is live, and both are owned by the document as the assembly one always was.
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
| A scripted write while the machine is running | Refused by the runner's own gate (`isStopped()` per operation) before the backend sees it: a command written while running would be deferred by Hatari to the *next* stop, a different stop from the script's | Console |
| `readmem` beyond `HookApi::kMaxReadBytes` | Raises; nothing is queued | Console |
| Symbol unresolvable (L2) | Raises naming the symbol; never 0 | Console |

The README's API list is the contract; if a capability is not in it, the docs do not claim it.

## 12. Testing

- **`tst_stophooks`** — built and green: 17 cases, no emulator and no window. The engine half runs
  the real Lua against a fake `HookApi` (byte-exact `readmem`, a caught refusal and an uncaught one,
  Lua's own argument refusal, a runaway script stopped inside its budget, a missing file, both spellings
  of `continue`, and the line an error came from). The runner half drives a fake `IDebugBackend`
  whose commands complete a turn later, as the transport's do, and asserts what only a test can see:
  the **write is acknowledged before the `continue()` it asked for** (R5 — the silent bug), the
  console is held across the run and released after it, a register read answers from the stop's own
  copy and makes a round-trip once a scripted write has staled it, a non-stopped run is refused, an
  unanswered command abandons the script without resuming, and a disabled hook does nothing at all.
- **`tst_settings`** — the `.pistproject` round-trip: the `hooks` object survives save/load, a
  relative script path resolves against the project directory, and a `hooks` value of the wrong type
  is refused by name rather than silently dropped.
- **`tst_emulatorhost`** — the sheet's acceptance test, on a real session:
  `sourceLineBreakpointFiresAndResolvesBack` (`:987-1094`) is the pattern. Stop at a line, have the
  script write a byte at a known address, then read that address back with `requestMemoryDump()` and
  assert the value.
- **`tst_gui`** (offscreen) — the doorway and the dialog:
  `runMenuOffersStopHooks` (the action exists *and* sits in the Run menu),
  `stopHooksDialogGatesItsFieldsOnWhetherItRuns`,
  `stopHooksDialogOffersToMakeAScript` (no handler means no button; a handler's path lands in the
  field and switches the hook on; a handler that fails changes nothing) and
  `createStopHookScriptWritesItBesideTheSource` (the file lands beside the source with the starter,
  opens in a tab, and is adopted rather than overwritten on a second ask).
- **`tst_hrdb`** (L2) — one stop-hook case against the fork, skipping without `$PIST_HRDB_HATARI`.

Not covered, and said so rather than implied: the dialog's **validation**. `accept()` refuses an
unreadable path through a modal warning, which an offscreen test cannot answer without hanging — the
same hazard that keeps modal dialogs out of this suite. What the dialog *decides* is covered:
`tst_gui`'s `stopHooksDialogGatesItsFieldsOnWhetherItRuns` pins that the path and auto-continue
fields are inert while the hook is off, and that the last-run line is never blank.

None of the rest asserts wiring, forwarding or "it did not throw": each fails on a plausible bug —
stale state, silent drop, wedged session, wrong stop.

## 13. Phasing

**L0 — engine spike (throwaway). Done 2026-10-17.**
Both engines were built as standalone programs and measured; the outcome is D4 and the numbers are in
`docs/PLAN.md` §9. What the spike did *not* measure: the sizes are Linux-only, and the CI `modules:`
question became moot once the Qt route was dropped on the interruption and API findings.

**L1 — the runner and the human surface (v0). Implemented; §12 is what proves it.**
`src/hooks/HookApi.h` (the contract the engine binds to), `src/hooks/StopHookEngine.{h,cpp}` (Lua,
pinned and static), `src/hooks/StopHookRunner.{h,cpp}` (the API over the typed intents,
the waits, the budget, the continue policy), the trigger at the stop publication plus the step flag
in `DebugSessionController`, the `hooks` object in `ProjectSettings`, Run ▸ Stop hooks… with
`StopHooksDialog`, the `[hook]` console prefix, the README section, `LuaHighlighter` in the editor,
and the pinned Lua build. The script is authored in the IDE too: **New script…** writes it beside
the source and opens it (§9), so the feature is usable without leaving PiST or inventing a file.

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
| A statically linked Lua is invisible to the release licence scan | Medium | The MIT notice is hand-listed and `collect-notices.py` gains a static-component rule for it; the limitation is written down in that script, not discovered later |
| Configure now fetches Lua, so an offline build needs the tarball | Low | `-DPIST_LUA_TARBALL=<file>`, hash-checked the same way; documented in the README's build section |
| HRDB's stop edges and `needsStop` gating differ from native's | Medium | Typed intents only (R8) + the L2 `tst_hrdb` case |
| The script API drifts from the remote verbs' semantics | Medium | The README API list is the contract; L3's verb reports the same facts |
| Users expect hooks while running | Low | Documented, and refused honestly by R7 |

## 15. Answers, and what remains open

1. **Engine — answered: Lua 5.4.8** (§5 D4), on the measured numbers rather than on preference.
2. **Interruption — answered by measurement.** Lua's count hook stops a runaway loop in-thread, 20/20,
   in 0.23–0.36 ms. The QJSEngine route needed a second thread and a sticky `setInterrupted` flag, and
   went out with the engine.
3. **Unknown keys in `.pistproject` version 2 — answered.** The loader reads the keys it knows and
   ignores the rest, so an older PiST opens a project carrying `hooks` without complaint; a `hooks`
   value of the wrong *type* is refused by name rather than dropped (`tst_settings`). The residual is
   the ordinary one for any newer key: a round-trip through an older PiST loses the setting, and
   Run ▸ Stop hooks… sets it again.
4. **The writable-register set — answered.** `d0`–`d7`, `a0`–`a7`, `pc` and `sr`, which is the set the
   registers view offers as editable; `usp` and `isp` are readable and refuse a write with Hatari's
   reason, because Hatari cannot set them by name (the registers view's own comment).
5. **Correlating a hook with an agent's view** — still open, deliberately: L3 decides whether the
   published `stopped` event is enough for an agent to attribute a run, or whether a stop id is
   needed.
6. **QtQml deployment** — withdrawn with the engine.

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

Planned 2026-10-17 on `v0.10.0`, implemented the same day on `feature/stop-hooks`. **L0 is decided
and its numbers are in `docs/PLAN.md` §9; L1 is implemented and green** — the unit suite, the
`.pistproject` round-trip, and the full suite run offscreen against a real emulator.

L2 (symbols, the stop's cause, per-breakpoint scripts) and L3 (the `hook` verb, the MCP tool, the
event field) are the next work on this branch, which is where the whole feature lives.

What is *not* verified here: the three platform archives (the feature's cost is a static library and a
configure-time fetch, but the artifacts themselves need CI), HRDB (L2), and the dialog's own
behaviour (§12 says why).
