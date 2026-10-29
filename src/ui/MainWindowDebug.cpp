// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
#include "ui/MainWindowPrivate.h"

#include <QFileInfo>
#include "emu/HexFormat.h"
#include "support/CrashReport.h"
#include "support/DocumentWatcher.h"
#include "support/FileWrite.h"

#include "ui/BreakpointWatchpointModel.h"
#include "ui/DebugSessionController.h"
#include "ui/ProfilerController.h"
#include "ui/RemoteStateAdapter.h"
#include "ui/SessionLauncher.h"
#include "hooks/StopHookRunner.h"
#include "ui/StopHooksDialog.h"
#include "ui/SymbolPaletteDialog.h"
#include "ui/UiText.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/DebugBackend.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"
#include "ui/InstructionRefView.h"
#include "build/FloppyImage.h"
#include "editor/CursorReference.h"
#include "editor/IncludeNav.h"
#include "editor/InstrRef.h"
#include "editor/OsCallBinding.h"
#include "editor/OsCallRef.h"
#include "editor/OsCallScan.h"
#include "ui/ConsoleInput.h"
#include "ui/SymbolsView.h"
#include "control/RemoteControl.h"
#include "ui/ProfilerView.h"
#include "emu/ProfileData.h"
#include "build/SymbolTable.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "image/ImageDocument.h"
#include "image/StFormats.h"
#include "toolchain/Toolchain.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "ui/BreakpointPanel.h"
#include "ui/BitplaneExportDialog.h"
#include "ui/DisassemblyView.h"
#include "ui/EmulatorDisplayWidget.h"
#include "ui/EmuAudio.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include "ui/FileBrowser.h"
#include "ui/FindInFilesView.h"
#include "ui/GitPanel.h"
#include "ui/ImageEditor.h"
#include "ui/NewImageDialog.h"
#include "ui/NewFileDialog.h"
#include "ui/WelcomeView.h"
#include "ui/AboutDialog.h"
#include "ui/MemoryView.h"
#include "ui/PcHistoryView.h"
#include "ui/HardwareView.h"
#include "ui/SettingsDialog.h"
#include "ui/SetupDialog.h"
#include "ui/StackView.h"
#include "ui/RegistersView.h"

#include <algorithm>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QTextBlock>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QSize>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolTip>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtEndian>
#include "ui/MainWindowPrivate.h"

namespace pist {

namespace {

/// One media-channel frame (docs/PLAN.md §12) as pixels to paint.
///
/// The fork's normal stream is 32 bpp RGB32 from the dummy driver — masks
/// r=0x00ff0000, g=0x0000ff00, b=0x000000ff — which in little-endian memory is
/// B,G,R,X, exactly QImage::Format_RGB32, so that case is a direct wrap plus
/// the one copy that makes the image own the frame's pixels. Any other mask
/// layout is expanded component by component, because the header carries the
/// masks precisely so a different driver renders instead of producing garbage.
QImage mediaFrameImage(const MediaFrame &frame)
{
    if (frame.w == 0 || frame.h == 0 || frame.bpp != 32)
        return {};
    const qint64 bytes = qint64(frame.pitch) * frame.h;
    if (frame.pitch < frame.w * 4 || frame.pixels.size() < bytes)
        return {};

    if (frame.rmask == 0x00ff0000u && frame.gmask == 0x0000ff00u
        && frame.bmask == 0x000000ffu) {
        // The const-uchar constructor *borrows* its buffer (verified: the
        // image's bits are the caller's pointer), and the frame is gone when
        // the signal handler returns — copy() is what makes the image own its
        // pixels.
        return QImage(reinterpret_cast<const uchar *>(frame.pixels.constData()),
                      int(frame.w), int(frame.h), int(frame.pitch),
                      QImage::Format_RGB32)
            .copy();
    }

    const auto component = [](quint32 pixel, quint32 mask) -> int {
        if (mask == 0)
            return 0;
        int shift = 0;
        while (((mask >> shift) & 1u) == 0)
            ++shift;
        const quint32 max = mask >> shift;
        return int(((pixel & mask) >> shift) * 255u / max);
    };

    QImage image(int(frame.w), int(frame.h), QImage::Format_RGB32);
    for (quint32 y = 0; y < frame.h; ++y) {
        const uchar *row = reinterpret_cast<const uchar *>(frame.pixels.constData())
            + qint64(y) * frame.pitch;
        QRgb *out = reinterpret_cast<QRgb *>(image.scanLine(int(y)));
        for (quint32 x = 0; x < frame.w; ++x) {
            const quint32 pixel = qFromLittleEndian<quint32>(row + qint64(x) * 4);
            out[x] = qRgb(component(pixel, frame.rmask),
                          component(pixel, frame.gmask),
                          component(pixel, frame.bmask));
        }
    }
    return image;
}

const QString &offeredDebuggingKey()
{
    static const QString key = QStringLiteral("layout/offeredDebugging");
    return key;
}

} // namespace

void MainWindow::wireBackend()
{
    connect(m_host, &IDebugBackend::stateUpdated, this, &MainWindow::onStateUpdated);
    connect(m_host, &IDebugBackend::logLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });
    // Emulator errors are reported in the log *and* the status bar. Log-only was
    // the previous behaviour, and the log is a bottom-dock tab that is not always
    // visible, so a failed launch looked like a normal window.
    connect(m_host, &IDebugBackend::errorOccurred, this, [this](const QString &message) {
        m_log->appendPlainText(QStringLiteral("[error] ") + message);
        statusBar()->showMessage(message, 15000);
    });

