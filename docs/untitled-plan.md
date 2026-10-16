# Untitled plan: every document is a real file

Spec for removing the untitled assembly buffer as a document state, replacing
File ▸ New with a real create-file transaction, giving the zero-document state
a welcome view, and defining what happens when an open file is renamed or
deleted on disk. Written so that an agent (or human) can implement the whole
job from this document without re-deriving it. Design decided (2026-10-16);
landed in one change set (2026-10-16) — see the Status section at the end and
the ledger entries this change recorded in `docs/PLAN.md` §9.

Line references are as of the day this was written and precede the change; use
git to re-anchor them if they drift.

## 1. Problem

User report:

> When no code windows are open it shows an "untitled" window, which is a pain
> as a user invariably tries to run it, or save it, which gets complicated. A
> file is always associated with a file that exists. If a user wants a new file
> they create it in the directory sidebar, and File ▸ New should bring up a
> selector for name and location. When there's no file open, show a welcome
> view instead.

### Why it happens today (verified facts)

- **File ▸ New makes a phantom.** `m_actNewFile`
  (`src/ui/MainWindow.cpp:962-965`) calls `addEditorTab(QString())`: an editor
  whose file path is empty, displayed as `untitled`
  (`src/editor/CodeEditor.cpp:1262`). No file exists anywhere.
- **The Run path lies to the user.** `buildSourcePath()`
  (`MainWindow.cpp:3395-3406`) skips an untitled focused editor and silently
  falls back to the project's stored `sourceFile`, then to any other open
  editor's path. A user typing in an untitled tab who presses Run watches vasm
  assemble *something else* with no indication that their buffer was skipped.
- **Every downstream feature has to guard against the phantom.** Breakpoint,
  run-to-cursor, blame and save plumbing carry `filePath().isEmpty()` checks
  all over `MainWindow.cpp` (17 sites on the day of writing — lines 4031,
  4243, 4293 and 4305 are representative). Breakpoints are file:line because
  GEMDOS relocates the program each run (§5 of `docs/PLAN.md`), so a buffer
  with no path has no coordinates at all.
- **Save turns into Save-As on the phantom.** `saveFile()`
  (`MainWindow.cpp:3359-3385`) and `maybeSaveEditor()`
  (`MainWindow.cpp:2811-2848`) both fall into a Save-As file dialog when the
  path is empty; the comment at 3367-3372 records a real failure where a
  failed write left the buffer quietly untitled while the user believed the
  save had happened.
