// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/BreakpointWatchpointModel.h"

// For MainWindow::tr alone: the model's messages are written in the window's
// translation context. The include grants no access to the class — the model
// reaches the window only through its Host (MIN-89).
#include "ui/MainWindow.h"

#include "ui/DebugSessionController.h"
#include "ui/BreakpointPanel.h"
#include "editor/CodeEditor.h"
#include "emu/DebugBackend.h"

#include <algorithm>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QWidget>

namespace pist {

namespace {

/// The widget a modal is parented to: the seam's own QObject parent, which is
/// the window it was built for. Not a Host entry — parenting a dialog to the
/// window you serve is a fact about Qt, not an operation on the window.
QWidget *dialogParent(const QObject *seam)
{
    return qobject_cast<QWidget *>(seam->parent());
}

} // namespace

BreakpointWatchpointModel::BreakpointWatchpointModel(Host host, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
{
}

bool BreakpointWatchpointModel::toggle(const QString &file, int line)
{
    if (file.isEmpty() || line <= 0)
        return false;

    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [&](const Breakpoint &bp) {
                               return bp.line == line && bp.file == file;
                           });
    if (it != m_breakpoints.end())
        m_breakpoints.erase(it);
    else
        m_breakpoints.append(Breakpoint{file, line, QString(), true, 0, false});

    refreshMarkers();

    // Re-arm immediately with a live session, so the new breakpoint takes
    // effect without restarting the program: at once when stopped, and at the
    // next stop when the machine is running (see canReArmBreakpoints).
    if (m_host.session()->canReArmBreakpoints())
        m_host.session()->armBreakpoints();
    return true;
}

bool BreakpointWatchpointModel::toggleAtLabel(const QString &name, QString *detail)
{
    for (const SymbolEntry &sym : m_host.symbols()) {
        if (sym.name != name)
            continue;
        if (sym.file.isEmpty()) {
            // A command-line define or macro-generated name has no source
            // position to break at.
            *detail = MainWindow::tr("error symbol '%1' has no source position").arg(name);
            return false;
        }
        // Breakpoints key by base name everywhere (gutter, arming, the
        // panel); the symbol table carries full paths.
        const QString base = QFileInfo(sym.file).fileName();
        // A label written on its own line has no code there — `count:`
        // followed by the instruction on the next line is the norm — and a
        // breakpoint on that line can never fire. Resolve to the first code
        // line at or after the definition; base-independent, so this holds
        // before a session supplies live bases too. An `equ` has no code of
        // its own and finds nothing, rather than resolving to whatever
        // unrelated instruction follows it.
        const int line = m_host.programMap().nextCodeLine(sym.file, sym.line);
        if (!line) {
            *detail = MainWindow::tr("error '%1' has no code at or after its definition "
                                     "(an equate cannot be broken on)").arg(name);
            return false;
        }
        toggle(base, line);
        *detail = QStringLiteral("ok %1:%2").arg(base).arg(line);
        // Tell the agent the address too, when the map can — it confirms the
        // label resolved to the instruction they meant.
        quint32 address = 0;
        if (m_host.programMap().isResolved()
            && m_host.programMap().codeAddressFor(sym.file, line, &address))
            *detail += QStringLiteral(" = 0x%1").arg(address, 8, 16, QLatin1Char('0'));
        return true;
    }
    *detail = MainWindow::tr("error no symbol named '%1' (has the project been built?)").arg(name);
    return false;
}

void BreakpointWatchpointModel::remove(const QString &file, int line)
{
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [&](const Breakpoint &bp) {
                               return bp.line == line && bp.file == file;
                           });
    if (it == m_breakpoints.end())
        return;

    m_breakpoints.erase(it);
    refreshMarkers();

    // Same gate as toggleBreakpoint: a live session re-arms from the models
    // (dropping one has to reach the emulator too, or the removed breakpoint
    // keeps firing).
    if (m_host.session()->canReArmBreakpoints())
        m_host.session()->armBreakpoints();
}

void BreakpointWatchpointModel::clear()
{
    m_breakpoints.clear();
    refreshMarkers();
    if (m_host.session()->canReArmBreakpoints())
        // Re-derive the host from the models rather than a bare `b all`: watchpoints
        // are armed as `b` conditions (Hatari has no data watchpoints), so `b all`
        // would disarm them too, leaving the panel listing watchpoints that are dead
        // on the emulator until the next arm. armBreakpoints() clears then re-arms —
        // the cleared breakpoints go, the retained watchpoints come back. With the
        // machine mid-run the same re-arm is what removes the cleared breakpoints
        // at the next stop.
        m_host.session()->armBreakpoints();
    m_host.log(MainWindow::tr("[breakpoints] all cleared"));
}

void BreakpointWatchpointModel::clearAllTargets()
{
    // The dock's "Clear all" lists watchpoints too (its button enables when only
    // watchpoints are present), so it clears both models — unlike the
    // breakpoint-only Run-menu action. Watchpoints are armed as `b` conditions
    // (Hatari has no data watchpoints), so the host's `b all` removes them
    // alongside breakpoints; refreshMarkers() updates the panel's
    // breakpoint rows but not its watchpoint rows, so those are set explicitly.
    m_breakpoints.clear();
    m_watchpoints.clear();
    refreshMarkers();
    m_host.showWatchpoints(m_watchpoints);
    if (m_host.backend()->isRunning())
        m_host.backend()->clearBreakpoints();
    m_host.log(MainWindow::tr("[breakpoints] all breakpoints and watchpoints cleared"));
}