    // Without this the status bar kept saying "Running" after Hatari had died,
    // because only stoppedChanged was connected. A dead emulator looked live and
    // step/continue stayed enabled.
    connect(m_host, &IDebugBackend::runningChanged, this, [this](bool running) {
        emit sessionRunningChanged(running);
        if (running) {
            // One launched session (docs/untitled-plan.md §6). The counter sits
            // on the transport's own "the machine is up" transition, so a launch
            // that never came up is not counted.
            settings::noteRun();
            if (m_actPause)
                m_actPause->setEnabled(!m_host->isStopped());
            if (m_consoleInput)
                m_consoleInput->setEnabled(true);
        }
        // The panel's media-session state (cursor hiding, the input grab) is
        // simply "media display engaged and a session is up" — false again
        // here on the way down, which also releases any capture (§12.4).
        if (m_display)
            m_display->setMediaSession(m_mediaDisplay && running);
        // The audio buffer belongs to the session too: a dead emulator's
        // last samples must not keep playing into the next one.
        if (m_audio && !running)
            m_audio->reset();
        if (running)
            return;
        if (m_consoleInput)
            m_consoleInput->setEnabled(running);
        updateSessionChip();
        if (m_actPause)
            m_actPause->setEnabled(false);
        if (m_session->sessionArmed()) {
            if (m_actStep) m_actStep->setEnabled(false);
            if (m_actStepOver) m_actStepOver->setEnabled(false);
            if (m_actStepOut) m_actStepOut->setEnabled(false);
            if (m_actRunToCursor) m_actRunToCursor->setEnabled(false);
            if (m_actFollow) m_actFollow->setEnabled(false);
            if (m_actResume) m_actResume->setEnabled(false);
            m_log->appendPlainText(tr("[session] emulator is no longer running"));
        }
        // One owner for everything an ended session leaves behind — the same
        // reset the next launch performs, so stale arming state, bases, the
        // cached machine state and a pending remote command cannot survive
        // into whatever happens next.
        m_session->resetSessionState();
        updateRegisterStrip();
        // The media display belonged to the session that just ended: forget
        // its frame, so the panel paints its empty state rather than a stale
        // last frame that reads as the emulator's current one.
        if (m_display)
            m_display->setFrame(QImage());
        // The last media frame belonged to the session that just ended too;
        // keeping it would make a later screenshot show a stale screen, and the
        // media mode itself ends with the session.
        m_lastMediaFrame = QImage();
        m_mediaDisplay = false;
        // Both transcripts describe the session that just ended, and neither is
        // re-requested without one: left on screen they read as the hardware's
        // current state. The views' own `clear()` documents the no-session state.
        if (m_hardware)
            m_hardware->clear();
        if (m_pcHistory)
            m_pcHistory->clear();
        // A session's profile and its gutter heat belong to that session.
        if (m_profiler)
            m_profiler->clear();
        for (CodeEditor *editor : openEditors())
            editor->setLineHeat({});
    });
    connect(m_host, &IDebugBackend::memoryDumpReady, this,
            [this](quint32 address, const QList<MemoryRow> &rows, int tag) {
                // The remote `readmem` verb's dump carries a tag no pane owns
                // (kRemoteReadTag) and belongs to the waiting verb, not to a
                // view. Otherwise: route the dump to the pane that asked for it.
                // Either way the rows are already parsed (MAJ-45) — the verb
                // renders its JSON from them and no one re-reads the dump text.
                if (tag == kRemoteReadTag) {
                    emit debugReadMemoryFinished(address, rows);
                    return;
                }
                if (auto *view = m_memoryPanes.value(tag, nullptr))
                    view->applyDump(rows);
            });
    connect(m_host, &IDebugBackend::disassemblyReady, this,
            [this](quint32 address, const QString &response) {
                // Only the remote `disasm` verb reads the disassembly at a given
                // address: the snapshot's own read is readDisassembly() at the
                // PC, which reports no text.
                emit debugReadFinished(address, response);
            });
    // The hardware view's `info <subject>` reports, named by the subject that
    // was asked for — the pane is the only consumer, and only for the subject it
    // is showing. The report is parsed in the backend (MAJ-45), so the pane
    // renders a summary rather than a transcript it reads meaning out of.
    // Nothing else internal matches on a command's text.
    connect(m_host, &IDebugBackend::hardwareInfoReady, this,
            [this](const QString &subject, const HardwareSummary &summary) {
                if (m_hardware && subject == m_hardware->subject())
                    m_hardware->setInfo(summary);
            });
    connect(m_host, &IDebugBackend::historyReady, this,
            [this](const QString &response) {
                if (m_pcHistory)
                    m_pcHistory->setHistory(response);
            });
    connect(m_host, &IDebugBackend::profileSaveFinished, this,
            [this](const QString &) {
                // The save's file is what was asked for; showProfileResults
                // parses it and reports a failure in the console itself.
                if (!m_session->consumeProfileSavePending())
                    return;
                showProfileResults();
            });
    connect(m_host, &IDebugBackend::commandFinished, this,
            [this](const QString &command, const QString &response) {
                // Only the remote `cmd` verb's own text is matched here: it is
                // the user's command, echoed back so its response can be
                // delivered to the client waiting on it.
                if (!m_pendingDebugCommand.isEmpty() && command == m_pendingDebugCommand) {
                    m_pendingDebugCommand.clear();
                    emit debugCommandFinished(command, response);
                }
            });