- **Remote control cannot name the phantom.** The `save` command
  (`src/control/RemoteControl.cpp:685-689`) errors by design ("an untitled
  document can't be named from here"), degrading an entire automation surface
  for the sake of the greeting tab.
- **Startup can land on the phantom.** When there is no last source (or it no
  longer exists), nothing reopens and the pristine untitled tab stands there
  (`MainWindow.cpp:583-587`, `MainWindow.cpp:2889-2896`).
- **The phantom is hand-swept by special cases.** `isPristineEditor()`
  (`MainWindow.cpp:163-165`) defines "empty path, unmodified"; both
  `addEditorTab` (601-615) and `addImageTab` (635-641) contain bespoke logic
  whose only purpose is to not strand that pristine tab.
- **No file watcher exists anywhere.** `CodeEditor` has no `QFileSystemWatcher`
  (verified by grep on the day of writing). A file deleted or renamed on disk
  while open goes undetected; the next `saveFile()` recreates the file
  wholesale, so today the models just don't meet.
- **The sidebar's New File is real but half-blind.** The context-menu entry
  (`src/ui/FileBrowser.cpp:1084`, 1108-1118) asks a bare name with
  `QInputDialog::getText` and creates the file — but neither opens it in an
  editor nor reveals it in the sidebar, and the name is validated only after
  submission ("The name may be invalid or already exist", line 1116-1118).
- **The app already has the pattern this plan generalises.**
  `MainWindow::newImageIn()` (`MainWindow.cpp:3408-3439`) opens
  `NewImageDialog` — name plus directory, prefilled by
  `suggestedImageDirectory()` (3413-3430) — and creates the image through it.
- **The demo projects exist.** `demo/hello.pistproject`,
  `demo/keyred.pistproject` and `demo/spritedemo/sprites.pistproject` ship in
  the repository; `run.sh` opens `demo/hello.s`.

### Why the phantom must die rather than be reworked

An `.s` file's only purpose is to be assembled *from disk*, next to a `.PRG`
— the build, the launch, the line map, the breakpoints, the git blame lane,
the floppy write-back and remote control are all anchored to a real path.
"Name it later" buys nothing in this domain; there is no workflow in PiST
where an untitled buffer can even be assembled. The template picker (§4)
makes naming-at-creation cheap enough to lose nothing.

## 2. Goals

1. Zero-document is a real, designed state — a welcome view — never a phantom
   editor pretending to be one.
2. File ▸ New is a transaction: name, location, template, then the file
   exists on disk, is open in a tab, and is revealed in the sidebar.
3. The Run path stops falling back silently; what you build is what you see.
4. Renaming a file from inside the app follows the open tab through every
   path-anchored structure; deleting a file on disk while it is open prompts
   the user instead of silently stranding the editor.
5. Every untitled guard branch dies along the way (~17 sites in
   `MainWindow.cpp` plus the specified others) rather than remaining dead code
   beside the new model.

Non-goals:

- No changes to the build/launch pipeline itself, the debug transport, or the
  breakpoint semantics (§5 of `docs/PLAN.md`); this plan only touches the
  document model around them.
- The `ImageEditor` keeps its deliberate untitled scratch sprites
  (`src/ui/ImageEditor.h:126` region extraction writes a `.pim` unnamed —
  `src/ui/ImageEditor.cpp:1023`); that is an image workflow, not a source
  document, and is out of scope (see §9 open items).
- No welcome-page settings, themes or layouts: it is a fixed, small surface.
- No telemetry beyond counters written to the user's own `QSettings` (§6).

## 3. Decisions made (2026-10-16)

1. The untitled buffer is removed as a source-document state. Every editor tab
   maps to a file on disk.
2. File ▸ New becomes a dialog (name + location + template), reusing the
   `NewImageDialog` pattern; the sidebar context-menu creation is brought into
   line with it.
3. Template set v1, four entries: blank listing, GEMDOS hello, keyboard/input
   skeleton, image-include listing — seeded from the demo sources (§4).
4. After creation the file is opened in a tab *and* revealed in the sidebar.
5. A welcome view fills the central area whenever zero documents are open;
   it is a plain widget, not a tab, dock or web view.
6. Startup keeps the auto-reopen of the last source (`MainWindow.cpp:583-587`
   unchanged); the welcome view is for genuinely empty states and deliberate
   close-all.
7. Lifetime usage counters are kept and displayed as a small shelf on the
   welcome view, alongside "last session" facts (§6).
8. The demo link on the welcome view opens `demo/hello.pistproject`; when the
   recent list is empty (first run), the three demo projects are listed as
   the "Open a project" rows.
9. Renaming a file follows the tab through the path-anchored stores; deleting
   an open file's backing prompts Close tab / Save a copy… / Cancel (§7).
10. "Save a copy…" (the delete-rescue) becomes the only Save-As-shaped
    gesture left in the app — a rescue valve for an accident, not a everyday
    save.

## 4. The New File transaction (`NewFileDialog`)

A small dialog in the shape of `NewImageDialog`:

- **Name** field, extension defaulting to `.s` from the chosen template;
  free-form override allowed (`asm`, `x68` etc.).
- **Directory** field with a Browse button, prefilled by the same chain
  `suggestedImageDirectory()` uses (`MainWindow.cpp:3413-3430`): the focused
  document's directory, else the sidebar's root, else the last source's
  directory, else home. When a project is open, its directory heads the chain.
- **Template** picker, v1 exactly four:
  1. *Blank listing* — the purist's empty file.
  2. *GEMDOS hello* — shaped on `demo/hello.s`; the Ctrl+N → Enter → Run →
     TOS boots arc for a first-time user.
  3. *Keyboard/input skeleton* — shaped on `demo/keyred.s`.
  4. *Image-include listing* — the `demo/spritedemo/sprites.s` pipeline shape,
     which shows the `.dat` hand-off to the image editor.
- **Validation as-you-type**: empty name, illegal characters, and an
  existing target all marked before Accept. A file longer than the 8+3 GEMDOS
  convention draws a passive note (it assembles fine from the host; only
  floppy-image destinations care), never a refusal.
- **On accept**: create the file by writing the template bytes through
  `files::write()` (`src/support/FileWrite.h` — the staged-write rule;
  the Never-Truncate invariant), then `openPath(path)` (which raises an
  already-open document instead of duplicating — `MainWindow.cpp:2922-2932`),
  then reveal it in the sidebar. Record the path via
  `settings::rememberLastSource()` so the next launch reopens it.
- `m_actNewFile` keeps the `QKeySequence::New` shortcut and objectName
  `newFileAction`; only its slot changes.
- The FileBrowser context-menu "New File…" opens this same dialog rooted at
  the context directory (`FileBrowser.cpp:1084` and 1108-1118 are replaced),
  gaining open-and-reveal and fixing the post-hoc name validation.

## 5. The welcome view

A plain widget (`WelcomeView` in `src/ui/`) shown as the central area whenever
the tab count is zero. Not a `QTabWidget` tab (never in the Ctrl+Tab cycle),
not a dock, not `QWebEngine` — plain Qt widgets so both the view and its tests
run under `QT_QPA_PLATFORM=offscreen`.

Content, top to bottom:

1. **Recent files / projects as clickable lines** — `settings::recentSources()`
   (`src/project/ProjectSettings.h:173-186`), capped at ten, each opening the
   file, not a menu. One click reopens where a menu behind a menu used to be.
2. **Actions row** — Open…, New File…, New Image…: the welcome view doubles
   as the discoverable home for the document model.
3. **Readiness row** — toolchain and TOS ROM detected or missing, with a
   Setup… button wired to the first-run setup dialog. Genuinely useful on a
   first run and the day a distro update eats vasm.
4. **Demo rows** — on a first run (empty recent list) the three demo projects
   (`hello`, `keyred`, `spritedemo`) are listed to open in one click; they
   disappear behind the recents as those accumulate, rather than a permanent
   hard-coded link racing the MRU the user builds.
5. **Stats shelf** (§6) — small, in one corner, not a panel.

Behaviour rules:

- It appears when the last tab closes and is replaced the instant any document
  opens; it carries no per-document state — no gutter wiring, no breakpoint
  handlers, nothing persisted.
- It must be reachable deliberately too: a Close-file affordance on the last
  tab must leave the window on the welcome view (verify the Close actions'
  routing during implementation; if only tab ×-buttons reach zero tabs, add
  File ▸ Close File / Close All).
- During a live debug session the welcome view replaces the editor's text for
  the *user* but not the session's home: the debug docks stay populated and
  the session is not torn down by closing files. The path-guarded features
  (run-to-cursor, breakpoint binding) are already refused by the existing
  guards and simply lie fallow; the execution marker has nowhere to live.
- Startup keeps the auto-reopen of the last source; the welcome view shows
  only when that path is empty or does not exist.

## 6. The stats shelf

Two tiers, both local to the user's `QSettings` — nothing leaves the machine:

- **Lifetime counters**, kept for the gamification value (a number that goes
  up is the only reason many people keep a learning diary). Counted next to
  the existing `settings::` accessors (`src/project/ProjectSettings.*`), one
  accessor per key so a key cannot be spelled two ways (the MIN-53 pattern):
  sessions (main-window starts), builds, runs, files opened, and the
  first-launch date. Counting starts when this lands, so day one counts.
- **Last-session facts** as orientation rather than vanity: the last opened
  file, that session's build count, and the last build's outcome. Held in
  memory during the session and flushed once at teardown; a crash loses the
  summary, not the work.

Presentation: three or four numbers in one corner block, Atari-flavoured
copy where it comes free ("1,024 builds — a full double-sided disk"). Not a
dashboard; the welcome view is a doorway, not a simulator briefing screen.

## 7. Disk change: rename and delete

### 7.1 A document-level watcher

A small dedicated class (so it can be tested headless, without a window)
handed every open editor's path:

- **Re-arm after our own writes.** `files::write()` replaces files by staging
  and renaming over them, which is exactly the operation that drops an
  inotify watch. After every successful `saveFile()`, re-add the path.
- **Distinguish self-replace from real deletion.** On a changed/deleted
  signal, check `QFileInfo::exists()` first: an existing path is our own
  atomic replace (react by re-adding the watch only), a missing path is a
  real deletion.
- External rename is treated by the watcher as delete-then-potentially-new;
  the sidebar's in-app rename bypasses the prompt entirely and emits the
  follow event instead (§7.2).

### 7.2 Rename: the tab follows the file

The path is load-bearing, so a rename event fans out exactly:

1. The editor's own path (title, bookmarking, save target).
2. The breakpoint store, keyed file:line and persisted across sessions —
   migrate the per-file entries in place, or breakpoints vanish on the next
   run (unit-tested in `tst_debug`).
3. `.pistproject` `sourceFile` when it names the renamed file — `buildSourcePath()`
   falls back to it, so a stale entry strands the project's run target
   (`MainWindow.cpp:3395-3406`).
4. `settings::rememberLastSource()` and the recents list.
5. Blame lane refresh.

### 7.3 Delete: the rescue prompt

An open file's backing vanishing from disk is a background event, not a user
gesture — the user may be mid-typing — so it is a modal prompt:

- **Close tab** — the file is gone, the tab goes with it; lands on the
  welcome view if it was the last.
- **Save a copy…** — a normal file dialog; the buffer's bytes land at the
  chosen path and the editor rebinds to it (this is deliberately Save-As
  semantics: the buffer stays real-file-bound). This is the only
  Save-As-shaped gesture left in the app after §8, offered exactly when the
  user wants it.
- **Cancel** — the editor is left as it is, still path-bound to a name that
  no longer exists; a later save recreates the file, which is the pre-existing
  saveFile() semantics. This is accepted deliberately: not-untitled is about
  no document *starting* life without a name, not about making an in-flight
  editor impossible to keep alive after the user refused the rescue.

## 8. Removal list

Everything below dies on purpose; each item exists only because the phantom
made it necessary:

- `m_actNewFile`'s `addEditorTab(QString())` slot (`MainWindow.cpp:965`).
- `isPristineEditor()` (`MainWindow.cpp:163-165`) and both special cases in
  `addEditorTab` (601-615) and `addImageTab` (635-641).
- The empty-path branch of `saveFile()` (`MainWindow.cpp:3359-3378`) — Save
  becomes unconditionally "write to the known path"; `maybeSaveEditor()`'s
  branch (2828-2833) likewise.
- `CodeEditor::displayName()`'s `untitled` fallback
  (`src/editor/CodeEditor.cpp:1262`) — unreachable by construction.
- The remote-control `save` untitled comment and error framing
  (`RemoteControl.cpp:685-689`) — the error becomes "the write failed",
  because a document always has somewhere to go.
- The per-branch `filePath().isEmpty()` guards across `MainWindow.cpp`
  (17 sites, e.g. 4031, 4243, 4293, 4305) — collapse to path-required
  assertions or plain removal.
- `addEditorTab()` requires a non-empty path after the change; a debug
  assertion documents the invariant instead of a runtime guard.

## 9. Test strategy

House rules apply: one test only where a plausible bug would fail it; the
full suite stays green; every run under `QT_QPA_PLATFORM=offscreen`.

- **Welcome view**: integration tests in `tests/tst_gui` assert that closing
  the last document tab leaves the central area on the welcome view and that
  opening a file replaces it — the same assertions the pristine-tab
  behaviour obsoleted.
- **New File dialog**: split the dialog's non-visual work (path derivation,
  validation, template bytes, creation) into a helper class the dialog
  shells, so `tst_gui` can exercise creation without `exec()`-ing a modal.
  Assert: the file exists with the template's bytes, a tab is open on it, the
  sidebar reveals it.
