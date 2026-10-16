// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace pist {

/// What fills the central area when no document is open (docs/untitled-plan.md
/// §5).
///
/// Not a tab, not a dock and not a web view: a plain widget, so it costs
/// nothing to construct, appears the instant the last tab closes, and is driven
/// by the same offscreen tests everything else is. It carries no per-document
/// state — it is a doorway, and the only thing it knows is what its setters
/// were told.
///
/// It is also where the document model explains itself: a first run gets the
/// demo projects, a session in progress gets the documents it had open, and a
/// machine missing vasm gets told so with a way to fix it.
class WelcomeView : public QWidget
{
    Q_OBJECT

public:
    /// One doorway: a file to open, with its name on the button and its path in
    /// the tooltip.
    struct Entry
    {
        QString label;
        QString path;
        QString detail;
    };

    /// The user's own numbers, from their own settings (docs/untitled-plan.md
    /// §6). Nothing here leaves the machine.
    struct Stats
    {
        int sessions = 0;
        int builds = 0;
        int runs = 0;
        int opened = 0;
        QString firstLaunch;      ///< the first launch's date, once there was one
        QString lastFile;         ///< the file the last session was on
        int lastSessionBuilds = 0;
        bool lastBuildOk = false;
        bool lastSessionKnown = false; ///< false until a session has been flushed
    };

    explicit WelcomeView(QWidget *parent = nullptr);

    /// The recent documents, most recent first. Rows are shown for the ones
    /// that still exist — a row that opens nothing is worse than no row — and
    /// the demo rows are held back until this list is empty, so the first run's
    /// three projects do not race the list the user is building
    /// (docs/untitled-plan.md §5.4).
    void setRecentSources(const QStringList &sources);

    /// The demo projects that exist in this checkout; see demoProjects().
    void setDemoProjects(const QList<Entry> &demos);

    /// Toolchain readiness: the lines that say what was found, and whether
    /// something is missing (which the Setup… button is there for).
    void setReadiness(bool missing, const QStringList &lines);

    void setStats(const Stats &stats);

    /// The demo projects shipped beside this PiST, or an empty list when it is
    /// not running from a source tree.
    ///
    /// The demos live in the repository, not in an installed prefix (nothing
    /// installs `demo/`), so a packaged PiST offers no rows rather than rows
    /// that open nothing. The lookup is deliberately dull: the project files
    /// beside the process's working directory, which is where `run.sh` and the
    /// tests both start, then walking up from the executable — a development
    /// build sits in `build/` inside the tree.
    static QList<Entry> demoProjects();

signals:
    void openRequested(const QString &path);
    void newFileRequested();
    void newImageRequested();
    void browseRequested();
    void setupRequested();

private:
    void rebuildDoorways();

    QLabel *m_doorTitle = nullptr;
    QLabel *m_readiness = nullptr;
    QLabel *m_stats = nullptr;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rows = nullptr;
    QList<Entry> m_recents;
    QList<Entry> m_demos;
};

} // namespace pist