    // The media channel's frames (docs/PLAN.md §12): convert once, keep the
    // latest for the screenshot verb, and hand the panel its pixels. The
    // conversion is here, not in emu/, because pist_emu links no QtGui.
    connect(m_host, &IDebugBackend::mediaFrameReceived, this,
            [this](const pist::MediaFrame &frame) {
                if (!m_display)
                    return;
                const QImage image = mediaFrameImage(frame);
                if (image.isNull())
                    return;
                m_lastMediaFrame = image;
                m_display->setFrame(image);
            });

    // The media channel's audio (phase 3): chunks of s16le stereo from the
    // fork, played through PiST's own sink (EmuAudio owns pacing and the
    // jitter buffer; it is created once, beside the display widget).
    if (m_audio)
        connect(m_host, &IDebugBackend::mediaAudioReceived, this,
                [this](quint32 rate, const QByteArray &samples) {
                    m_audio->writeChunk(rate, samples);
                });

    connect(m_host, &IDebugBackend::stoppedChanged, this, [this](bool stopped) {
        // Watchers (remote-control `watch`, the MCP shim) learn the running
        // edge here; the stopped edge waits for onStateUpdated, which has the
        // PC worth reporting.
        if (m_eventSink) {
            if (stopped) {
                m_session->setStopEventPending(true);
            } else {
                // A stop whose state never became valid (no register batch
                // followed) must not leave a watcher hanging: publish it
                // without the pc detail rather than drop it.
                if (m_session->consumeStopEventPending()) {
                    m_eventSink->publishEvent(QStringLiteral("stopped"));
                }
                m_eventSink->publishEvent(QStringLiteral("running"));
            }
        }
        m_actStep->setEnabled(stopped);
        m_actStepOver->setEnabled(stopped);
        m_actStepOut->setEnabled(stopped);
        m_actRunToCursor->setEnabled(stopped);
        m_actFollow->setEnabled(stopped);
        m_actResume->setEnabled(stopped);
        updateRunContinueShortcut();
        updateSessionChip();
        updateRegisterStrip();
        // The first stop of a session is the entry stop: the program has
        // loaded and the debugger is waiting before its first instruction.
        // That is the one stop a new user reads as a hang, so the panel names
        // it and offers Continue rather than only muting the video.
        const bool entryStop = stopped && !m_session->sessionArmed();
        const QString continueHint =
            m_actResume->shortcut().toString(QKeySequence::NativeText);
        // The embedded panel renders no frames while stopped, so tell it to show
        // its paused hint rather than look frozen.
        if (m_display)
            m_display->setPaused(stopped, entryStop, continueHint);
        // A stop is also when the user wants their keyboard back: a machine
        // halted in its debugger cannot consume input, so anything typed
        // would sit in the media socket until a resume replayed it (seen
        // live in the phase-2 desktop proof). §12.4's stop edge.
        if (stopped && m_display)
            m_display->setInputCaptured(false);
        // Registers are only writable while stopped, through the debugger.
        if (m_actPause)
            m_actPause->setEnabled(!stopped && m_host->isRunning());
        if (m_registers)
            m_registers->setEditingEnabled(stopped);
        if (m_memory)
            m_memory->setEditingEnabled(stopped);
        m_profiling->syncActions();
        if (stopped) {
            m_session->onDebuggerStopped();
            // A status line, not a dialog: a modal here hangs the offscreen
            // stop tests. The key makes the offer once per settings store.
            if (!QSettings().value(offeredDebuggingKey()).toBool()) {
                QSettings().setValue(offeredDebuggingKey(), true);
                statusBar()->showMessage(
                    tr("View → Layout → Debugging arranges the panels around a stop."),
                    8000);
            }
            // The panel carries the same message, but it is hidden when the
            // session uses an emulator without the media channel — and a
            // loaded program that never moves is the report that prompted
            // this. The status bar is the one surface both transports share.
            if (entryStop && m_display && !m_display->isVisible()) {
                statusBar()->showMessage(
                    continueHint.isEmpty()
                        ? tr("Program loaded and paused — choose Continue to run it.")
                        : tr("Program loaded and paused — press %1 to run it.")
                              .arg(continueHint),
                    10000);
            }
        }
    });