- **Templates**: the shipped template text is asserted to be non-empty and
  (where it has a buildable shape) assembled once in the emulator-supplied
  integration test — a template that drifted into a non-building state should
  fail CI, not a first-time user.
- **Watcher**: the dedicated §7.1 class is unit-tested headless against real
  temp files: create → rename under it → delete under it; re-arm behaviour
  after an atomic replace (stage temp, rename over the watched path) is
  asserted explicitly, because that is exactly the Qt behaviour this design
  suspected (§10).
- **Rename propagation**: the breakpoint-store migration gets a unit test in
  `tst_debug` (rename moves the file's breakpoints; other files' entries are
  untouched); the `.pistproject` `sourceFile` rewrite gets one wherever the
  settings tests live today.
- **Debug-loop features** (breakpoints on a freshly created template file,
  run-to-cursor there) are proven against a real emulator run per the
  house rule — the template-picker tests reuse the existing
  emulator-present integration scaffolding.

## 10. Phasing

- **Phase A — the swap** (one coherent change set): New File dialog with the
  four templates, the welcome view skeleton (recents, actions, readiness,
  demo rows), and the entire §8 removal list. These cannot be separated:
  removing the untitled buffer requires a zero-document destination, and the
  welcome view skeleton is that destination.
- **Phase B — stats**: counters plus the shelf and last-session facts.
  Adds no behavioural risk; can trail A indefinitely.
