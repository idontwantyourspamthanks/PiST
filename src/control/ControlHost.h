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

#include "debug/Watchpoint.h"
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
    /// Add a watchpoint from its text form: an address ("$12345", "12345.l") or a
    /// label ("counter.l"), which the arm plan resolves again on every run.
    /// `added` receives what was stored — a label stays a label — with the
    /// resolved address filled in when the symbol table can already place it.
    /// False, with `error` set, on a bad address or a name that cannot be watched.
    virtual bool addWatchpointAddress(const QString &text, Watchpoint *added, QString *error) = 0;
    /// Remove watchpoints: every one when `text` is empty, otherwise those it
    /// names — a label, `label.width`, what the panel shows, or an address.
    /// `removed` reports how many went. False, with `error` set, when a text
    /// matches nothing, so "clear what I named" can tell "none left" from "never
    /// there" (docs/glm-features.md §5).
    virtual bool clearWatchpoints(const QString &text, int *removed, QString *error) = 0;
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
    /// The stop hook as JSON {enabled, path, autoContinue, last} — the project's
    /// script and what its last run did, for the `hook` verb an agent uses to
    /// learn what a hook it never sees ran.
    virtual QJsonObject hookJson() const = 0;

    /// How the program on disk differs from the snapshot taken before the last
    /// build replaced it. `{ok, written, sections?}` or `{ok:false, error}`.
    /// `written` is false: the verb does not change the machine. For `builddiff`.
    virtual QJsonObject buildDiffJson() const = 0;

    /// The project's declared sprite exports, as JSON objects
    /// {source, phase, output, palette, sprite, masked, shifted, shiftedMasked,
    /// preShifts, transparent}. Empty when none are declared. For `assets`.
    virtual QJsonArray assetsJson() const = 0;

    /// Add or replace the export whose `output` `spec` names, and save the
    /// project. False, with `error` set, when there is no source to save beside
    /// or the spec cannot be stored.
    virtual bool declareAsset(const QJsonObject &spec, QString *error) = 0;

    /// Remove the export whose output file name is `output`, and save the
    /// project. False, with `error` set, when that name is not in the list.
    virtual bool removeAsset(const QString &output, QString *error) = 0;

    /// "file.s:line" for the stopped PC when the line map has one, else empty.
    /// A memory mark records it so a later compare can say where the checkpoint
    /// was taken.
    virtual QString stoppedSourceLocation() const = 0;

    /// Read a slice of `path` as low-resolution bitplanes and write a .pim to
    /// `outPath`. `hasPalette` selects whether `paletteOffset` is read (32 STfm
    /// colour words). Synchronous. False, with `*error` set, on a bad range, a
    /// short read, or a failed write. No emulator.
    virtual bool ripImage(const QString &path, quint32 offset, int width, int height,
                          const QString &outPath, bool hasPalette, quint32 paletteOffset,
                          QString *error) = 0;

    /// Read `length` bytes at `offset` in `path` and write an 8-bit mono WAV to
    /// `outPath`. `signedSamples` is the source encoding. Synchronous. False, with
    /// `*error` set, on a bad range or a failed write. No emulator.
    virtual bool ripSample(const QString &path, quint32 offset, int length, int rate,
                           bool signedSamples, const QString &outPath,
                           QString *error) = 0;

    /// Replace `path` with `bytes`. False, with `*error` set, when the write fails.
    /// Empty `bytes` is a failure (a dump that read nothing must not create an empty
    /// file and report success).
    virtual bool writeBinaryFile(const QString &path, const QByteArray &bytes,
                                 QString *error) = 0;

    /// Turn a stopped low-resolution screen dump into a `.pim`. `resolutionByte`
    /// is the single shifter byte from $ff8260. No debugger traffic.
    virtual bool writeCapturedScreen(const QString &outPath, const QByteArray &resolutionByte,
                                    const QByteArray &paletteWords, const QByteArray &planes,
                                    QString *error) = 0;

    /// Turn a stopped STE DMA frame into an unsigned 8-bit mono WAV. No debugger
    /// traffic. Stereo and over-long frames are refused before this is called.
    virtual bool writeCapturedDma(const QString &outPath, int rateHz, const QByteArray &samples,
                                 QString *error) = 0;

    /// Read the stopped machine and write a low-resolution .pim. The caller has
    /// already checked that the machine is stopped. False, with `*error` set.
    virtual bool captureScreenTo(const QString &outPath, QString *error) = 0;

    /// Read the stopped machine's STE DMA frame and write an 8-bit mono WAV.
    /// Stereo and a frame longer than 65536 bytes fail with the same sentences the
    /// verb already returns. The caller has already checked that the machine is stopped.
    virtual bool captureDmaTo(const QString &outPath, QString *error) = 0;

    /// Read an AVR header at `offset` and the samples it names, and write a WAV.
    /// Does not read past the declared length. No emulator.
    virtual bool ripAvr(const QString &path, quint32 offset, const QString &outPath,
                        QString *error) = 0;

    /// AVR, IFF, Ice and Atomik hits in `path`, as `{offset, kind}` objects.
    /// `*error` is set when the file cannot be read or is over 2 MiB. An empty
    /// array with an empty error is a file with no hits.
    virtual QJsonArray scanFileMagic(const QString &path, QString *error) = 0;

    /// The read-only TOS listing for `path`: `summary` plus `rows` of
    /// `{address, label, bytes, text, note}`. Addresses are image-relative
    /// (`0x` and eight hex digits). `*error` is set when the file cannot be
    /// read, is over 2 MiB, or is not a TOS executable. Does not open a tab
    /// and does not need the emulator.
    virtual QJsonObject analyzeProgram(const QString &path, QString *error) = 0;

    /// The same hits inside the files of a FAT12 image. A list failure sets
    /// `*error`. A file that cannot be read is one `{path, error}` object.
    virtual QJsonArray scanDiskMagic(const QString &imagePath, QString *error) = 0;

    /// "ice", "atomik", or empty. A short read is empty, not an error. `*error`
    /// is set when the file cannot be opened. Does not write anything.
    virtual QString identifyPacker(const QString &path, quint32 offset, QString *error) = 0;

    /// SNDH text tags in at most 4096 bytes at `offset`. `found` false is still
    /// a result. `*error` is set when the file cannot be read.
    virtual QJsonObject sndhInfo(const QString &path, quint32 offset, QString *error) = 0;

    /// Capture the IDE's window into `path` (the `screenshot` verb). False
    /// when the capture or the write failed. The capturing itself is the UI's:
    /// it needs the window handle and, under X11, the XGetImage path the
    /// embedder already owns.
    virtual bool saveScreenshot(const QString &path) = 0;

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