    connect(m_host, &IDebugBackend::stackDumpReady, this,
            [this](quint32 sp, const QList<MemoryRow> &rows) {
                // A step-out dump is consumed regardless of the stack view: any
                // stack response is at the same SP with the return address on
                // top. Those rows are the dump, parsed once in the backend
                // (MAJ-45) — the controller reads the return address out of
                // them and the view below renders the same ones, instead of
                // both parsing the transcript.
                if (!m_session->handleStackDumpForStepOut(rows))
                    return;
                if (!m_stack)
                    return;
                // The annotation needs the *text* extent, not the data base.
                // With no data section (or one that does not follow text) the
                // data base gives an empty range and every return address
                // silently goes unmarked, so take the extent from the
                // listings and fall back to the old bound when the map has
                // none (docs/code-review-glm-001.md, P3).
                const quint32 textEnd = m_programMap.textEnd();
                m_stack->setStackDump(sp, rows, m_lastState.textBase,
                                      textEnd ? textEnd : m_lastState.dataBase);
            });
}


void MainWindow::step()
{
    // A plain step is not a running->stopped transition, so stoppedChanged does
    // not fire and onDebuggerStopped is never called — the refresh has to be
    // explicit or nothing updates (registers, disassembly, the editor, and the
    // step-back snapshot all depend on it). The same fact is why the stop hook
    // needs the step flag: the state batch that follows is the only report of
    // the movement (docs/stop-hooks-plan.md §6).
    m_session->setStepEventPending(true);
    m_host->step();
    m_host->refresh();
}


void MainWindow::stepOver()
{
    m_session->setStepEventPending(true);
    m_host->stepOver();
    m_host->refresh();
}


void MainWindow::stepOut()
{
    m_session->stepOut();
}


void MainWindow::runToCursor()
{
    if (!m_host->isStopped() || !m_editor)
        return;
    // "Run to my line" is a caret gesture like any other: it reads honestly as
    // "run until *my* line" only while the caret is the user's, so following
    // ends with it rather than dragging the caret off the line being run to
    // (cursor-plan §4.4).
    setPcFollow(false);
    const int line = m_editor->textCursor().blockNumber() + 1;
    quint32 address = 0;
    if (!m_programMap.codeAddressFor(m_editor->filePath(), line, &address)) {
        m_log->appendPlainText(tr("[run to cursor] no code address for %1:%2")
                                   .arg(m_editor->filePath())
                                   .arg(line));
        return;
    }
    m_log->appendPlainText(tr("[run to cursor] %1:%2 -> 0x%3")
                               .arg(m_editor->filePath())
                               .arg(line)
                               .arg(address, 0, 16));
    m_host->breakAtAddressOnce(address);
    m_host->resume();
}


void MainWindow::pauseSession()
{
    m_host->pause();
}


void MainWindow::resume()
{
    m_host->resume();
}


void MainWindow::rebuildProgramMap()
{
    m_programMap.clear();

    const QStringList listings = m_build->listingFiles();
    const QStringList objects = m_build->objectFiles();
    QFileInfo sourceInfo(buildSourcePath());

    QStringList sources{sourceInfo.absoluteFilePath()};
    sources += m_settings.additionalSources;

    QString error;
    for (int i = 0; i < listings.size() && i < sources.size(); ++i) {
        // A module whose listing cannot be read is reported and skipped rather
        // than abandoning the whole map: the program still built and runs, only
        // that module's source lines are unavailable.
        if (!m_programMap.addModule(sources.at(i), objects.value(i), listings.at(i), &error)) {
            m_log->appendPlainText(QStringLiteral("[line map] ") + error);
        }
    }

    if (m_build->usesLinker()) {
        LinkMap linkMap;
        if (linkMap.parse(m_build->linkMapFile(), &error)) {
            m_programMap.setLinkMap(linkMap);
        } else {
            // Without the map, modules past the first cannot be located, so
            // breakpoints in them would be armed at the wrong address. Say so
            // instead of mapping them wrongly.
            m_log->appendPlainText(QStringLiteral("[link map] ") + error);
            addProblemRow({QString(), 0,
                           tr("Could not read the link map, so only the first module "
                              "can be mapped to source: %1").arg(error),
                           true});
        }
    }

    // The symbol browser reads the same listings. Addresses stay blank until
    // a session resolves the live bases (the view is re-fed in onStateUpdated);
    // the names feed the console's completion immediately.
    if (m_symbolsView) {
        m_symbols.clear();
        for (int i = 0; i < listings.size() && i < sources.size(); ++i)
            m_symbols += symbolsFromListing(listings.at(i), sources.at(i));
        m_symbolsView->setSymbols(m_symbols, &m_programMap);
        if (m_consoleInput) {
            QStringList all = m_consoleVerbs;
            for (const SymbolEntry &s : m_symbols)
                all << s.name;
            m_consoleInput->setCompletions(all);
        }

        // One candidate list, three surfaces (docs/glm-features.md §4): the
        // editor's completion popup filters this, the palette lists its symbols,
        // and the hover uses its kinds to decide what a word means. A symbol with
        // no source position (a command-line define) is still offered by name; its
        // file and line stay empty, so nothing claims a location it does not have.
        m_completionItems = CompletionModel::vocabulary();
        for (const SymbolEntry &symbol : m_symbols) {
            CompletionItem item;
            item.text = symbol.name;
            item.kind = symbol.equate ? CompletionKind::Equate : CompletionKind::Label;
            item.file = symbol.file;
            item.line = symbol.line;
            m_completionItems.append(item);
        }
        for (CodeEditor *editor : openEditors())
            editor->setCompletions(m_completionItems);
    }
    // Only a linked build can lose a module: a single-module `-Ftos` build has no
    // linker and no map, and its one module owns the whole program, so it is
    // placed by definition and its lines resolve against the live bases
    // directly. The line used to fire for exactly that case — naming the only
    // module of a plain single-file build as unplaced, before any live base
    // existed to place it (MIN-84).
    if (m_build->usesLinker()) {
        const QStringList unplaced = m_programMap.unplacedModules();
        if (!unplaced.isEmpty()) {
            m_log->appendPlainText(
                tr("[link map] no placement for: %1").arg(unplaced.join(QLatin1String(", "))));
        }
    }
}