- **Phase C — disk changes**: the §7 watcher, rename propagation and delete
  rescue. Deliberately last and independently testable: the hidden complexity
  (inotify re-arm, breakpoint migration) should not gate the user-visible win
  of Phase A.

## 11. Verified elsewhere / to verify during implementation

Verified facts used above live in §1 with their references. Claims made in
this document that deserve runtime evidence before or during Phase C — each
should be promoted into `docs/PLAN.md` §9 once measured:

- The exact `QFileSystemWatcher` semantics around atomic replaces on this
  kernel/Qt pair: whether the watch survives a rename-over, whether
  `fileChanged` fires, and whether a missing watch silently eats later
  renames of the same path. Re-arm-after-save is the designed remedy, but
  §9's watcher test must prove it, not assume it.
- Whether a Close-file menu action already reaches the zero-tab state
  (§5 behaviour rules) or whether Close File / Close All must be added.
- Whether the emulator-present integration scaffolding can reuse the
  template files directly or whether the tests need their own fixture copy.

Open items deliberately deferred (review after Phase C):

- The `ImageEditor`'s untitled scratch sprites (region extraction,
  `src/ui/ImageEditor.h:126`): same treatment (an image-save dialog at
  extraction time) if the asymmetry confuses anyone, not on principle.
- `run.sh` still opens `demo/hello.s` rather than
  `demo/hello.pistproject`; harmless today, may be switched with the
  welcome view's demo row for a consistent first-run arc.

## Status

**Landed** (2026-10-16) — all three phases in one change set rather than the
A/B/C split §10 proposed, because phase A's removal of the untitled buffer
needs its replacement (the welcome view) in the same stroke. This document is
kept as the spec that was implemented; its line references were written before
the change and are historical.

Three of its premises did not survive contact with the code — the `.pistproject`
`sourceFile` key, breakpoints persisted across sessions, and the ImageEditor's
region extraction — and the `QFileSystemWatcher` behaviour §11 asked to be
measured was measured. All four are recorded in `docs/PLAN.md` §9 (the
verified-vs-unverified ledger), not absorbed silently.
