// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
#include "emu/AttributedProfile.h"
#include "emu/MachineState.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <utility>

class QTabWidget;

namespace pist {

class CodeEditor;
class IDebugBackend;

/// The window's state as the remote-control interface's JSON and text verbs see
/// it (MAJ-41).
///
/// These are the `read`/`statejson`/`problems`/`tabs`/`symbols`/`profile
/// results`/`state` answers: each one reports what a pane already shows rather
/// than re-deriving it from the project, so an agent sees the same picture the
/// user does. Read-only by construction — every source of that picture arrives
/// as a const access through the `Host` below (MIN-89 — the adapter used to be
/// a `friend` of MainWindow holding it as a pointer).
class RemoteStateAdapter : public QObject
{
public:
    /// One Problems-pane entry, as this adapter reports it. It lives here
    /// because the row's four fields *are* the adapter's `problems` JSON; the
    /// window's Problems pane stores its rows in this type, so the verb reads
    /// the pane's own list with no second shape between them.
    struct ProblemRow
    {
        QString file;
        int line = 0;
        QString message;
        bool error = false;
    };

    /// Where the adapter reads the picture from: the panes and documents
    /// themselves, plus the two derived values (the machine state and the
    /// attributed profile) that a pane shows but does not own. All const.
    struct Host
    {
        /// The current text editor, or null when the current tab is not one.
        std::function<const CodeEditor *()> editor;
        /// The debug transport, for the running/stopped pair.
        std::function<const IDebugBackend *()> backend;
        /// The machine state of the last stop.
        std::function<const MachineState &()> lastState;
        /// The Problems pane's rows, in the pane's own order.
        std::function<const QList<ProblemRow> &()> problems;
        /// The document tabs, which the `tabs` verb walks.
        std::function<QTabWidget *()> tabs;
        /// The build's symbols.
        std::function<const QVector<SymbolEntry> &()> symbols;
        /// The program map, which supplies an address only once it is resolved.
        std::function<const ProgramLineMap &()> programMap;
        /// The session's attributed profile (the `profile results` verb).
        std::function<const AttributedProfile &()> profile;
    };

    explicit RemoteStateAdapter(Host host, QObject *parent = nullptr);

    /// The current editor document as JSON {path, text}, for the remote
    /// `read` verb. JSON rather than raw text because the block protocol
    /// terminates on a lone `.` line, which assembly source can legitimately
    /// contain. Empty object when no source is open.
    QJsonObject documentJson() const;

    /// Machine state as JSON: {running, stopped}, plus pc/d0-7/a0-7/sr when a
    /// register batch has landed. For the remote `statejson` verb, which the
    /// MCP shim serves as structured content.
    QJsonObject stateJson() const;

    /// The Problems pane as JSON objects {file, line, message, severity}, for
    /// the remote `problems` verb.
    QJsonArray problemsJson() const;

    /// Every open document as JSON objects {path, modified, current}, for the
    /// remote `tabs` verb — the agent's map of what the IDE has open.
    QJsonArray tabsJson() const;

    /// The build's symbols as JSON objects {name, file?, line?, address?},
    /// optionally name-filtered (case-insensitive). Addresses appear only once
    /// the program map has live bases — SymbolsView's honesty rule. For the
    /// remote `symbols` verb.
    QJsonArray symbolsJson(const QString &filter) const;

    /// Profiler hot lines as JSON objects {line, count}, sorted by descending
    /// count, for the remote `profile results` verb. Empty when no profile
    /// has been collected.
    QJsonArray profilerResultsJson() const;

    /// A plain-text snapshot of the machine state (registers, PC, running
    /// state), for the remote-control interface. Read-only.
    QString stateSummary() const;

private:
    Host m_host;
};

} // namespace pist