void MainWindow::addWatchpoint()
{
    m_bpModel->addWatchpoint();
}



void MainWindow::debugCommand(const QString &command)
{
    // Route through the host; the response comes back on debugCommandFinished,
    // which is just the host's commandFinished re-emitted for the socket. The
    // text is the client's own, so it is the one place besides the console
    // where a debugger command is passed through as written.
    m_pendingDebugCommand = command;
    m_host->command(command);
}


QString MainWindow::stoppedSourceLocation() const
{
    if (!m_host->isStopped() || !m_lastState.regs.valid)
        return {};
    if (!m_bases.isValid() || m_programMap.isEmpty())
        return {};
    LineMap::Address loc;
    if (!m_programMap.lineFor(m_lastState.pc, &loc))
        return {};
    return QFileInfo(loc.file).fileName() + QLatin1Char(':') + QString::number(loc.line);
}


void MainWindow::debugReadMemory(quint32 address, int length)
{
    // A tagged dump that no memory pane owns, so it reaches the verb that asked
    // (see kRemoteReadTag) instead of being applied to a view.
    m_host->requestMemoryDump(address, length, kRemoteReadTag);
}


void MainWindow::debugReadDisassembly(quint32 address)
{
    // The read reports its text on disassemblyReady, which this relays as a
    // remote read's answer.
    m_host->readDisassemblyAt(address);
}


void MainWindow::sendConsoleCommand(const QString &command)
{
    if (!m_host->isRunning()) {
        m_log->appendPlainText(tr("[console] no emulator session is running"));
        return;
    }
    m_log->appendPlainText(QStringLiteral("> ") + command);
    // The response streams via logLine from both backends (HRDB emits the ack
    // for console-originated commands; native streams stderr). While the machine
    // is running a debugger command cannot run: the native debugger does not
    // read stdin until it is entered, and HRDB's console handler only runs from
    // its break loop — so both hold it for the next stop, and HRDB says so in
    // the log rather than deferring it in silence.
    m_host->consoleCommand(command);
}


bool MainWindow::setMemoryByte(quint32 address, quint32 value, MemoryView *pane)
{
    if (!m_host->isStopped()) {
        m_log->appendPlainText(tr("[edit] memory can only be changed while stopped"));
        return false;
    }
    // One byte is written; the debugger is silent on success, so refresh the
    // pane that asked. Without a pane (the remote-control setmem path) the
    // first pane is the sensible default.
    m_host->writeMemoryByte(address, quint8(value));
    if (pane)
        pane->refresh();
    else if (m_memory)
        m_memory->refresh();
    return true;
}


bool MainWindow::setRegister(const QString &regName, quint32 value)
{
    if (!m_host->isStopped()) {
        m_log->appendPlainText(tr("[edit] registers can only be changed while stopped"));
        return false;
    }
    // A successful register write prints nothing — so refresh to show the new
    // value.
    m_host->writeRegister(regName, value);
    m_host->refresh();
    return true;
}


bool MainWindow::addWatchpointAddress(const QString &text, Watchpoint *added, QString *error)
{
    return m_bpModel->addWatchpointAddress(text, added, error);
}


bool MainWindow::clearWatchpoints(const QString &text, int *removed, QString *error)
{
    return m_bpModel->clearWatchpoints(text, removed, error);
}


void MainWindow::removeWatchpoint(int index)
{
    m_bpModel->removeWatchpoint(index);
}


