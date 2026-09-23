// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/RemoteStateAdapter.h"

// For MainWindow::tr alone: the adapter's one message is written in the
// window's translation context. The include grants no access to the class — the
// adapter reads the window only through its Host (MIN-89).
#include "ui/MainWindow.h"

#include "ui/ImageEditor.h"
#include "ui/ProfilerController.h"
#include "emu/DebugBackend.h"
#include "emu/HexFormat.h"
#include "editor/CodeEditor.h"

#include <QJsonValue>
#include <QTabWidget>

namespace pist {

RemoteStateAdapter::RemoteStateAdapter(Host host, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
{
}

QJsonObject RemoteStateAdapter::documentJson() const
{
    QJsonObject object;
    const CodeEditor *const editor = m_host.editor();
    if (!editor || editor->filePath().isEmpty())
        return object;
    object.insert(QStringLiteral("path"), editor->filePath());
    object.insert(QStringLiteral("text"), editor->toPlainText());
    return object;
}

QJsonObject RemoteStateAdapter::stateJson() const
{
    QJsonObject object;
    const IDebugBackend *const backend = m_host.backend();
    const bool running = backend && backend->isRunning();
    const bool stopped = backend && backend->isStopped();
    object.insert(QStringLiteral("running"), running);
    object.insert(QStringLiteral("stopped"), stopped);
    if (!m_host.lastState().regs.valid)
        return object;

    const Registers &r = m_host.lastState().regs;
    const auto word = [](quint32 value) {
        return QStringLiteral("0x%1").arg(hex::hex32(value, hex::Case::Lower));
    };
    object.insert(QStringLiteral("pc"), word(m_host.lastState().pc));
    QJsonObject d, a;
    for (int i = 0; i < 8; ++i) {
        d.insert(QStringLiteral("d%1").arg(i), word(r.d[i]));
        a.insert(QStringLiteral("a%1").arg(i), word(r.a[i]));
    }
    object.insert(QStringLiteral("d"), d);
    object.insert(QStringLiteral("a"), a);
    object.insert(QStringLiteral("sr"),
                  QStringLiteral("0x%1").arg(r.sr, 4, 16, QLatin1Char('0')));
    return object;
}

QJsonArray RemoteStateAdapter::problemsJson() const
{
    QJsonArray list;
    for (const ProblemRow &entry : m_host.problems()) {
        QJsonObject problem;
        problem.insert(QStringLiteral("file"), entry.file);
        problem.insert(QStringLiteral("line"), entry.line);
        problem.insert(QStringLiteral("message"), entry.message);
        problem.insert(QStringLiteral("severity"),
                       entry.error ? QStringLiteral("error") : QStringLiteral("warning"));
        list.append(problem);
    }
    return list;
}

QJsonArray RemoteStateAdapter::tabsJson() const
{
    QJsonArray list;
    QTabWidget *const tabs = m_host.tabs();
    for (int i = 0; i < tabs->count(); ++i) {
        QJsonObject tab;
        QString path;
        bool modified = false;
        if (auto *editor = qobject_cast<CodeEditor *>(tabs->widget(i))) {
            path = editor->filePath();
            modified = editor->isModifiedSinceLoad();
        } else if (auto *image = qobject_cast<ImageEditor *>(tabs->widget(i))) {
            path = image->filePath();
            modified = image->isModifiedSinceLoad();
        }
        tab.insert(QStringLiteral("path"), path);
        tab.insert(QStringLiteral("modified"), modified);
        tab.insert(QStringLiteral("current"), i == tabs->currentIndex());
        list.append(tab);
    }
    return list;
}

QJsonArray RemoteStateAdapter::symbolsJson(const QString &filter) const
{
    QJsonArray list;
    for (const SymbolEntry &sym : m_host.symbols()) {
        if (!filter.isEmpty() && !sym.name.contains(filter, Qt::CaseInsensitive))
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("name"), sym.name);
        if (!sym.file.isEmpty()) {
            object.insert(QStringLiteral("file"), sym.file);
            object.insert(QStringLiteral("line"), sym.line);
            // SymbolsView's honesty rule, shared: an address only once the map
            // has live bases, and only from the definition's own line.
            if (m_host.programMap().isResolved()) {
                quint32 address = 0;
                if (m_host.programMap().codeAddressFor(sym.file, sym.line, &address))
                    object.insert(QStringLiteral("address"),
                                  QStringLiteral("0x%1").arg(hex::hex32(address, hex::Case::Lower)));
            }
        }
        list.append(object);
    }
    return list;
}

QJsonArray RemoteStateAdapter::profilerResultsJson() const
{
    QJsonArray list;
    // From the session's attributed profile, not out of the profiler dock
    // (MAJ-44): the verb answers with the same numbers the gutter heat is
    // scaled from even when no dock has ever been shown.
    const QHash<int, quint64> counts = lineCounts(m_host.profile());
    QList<QPair<int, quint64>> sorted;
    sorted.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it)
        sorted.append({it.key(), it.value()});
    // Counts descend; ties break by line, the same order the profiler table
    // and the gutter heat use, so all three presentations agree.
    std::sort(sorted.begin(), sorted.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
    });
    for (const auto &[line, count] : sorted) {
        QJsonObject entry;
        entry.insert(QStringLiteral("line"), line);
        // A number, not a string: agents do arithmetic on these.
        entry.insert(QStringLiteral("count"), QJsonValue::fromVariant(count));
        list.append(entry);
    }
    return list;
}

QString RemoteStateAdapter::stateSummary() const
{
    if (!m_host.lastState().regs.valid) {
        // "no session state" reads like failure to a remote caller whose
        // natural first move after `run` is `state`; say why instead.
        const IDebugBackend *const backend = m_host.backend();
        if (backend && backend->isRunning() && !backend->isStopped())
            return MainWindow::tr("machine is running; state is captured when the debugger stops\n");
        return MainWindow::tr("no session state\n");
    }

    const Registers &r = m_host.lastState().regs;
    QStringList lines;
    lines << QStringLiteral("pc  %1").arg(m_host.lastState().pc, 8, 16, QLatin1Char('0'));
    for (int i = 0; i < 8; ++i)
        lines << QStringLiteral("d%1  %2").arg(i).arg(r.d[i], 8, 16, QLatin1Char('0'));
    for (int i = 0; i < 8; ++i)
        lines << QStringLiteral("a%1  %2").arg(i).arg(r.a[i], 8, 16, QLatin1Char('0'));
    lines << QStringLiteral("sr  %1").arg(r.sr, 4, 16, QLatin1Char('0'));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace pist
