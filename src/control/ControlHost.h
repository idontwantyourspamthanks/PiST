// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The seam between the remote-control protocol and the IDE it drives.
//
// RemoteControl drives a *user interface*: it starts builds and runs, edits the
// debug session, reads the panes as JSON and waits for the events those
// operations produce. It used to say so by name — the server included the
// window's own header and called the window's methods directly — which is the
// `control -> ui` include edge that made the module graph cyclic (MIN-86). This
// header is that edge inverted: the control layer names the operations and the
// events it needs, the UI implements them, and only the UI side knows what the
// window is.

#pragma once

#include "emu/MemoryDump.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include <functional>

namespace pist {
namespace control {

/// The IDE as the remote-control protocol sees it (MIN-86).
///
/// Every member is a verb RemoteControl serves or an event one of its verbs
/// waits on, and nothing else: the surface is the protocol's, not the
/// window's, which is what keeps the dependency pointing one way. The
/// implementer is the IDE's main window.
///
/// Deliberately *not* a QObject, and its events are subscriptions rather than
/// signals: a QObject-derived interface cannot be a *second* base of a QObject.
/// Such a class has two QObject subobjects, and the code moc generates for it
/// does not even compile (`QObject::d_ptr`, `QObject::qt_metacall` and friends
/// are ambiguous), quite apart from connect() looking a signal up in the
/// sender's own metaobject chain, which a second base is not part of. An
/// explicit subscription hook is the same QObject::connect made on the far side
/// of the interface, with the waiter as the connection's context object, so a
/// handler dies with the wait that installed it exactly as before.
class ControlHost
{
public:
    virtual ~ControlHost() = default;

    // --- verbs that run something the IDE already knows how to do ---------

    /// Open a source file by path without a dialog, for the remote `open`
    /// verb. False when it could not be opened: the remote caller has nobody
    /// to dismiss a modal, so the failure has to travel back as a value.
    virtual bool openPathQuiet(const QString &path) = 0;

    /// Build now; the answer is the buildCompleted event.
    virtual void build() = 0;
    /// Build with the project's *release* assembler arguments rather than the
    /// debug ones — the same build `Build Release` runs, and the only way a
    /// remote caller can produce a shipping `.PRG`. The answer is the
    /// buildCompleted event, exactly as for build().
    virtual void buildRelease() = 0;
    /// Build and, when it succeeds, start the emulator session; the answer is
    /// either sessionRunningChanged(true) or buildCompleted(false).
    virtual void run() = 0;
    virtual void stopSession() = 0;
    virtual void step() = 0;
    virtual void stepOver() = 0;
    virtual void resume() = 0;
    virtual void profileStart() = 0;
    /// Save and parse the collected profile. False when there was nothing to
    /// save, in which case no profileResultsReady follows. The answer is the
    /// profileResultsReady event.
    virtual bool profileStop() = 0;

    // --- verbs that edit the debug session ---------------------------------

    /// Toggle a breakpoint at a line of the current editor. False when there
    /// is no source file open, or the line number is invalid.
    virtual bool toggleBreakpointAtLine(int line) = 0;
    /// Toggle a breakpoint at a symbol's definition. `detail` carries the
    /// reply text either way ("ok <file>:<line> [= 0x…]" or an "error …").
    virtual bool toggleBreakpointAtLabel(const QString &name, QString *detail) = 0;
    /// Whether the current editor's file has an instruction at `line`, so a
    /// `breakpoint` reply can say when a breakpoint there cannot fire. True
    /// when there is no program map yet ("can't tell" is not "cannot fire").
    virtual bool lineHasCode(int line) const = 0;
    /// Write a register through the debugger, when stopped.
    virtual bool setRegister(const QString &regName, quint32 value) = 0;
    /// Write a memory byte through the debugger, when stopped. (Named for the
    /// intent rather than for the window's pane-aware method: the verb has no
    /// pane to refresh.)
    virtual bool writeMemoryByte(quint32 address, quint32 value) = 0;
    /// Add an address watchpoint from its text form ("$12345", "$12345.l").
    /// False, with `error` set, on a bad address.
    virtual bool addWatchpointAddress(const QString &text, QString *error) = 0;
    /// Save the current document without a dialog. False when no document is
    /// open, or when the write failed.
    virtual bool saveCurrentDocument() = 0;

    // --- typed debugger access --------------------------------------------

    /// Send a debugger command as written; the answer is debugCommandFinished.
    virtual void debugCommand(const QString &command) = 0;
    /// A memory read whose answer is data rather than a pane update; the
    /// answer is debugReadMemoryFinished, carrying the address asked about.
    virtual void debugReadMemory(quint32 address, int length) = 0;
    /// A disassembly read; the answer is debugReadFinished, carrying the
    /// address asked about.
    virtual void debugReadDisassembly(quint32 address) = 0;

    // --- state reads: the JSON and text verbs ------------------------------