bool MainWindow::toggleBreakpointAtLine(int line)
{
    if (!m_editor || line <= 0)
        return false;

    // Breakpoints are keyed by base name, so two modules linked from different
    // directories under the same file name (`util.s`) collide here and in the
    // gutter. Re-keying on the full path is a larger change (the panel, the
    // listings and the linker map all identify modules the same way); until then
    // this is the documented limitation.
    return m_bpModel->toggle(QFileInfo(m_editor->filePath()).fileName(), line);
}


bool MainWindow::toggleBreakpoint(const QString &file, int line)
{
    return m_bpModel->toggle(file, line);
}


bool MainWindow::toggleBreakpointAtLabel(const QString &name, QString *detail)
{
    return m_bpModel->toggleAtLabel(name, detail);
}


bool MainWindow::lineHasCode(int line) const
{
    if (!m_editor || line <= 0)
        return false;
    if (!m_programMap.isResolved())
        return true; // no build map to judge by; arming will decide
    quint32 address = 0;
    return m_programMap.codeAddressFor(QFileInfo(m_editor->filePath()).fileName(),
                                       line, &address);
}


void MainWindow::removeBreakpoint(const QString &file, int line)
{
    m_bpModel->remove(file, line);
}


QString MainWindow::resolveNavigablePath(const QString &file) const
{
    if (file.isEmpty())
        return {};
    if (QFileInfo(file).isAbsolute() && QFileInfo::exists(file))
        return QFileInfo(file).absoluteFilePath();
    for (const QString &source : m_programMap.sourceFiles()) {
        if (LineMap::sameSource(file, source) && QFileInfo::exists(source))
            return QFileInfo(source).absoluteFilePath();
    }
    const QString anchor = buildSourcePath();
    if (!anchor.isEmpty()) {
        const QString beside = QFileInfo(anchor).absolutePath() + QLatin1Char('/')
                             + QFileInfo(file).fileName();
        if (QFileInfo::exists(beside))
            return beside;
    }
    if (QFileInfo::exists(file))
        return QFileInfo(file).absoluteFilePath();
    return {};
}


bool MainWindow::navigateToSourceLine(const QString &file, int line)
{
    if (line <= 0)
        return false;

    // A build-level note has no source file. Stay in the editor that is open.
    if (file.isEmpty()) {
        if (!m_editor)
            return false;
        m_tabs->setCurrentWidget(m_editor);
        m_editor->gotoLine(line);
        return true;
    }

    CodeEditor *target = nullptr;
    for (CodeEditor *editor : openEditors()) {
        if (LineMap::sameSource(file, editor->filePath())) {
            target = editor;
            break;
        }
    }
    if (!target) {
        const QString path = resolveNavigablePath(file);
        if (path.isEmpty() || !openPath(path)) {
            statusBar()->showMessage(tr("Could not open %1").arg(file), 5000);
            return false;
        }
        for (CodeEditor *editor : openEditors()) {
            if (LineMap::sameSource(path, editor->filePath())) {
                target = editor;
                break;
            }
        }
        if (!target) {
            statusBar()->showMessage(tr("Could not open %1").arg(file), 5000);
            return false;
        }
    }
    m_tabs->setCurrentWidget(target);
    target->gotoLine(line);
    target->setFocus();
    return true;
}


void MainWindow::goToBreakpoint(const QString &file, int line)
{
    navigateToSourceLine(file, line);
}


void MainWindow::showSymbolPalette()
{
    if (!m_symbolPalette) {
        m_symbolPalette = new SymbolPaletteDialog(this);
        connect(m_symbolPalette, &SymbolPaletteDialog::symbolActivated, this,
                [this](const QString &file, int line) { navigateToSourceLine(file, line); });
    }
    // The symbols belong to the build, and a build may have run since the palette
    // was last shown: hand it the current candidates every time.
    m_symbolPalette->setSymbols(m_completionItems);
    m_symbolPalette->show();
    m_symbolPalette->raise();
    m_symbolPalette->activateWindow();
    m_symbolPalette->focusFilter();
}


void MainWindow::stepDiagnostic(int direction)
{
    const int count = m_problems ? m_problems->topLevelItemCount() : 0;
    if (count == 0)
        return;

    // From the current row, wrapping; an item without a line (build-level
    // messages) cannot be navigated to, so it is skipped.
    int row = m_problems->indexOfTopLevelItem(m_problems->currentItem());
    for (int i = 0; i < count; ++i) {
        row = ((row + direction) % count + count) % count;
        QTreeWidgetItem *item = m_problems->topLevelItem(row);
        const RemoteStateAdapter::ProblemRow *entry = problemEntryFor(item);
        if (!entry || entry->line <= 0)
            continue;
        // Same contract as double-clicking the item: navigate within the open
        // document only (goToBreakpoint's rule), never switch files blindly.
        m_problems->setCurrentItem(item);
        navigateToSourceLine(entry->file, entry->line);
        return;
    }
}


void MainWindow::nextDiagnostic()
{
    stepDiagnostic(+1);
}


void MainWindow::previousDiagnostic()
{
    stepDiagnostic(-1);
}


void MainWindow::editBreakpointCondition(int line)
{
    m_bpModel->editCondition(line);
}