void BreakpointWatchpointModel::editCondition(int line)
{
    CodeEditor *const editor = m_host.editor();
    if (!editor || editor->filePath().isEmpty() || line <= 0)
        return;

    const QString file = QFileInfo(editor->filePath()).fileName();
    auto findBreakpoint = [&] {
        return std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                            [&](const Breakpoint &bp) {
                                return bp.line == line && bp.file == file;
                            });
    };

    auto it = findBreakpoint();
    const bool existed = it != m_breakpoints.end();

    bool accepted = false;
    const QString condition = QInputDialog::getText(
        dialogParent(this), MainWindow::tr("Breakpoint condition"),
        MainWindow::tr("Extra condition for %1:%2 (ANDed with the program counter).\n\n"
           "Hatari has no memory watchpoints, so this is how you watch a value,\n"
           "for example:  d0 = $1234   or   (buf) = $ff")
            .arg(file)
            .arg(line),
        QLineEdit::Normal, existed ? it->condition : QString(), &accepted);
    if (!accepted)
        return;

    if (!existed) {
        // Nothing to edit on an empty line: create the breakpoint only now the
        // dialog was accepted, so cancelling does not leave a condition-less one
        // behind.
        m_breakpoints.append(Breakpoint{file, line, QString(), true, 0, false});
        it = findBreakpoint();
        refreshMarkers();
    }

    it->condition = condition.trimmed();
    // The condition is part of the armed command, so the set is rebuilt through
    // the same gate as every other edit (see canReArmBreakpoints).
    if (m_host.session()->canReArmBreakpoints())
        m_host.session()->armBreakpoints();
}

void BreakpointWatchpointModel::addWatchpoint()
{
    bool accepted = false;
    const QString text = QInputDialog::getText(
        dialogParent(this), MainWindow::tr("Add watchpoint"),
        MainWindow::tr("Address to watch (hex), with optional .b/.w/.l width:"),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;

    QString error;
    if (!addWatchpointAddress(text, &error))
        QMessageBox::warning(dialogParent(this), MainWindow::tr("Add watchpoint"), error);
}

bool BreakpointWatchpointModel::addWatchpointAddress(const QString &text, QString *error)
{
    // A watchpoint breaks when the value at an address changes. Hatari has no
    // data watchpoints, so it is armed as a self-inequality breakpoint, which is
    // the debugger's change-tracking (see debug/Watchpoint.h). Width defaults to
    // word; a trailing b/w/l overrides it.
    static const QRegularExpression re(
        QStringLiteral("^\\s*(?:\\$|0x)?([0-9a-fA-F]+)(?:\\.(b|w|l))?\\s*$"));
    const auto match = re.match(text);
    if (!match.hasMatch()) {
        if (error)
            *error = MainWindow::tr("Could not read an address from '%1'.").arg(text);
        return false;
    }

    Watchpoint wp;
    wp.address = match.captured(1).toUInt(nullptr, 16);
    if (!match.captured(2).isEmpty())
        wp.width = match.captured(2).at(0).toLatin1();

    if (wp.address == 0) {
        if (error)
            *error = MainWindow::tr("Address 0 is not a useful thing to watch.");
        return false;
    }

    m_watchpoints.append(wp);
    m_host.log(MainWindow::tr("[watchpoint] %1").arg(wp.label()));
    // Re-arm so it takes effect now if a session is running or stopped, or at
    // the next stop if the machine is mid-run — the same gate every other
    // breakpoint edit uses (see canReArmBreakpoints). Without a session there
    // is nothing to arm and `EmulatorHost::command` would log "No emulator
    // session is running" once per command, so just show it and let the next
    // Run arm it.
    if (m_host.session()->canReArmBreakpoints())
        m_host.session()->armBreakpoints();
    else
        m_host.showWatchpoints(m_watchpoints);
    return true;
}

void BreakpointWatchpointModel::removeWatchpoint(int index)
{
    if (index < 0 || index >= m_watchpoints.size())
        return;
    m_watchpoints.removeAt(index);
    // The same gate as every other breakpoint edit: with a live session the set
    // is rebuilt (whether the machine is stopped or running), and with no
    // session the panel is refreshed — the next Run arms from the models.
    if (m_host.session()->canReArmBreakpoints())
        m_host.session()->armBreakpoints();
    else
        m_host.showWatchpoints(m_watchpoints);
}

QList<Breakpoint> BreakpointWatchpointModel::mergeResolved(const ArmPlan &plan) const
{
    QList<Breakpoint> merged = m_breakpoints;

    // Reset first: a breakpoint that was resolved on a previous run must not keep
    // showing a stale address from before the program was relocated.
    for (Breakpoint &bp : merged) {
        bp.resolved = false;
        bp.address = 0;
    }

    for (const Breakpoint &armed : plan.armed) {
        for (Breakpoint &bp : merged) {
            if (bp.line == armed.line && bp.file == armed.file) {
                bp.address = armed.address;
                bp.resolved = true;
                break;
            }
        }
    }

    return merged;
}

void BreakpointWatchpointModel::refreshMarkers()
{
    // Every open editor gets the markers for its own file.
    for (CodeEditor *editor : m_host.editors()) {
        const QString file = QFileInfo(editor->filePath()).fileName();
        QList<int> lines;
        for (const Breakpoint &bp : m_breakpoints)
            if (bp.file == file)
                lines.append(bp.line);
        editor->setBreakpointLines(lines);
    }

    m_host.showBreakpoints(m_breakpoints);
}

} // namespace pist