    /// The build & debug console's contents.
    virtual QString debugConsoleText() const = 0;
    /// A plain-text snapshot of the machine state (registers, PC, running
    /// state).
    virtual QString stateSummary() const = 0;
    /// The current document as JSON {path, text}; empty when none is open.
    virtual QJsonObject documentJson() const = 0;
    /// The machine state as JSON {running, stopped, pc?, d?, a?, sr?}.
    virtual QJsonObject stateJson() const = 0;
    /// The Problems pane as JSON rows {file, line, message, severity}.
    virtual QJsonArray problemsJson() const = 0;
    /// Every open document as JSON rows {path, modified, current}.
    virtual QJsonArray tabsJson() const = 0;
    /// The build's symbols as JSON rows {name, file?, line?, address?},
    /// optionally name-filtered.
    virtual QJsonArray symbolsJson(const QString &filter) const = 0;
    /// The profiler's hot lines as JSON rows {line, count}, worst first.
    virtual QJsonArray profilerResultsJson() const = 0;

    /// Capture the IDE's window into `path` (the `screenshot` verb). False
    /// when the capture or the write failed. The capturing itself is the UI's:
    /// it needs the window handle and, under X11, the XGetImage path the
    /// embedder already owns.
    virtual bool saveScreenshot(const QString &path) = 0;

    // --- the teaching layer (docs/tutorial-plan.md §7.1) -------------------
    //
    // A lesson has to *point* at things, not only drive them: a step that says
    // "look at line 16" cannot if nothing can put the editor there. These are
    // the operations the lesson runner needs, defined once here so the panel,
    // the headless test and — later — an external agent share them.

    /// Put the caret on `line` of `path` and show it, without taking the
    /// keyboard: a lesson points at a line, it does not steal focus from the
    /// user reading it.
    virtual bool navigateToLine(const QString &path, int line) = 0;
    /// Create a file from a New File template, through the same transaction the
    /// New File dialog uses. False, with `error` set, on an unknown template or
    /// a failed write.
    virtual bool createFileFromTemplate(const QString &templateLabel, const QString &path,
                                        QString *error) = 0;
    /// Raise the instruction/OS-call reference dock on `topic` — an instruction
    /// mnemonic or an OS-call name; empty raises the dock itself.
    virtual bool raiseReference(const QString &topic) = 0;
    /// Put a breakpoint at `line` of the current document (`on`), or take one
    /// away (`!on`).
    ///
    /// A lesson *sets* rather than toggles: re-running a step must not remove
    /// the breakpoint it just added. Clearing is separate for the same reason —
    /// a lesson that moves its breakpoint on (a loop's `dbf` line, then the line
    /// the loop exits to) has to be able to take the first one away, and a
    /// continue that re-triggers it would never reach the second.
    virtual bool setBreakpointAtLine(int line, bool on) = 0;
    /// The same, at a symbol's definition. `detail` carries the reply either
    /// way, exactly as toggleBreakpointAtLabel documents it.
    virtual bool setBreakpointAtLabel(const QString &name, bool on, QString *detail) = 0;
    /// The source line the program counter maps to, for an `at file:line`
    /// checkpoint. False when the PC cannot be mapped to a line.
    virtual bool pcSourcePosition(QString *file, int *line) const = 0;

    // --- events ------------------------------------------------------------
    //
    // Each hook is a QObject::connect with `context` as the connection's
    // owner, returning its connection so the waiter can disconnect. The
    // context is what makes a subscription safe to abandon: a handler is
    // dropped with the event loop that was waiting on it.

    /// The asynchronous build finished, for the `build` verb (and for `run`,
    /// which ends on a refused or failed build).
    virtual QMetaObject::Connection onBuildCompleted(QObject *context,
                                                     std::function<void(bool)> handler) = 0;
    /// The emulator session started or stopped, so `run` can answer when the
    /// session is actually up rather than merely requested.
    virtual QMetaObject::Connection onSessionRunningChanged(QObject *context,
                                                            std::function<void(bool)> handler) = 0;
    /// A profile save was parsed (true) or could not be (false), so a
    /// `profile results` right after `profile stop` cannot see the previous
    /// run's data.
    virtual QMetaObject::Connection onProfileResultsReady(QObject *context,
                                                          std::function<void(bool)> handler) = 0;
    /// A response to an arbitrary debugger command (`cmd`).
    virtual QMetaObject::Connection onDebugCommandFinished(
        QObject *context, std::function<void(const QString &, const QString &)> handler) = 0;
    /// The answer to a typed read, with the address it was asked about, so a
    /// late answer cannot satisfy the next request.
    virtual QMetaObject::Connection onDebugReadFinished(
        QObject *context, std::function<void(quint32, const QString &)> handler) = 0;
    /// The rows a typed memory read asked for (MAJ-45: the one parse of the
    /// debugger's dump that the pane and the verb share).
    virtual QMetaObject::Connection onDebugReadMemoryFinished(
        QObject *context, std::function<void(quint32, const QList<MemoryRow> &)> handler) = 0;
};

} // namespace control
} // namespace pist