void MainWindow::editBreakpointScript(int line)
{
    m_bpModel->editScript(line);
}


void MainWindow::clearAllBreakpoints()
{
    m_bpModel->clear();
}


void MainWindow::clearAllDebugTargets()
{
    m_bpModel->clearAllTargets();
}


void MainWindow::onStateUpdated(const MachineState &state)
{
    m_lastState = state;
    m_registers->setState(state);
    // Publish the stop only once a register batch has landed: the entry
    // attach's standalone `info basepage` emits a state update with pc still
    // 0, and publishing that (observed as "event stopped pc=0x00000000")
    // tells a watcher the machine stopped at address 0. The refresh that
    // follows every stop carries real registers, so the event keeps its pc.
    if (m_session->stopEventPending() && state.regs.valid) {
        m_session->setStopEventPending(false);
        if (m_eventSink)
            m_eventSink->publishEvent(QStringLiteral("stopped"),
                                      QStringLiteral("pc=0x%1").arg(hex::hex32(state.pc, hex::Case::Lower)));
        // The stop hook runs from here, and nowhere earlier: this is the moment
        // the stop is real enough to report — the register batch has landed, so
        // readreg() has the machine's actual truth, and the attach's own
        // commands have completed (docs/stop-hooks-plan.md D1).
        if (m_hooks) {
            // The cause, as far as the transport can tell us: a breakpoint whose
            // armed address is this PC. A pause, an exception and a watchpoint
            // are indistinguishable from here, so they stay "unknown" rather
            // than being guessed at.
            StopTrigger how;
            if (const Breakpoint *hit = breakpointAt(m_bpModel->breakpoints(), state.pc)) {
                how.reason = StopReason::Breakpoint;
                how.at = hit->label();
                how.script = hit->scriptPath;
            }
            m_hooks->onStopPublished(state, how);
        }
    } else if (m_session->stepEventPending() && state.regs.valid) {
        // A step never reaches the branch above: `s`/`n` are not running→stopped
        // transitions, so stoppedChanged does not fire for them (see step()).
        // The flag makes "every stop" include them.
        m_session->setStepEventPending(false);
        if (m_hooks) {
            StopTrigger how;
            how.reason = StopReason::Step;
            m_hooks->onStopPublished(state, how);
        }
    }
    m_disassembly->setState(state);

    bool navigated = false;
    if (state.hasBases()) {
        const bool firstBases = !m_bases.isValid();
        m_bases.text = state.textBase;
        m_bases.data = state.dataBase;
        m_bases.bss = state.bssBase;

        // Hand the live bases to the program map. Without this, lineFor and
        // addressFor return false forever: no breakpoint ever resolves, and no
        // stop ever finds a source line to place the execution marker on — the
        // chips read "Stopped" and nothing more (docs/code-review-glm-001.md
        // P1 root cause B).
        m_programMap.setLiveBases(m_bases);

        // The symbol browser resolves addresses only now that the bases exist.
        if (m_symbolsView && !m_symbols.isEmpty())
            m_symbolsView->setSymbols(m_symbols, &m_programMap);

        // Arm breakpoints when the bases they resolve against actually arrive —
        // here, where they are set — not on a zero-delay timer that always fires
        // before the basepage response does (P1 root cause A).
        if (m_session->sessionArmed() && !m_session->breakpointsArmedThisSession())
            m_session->armBreakpoints();

        // Point the memory pane at the program's own data on the first stop.
        // Its previous address is meaningless across sessions.
        if (firstBases && m_memory) {
            m_memory->goToAddress(state.dataBase ? state.dataBase : state.textBase);
            navigated = true;
        }
    }

    // The mark's "at file.s:12" and the text/data/bss tint both come from this
    // stop. Every pane gets them; only the first pane is refreshed below,
    // because the others are watching an address the user chose.
    if (state.regs.valid) {
        QString where;
        LineMap::Address loc;
        if (m_bases.isValid() && !m_programMap.isEmpty() && m_programMap.lineFor(state.pc, &loc))
            where = QFileInfo(loc.file).fileName() + QLatin1Char(':') + QString::number(loc.line);
        const quint32 textEnd = m_programMap.textEnd();
        const quint32 dataEnd = m_programMap.sectionEnd(QStringLiteral("data"));
        const quint32 bssEnd = m_programMap.sectionEnd(QStringLiteral("bss"));
        for (MemoryView *view : m_memoryPanes) {
            view->setStopContext(state.pc, where);
            view->setProgramRegions(state.textBase, textEnd, state.dataBase, dataEnd,
                                    state.bssBase, bssEnd);
        }
    }

    // Re-read the memory pane whenever the machine state changes, not only when
    // the user navigates: a memory view that goes stale the moment you step is
    // broken. The first stop is skipped because it just navigated, and that
    // already requested the dump it needs.
    if (m_memory && !navigated)
        m_memory->refresh();

    // Re-dump the stack too: it changes on every call, return, push and pop, so
    // like the memory view it is only meaningful if it tracks the machine.
    if (m_stack && state.regs.valid)
        m_host->requestStackDump(state.regs.a[7], 96);

    // Refresh the hardware registers as well.
    if (m_hardware)
        m_host->infoSubject(m_hardware->subject());

    // And the execution path that led here.
    if (m_pcHistory)
        m_host->readHistory(16);

    locationFromPc(state.pc);
    updateRegisterStrip();
}



