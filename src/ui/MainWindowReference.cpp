// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
#include "ui/MainWindowPrivate.h"
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

QString MainWindow::referenceCardAt(CodeEditor *editor, const QPoint &viewportPos) const
{
    if (!editor)
        return {};

    // The *pointer*, not the caret: the dock follows the caret, and hovering
    // something else should describe that thing.
    const QTextCursor under = editor->cursorForPosition(viewportPos);
    const CursorReference reference =
        referenceAroundCursor(editor, under.blockNumber(), under.positionInBlock());

    if (reference.call) {
        QStringList lines;
        lines << QStringLiteral("%1 — %2").arg(reference.call->name, reference.call->summary);
        lines << reference.call->prototype;
        if (!reference.args.isEmpty())
            lines << tr("Calling with: %1.").arg(reference.args.join(QStringLiteral(", ")));
        return lines.join(QLatin1Char('\n'));
    }
    if (reference.trapContext) {
        if (const InstructionInfo *info = instructionRef(QStringLiteral("trap")))
            return QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary);
        return {};
    }
    if (reference.word.isEmpty())
        return {};

    // What the word *is* comes from the same candidate list completion offers: a
    // mnemonic gets the reference's summary and flags, a symbol where it is
    // defined, a directive nothing (there is no honest summary for one yet) and a
    // word nobody knows nothing at all.
    const CompletionItem *known = nullptr;
    for (const CompletionItem &candidate : m_completionItems) {
        if (candidate.text.compare(reference.word, Qt::CaseInsensitive) == 0) {
            known = &candidate;
            break;
        }
    }
    if (!known)
        return {};

    switch (known->kind) {
    case CompletionKind::Mnemonic: {
        const InstructionInfo *info = instructionRef(known->text);
        if (!info)
            return {}; // an extra (bkpt, an FPU name): the reference has no entry
        QString text = QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary);
        if (!info->flags.isEmpty())
            text += QLatin1Char('\n') + tr("Flags: %1.").arg(info->flags);
        return text;
    }
    case CompletionKind::Label:
    case CompletionKind::Equate:
        if (known->file.isEmpty() || known->line <= 0)
            return tr("%1 — the build gives it no source position.").arg(known->text);
        return tr("%1 — defined at %2:%3").arg(known->text, known->file).arg(known->line);
    case CompletionKind::Directive:
        return {};
    }
    return {};
}


CursorReference MainWindow::referenceAroundCursor(CodeEditor *editor, int blockNumber,
                                                  int column) const
{
    if (!editor || !editor->document())
        return {};
    const QTextDocument *doc = editor->document();
    if (blockNumber < 0 || blockNumber >= doc->blockCount())
        return {};

    // The scanner only needs a small window around the position: OS calls are a
    // local idiom (OsCallScan bounds its scans to a few lines).
    const int base = qMax(0, blockNumber - 10);
    const int last = qMin(doc->blockCount() - 1, blockNumber + 10);
    QStringList window;
    window.reserve(last - base + 1);
    for (int i = base; i <= last; ++i)
        window.append(doc->findBlockByNumber(i).text());
    return referenceAt(window, blockNumber - base, column);
}


void MainWindow::followCursorReference(CodeEditor *editor)
{
    if (!editor || editor != m_editor)
        return;

    const QTextCursor cursor = editor->textCursor();
    const CursorReference reference =
        referenceAroundCursor(editor, cursor.blockNumber(), cursor.positionInBlock());

    auto showStrip = [this](const QString &text) {
        if (!m_instrStrip)
            return;
        m_instrStrip->setText(text);
        m_instrStrip->setToolTip(text.isEmpty() ? tr("Open the instruction reference") : text);
    };

    if (reference.call) {
        if (m_instrRef)
            m_instrRef->showOsCall(reference.call->trap, reference.call->opcode, reference.args);
        QString stack;
        if (!reference.args.isEmpty())
            stack = reference.args.join(QStringLiteral(", "));
        if (reference.call->stackBytes > 0) {
            if (!stack.isEmpty())
                stack += QStringLiteral(" · ");
            stack += tr("%1 bytes").arg(reference.call->stackBytes);
        }
        QString line = QStringLiteral("%1 — %2").arg(reference.call->name, reference.call->summary);
        if (!stack.isEmpty())
            line += tr("  Stack: %1").arg(stack);
        showStrip(line);
        return;
    }
    if (reference.trapContext) {
        // A trap whose function number is not statically known: the generic TRAP
        // entry still says what the instruction does.
        if (m_instrRef)
            m_instrRef->showInstruction(QStringLiteral("trap"));
        if (const InstructionInfo *info = instructionRef(QStringLiteral("trap")))
            showStrip(QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary));
        else
            showStrip(QString());
        return;
    }

    if (m_instrRef)
        m_instrRef->showInstruction(reference.word);
    if (const InstructionInfo *info = instructionRef(reference.word))
        showStrip(QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary));
    else
        showStrip(QString());
}


void MainWindow::raiseInstructionRef()
{
    auto *dock = findChild<QDockWidget *>(QStringLiteral("instructionRefDock"));
    if (!dock)
        return;
    dock->show();
    dock->raise();
}


} // namespace pist