void MainWindow::locationFromPc(quint32 pc)
{
    LineMap::Address address;
    const bool mapped = m_bases.isValid() && !m_programMap.isEmpty()
                        && m_programMap.lineFor(pc, &address);
    if (!mapped) {
        // Unmapped: library or ROM code, or a PC of 0 before the bases are
        // live. The marker goes (it is nothing but a lie now), and the caret,
        // the scroll, the focus and the tab layout — the user's, all of them —
        // stay exactly where they were (cursor-plan §3.4).
        setExecutionMarker(nullptr, 0);
        m_stoppedFile.clear();
        m_stoppedLine = 0;
        updateSessionChip();
        updateCaretChip();
        return;
    }

    // The PC's file among the open editors. The comparison must be
    // path-tolerant: the listing records whatever path was passed to vasm
    // (usually absolute), while the editor knows the file it was opened with
    // (usually just a name).
    CodeEditor *target = nullptr;
    for (CodeEditor *editor : openEditors())
        if (LineMap::sameSource(address.file, editor->filePath())) {
            target = editor;
            break;
        }

    m_stoppedFile = QFileInfo(address.file).fileName();
    m_stoppedLine = address.line;
    updateSessionChip();

    if (!m_pcPlaced) {
        // The session's first stop is a *placement*. It arrives at the
        // program's own first line moments after the user pressed Run, which is
        // exactly where a caret-following editor used to drag them (the
        // reported injury, cursor-plan §1). The marker and the chips say where
        // the machine is; nothing else moves, and no file is opened for it.
        m_pcPlaced = true;
        setExecutionMarker(target, address.line);
        updateCaretChip();
        return;
    }

    if (!m_pcFollow) {
        // The caret is the user's. The marker follows the PC; the caret, the
        // scroll, the focus and the tab layout do not, and a file the PC has
        // walked into is not opened on its behalf (cursor-plan §3.2, §4.3).
        setExecutionMarker(target, address.line);
        updateCaretChip();
        return;
    }

    // Following: this movement is the PC's, and the caret comes along.
    // Everything below — opening and raising the tab, and the caret move itself
    // — happens under the guard, so the cursorPositionChanged those produce is
    // not mistaken for the user claiming the caret back (cursor-plan §4.2).
    // Saved and restored rather than just cleared: opening a tab can run a
    // nested event loop (a warning modal for a file that will not load), and a
    // state update delivered in there re-enters this function — the guard the
    // outer call set has to survive that.
    const bool wasFollowingInProgress = m_pcFollowInProgress;
    m_pcFollowInProgress = true;

    // Opening the PC's file is part of following: the caret has to have
    // somewhere to land. It is the one thing the released path above must not
    // do, and the guard's opening paragraph covers it too — a tab created here
    // lands its own cursor in the document.
    if (!target && QFileInfo::exists(address.file))
        target = addEditorTab(address.file);

    if (target) {
        m_tabs->setCurrentWidget(target);
        setExecutionMarker(target, address.line);
        target->followExecutionLine(address.line);
    } else {
        setExecutionMarker(nullptr, 0);
    }

    m_pcFollowInProgress = wasFollowingInProgress;
    updateCaretChip();
}


bool MainWindow::engagePcFollow()
{
    if (!m_host || !m_host->isRunning() || !m_host->isStopped())
        return false;

    // The stop this engagement applies to is a movement of the PC's, not the
    // untouched placement of a fresh session: the user is asking for the caret
    // to come along from here on, whether or not a placement has happened yet
    // (it has not when the first stop's PC did not map).
    m_pcPlaced = true;
    setPcFollow(true);
    return true;
}


void MainWindow::setPcFollow(bool following)
{
    m_pcFollow = following;
    if (!m_actFollow)
        return;
    // Reported, not clicked: letting this through would re-enter the toggled
    // handler that calls the very function this is inside.
    const QSignalBlocker blocker(m_actFollow);
    m_actFollow->setChecked(following);
}


void MainWindow::setExecutionMarker(CodeEditor *target, int line)
{
    for (CodeEditor *editor : openEditors())
        if (editor != target)
            editor->clearCurrentExecutionLine();
    if (target)
        target->setCurrentExecutionLine(line);
}


void MainWindow::followExecution()
{
    if (!engagePcFollow())
        return;
    // Go to where the machine is now: every engage affordance is something the
    // user clicked *in order to* get to the PC, so engaging is also the jump.
    // An unmapped PC — library code, or a session whose first stop has not
    // arrived — has nowhere to go, and locationFromPc says so; following is
    // armed all the same, and the caret moves at the next mapped stop.
    locationFromPc(m_lastState.pc);
}

} // namespace pist
