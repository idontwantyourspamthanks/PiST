// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QWidget>

class QFrame;
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

    /// The demo projects that exist where this PiST is running; see
    /// demoProjects().
    void setDemoProjects(const QList<Entry> &demos);

    /// Toolchain readiness: the lines that say what was found, and whether
    /// something is missing (which the Setup… button is there for).
    void setReadiness(bool missing, const QStringList &lines);

    void setStats(const Stats &stats);

    /// Re-apply the theme's inks to the fixed labels. MainWindow's
    /// applyAppearance() calls this with every other panel, so switching theme
    /// with the welcome view on show does not leave the old theme's greys.
    void applyAppearance();

    /// The demo projects PiST can offer from where it is running — this
    /// checkout's, or the ones an installed PiST carries — or an empty list when
    /// it has neither.
    ///
    /// The lookup is deliberately dull, and it is one walk of the same
    /// directories. In a source checkout the demos are the repository's `demo/`:
    /// the project files beside the process's working directory, which is where
    /// `run.sh` and the tests both start, then walking up from the executable,
    /// which sits in `build/` inside the tree — that answer is asked for first
    /// and is unchanged. An installed PiST carries the same files in its own
    /// data directory, `share/pist/demo` under the prefix it was installed into
    /// (CMakeLists.txt installs them there), which the same upwards walk reaches
    /// without being told where the prefix is: `bin/pist` reaches `../share`, an
    /// AppImage's `usr/bin` reaches `usr/share`, a macOS bundle's
    /// `Contents/MacOS` reaches `Contents/share`, a deb/rpm's private
    /// `lib/pist/bin` reaches `lib/pist/share`. It is the walk
    /// `paths::bundledDataSearchPaths()` makes for the ROM.
    ///
    /// A candidate counts only when the demo it names is really there, so a row
    /// that would open nothing is never offered.
    ///
    /// The paths are writable. A source checkout's `demo/` is used as it is; a
    /// demo found under an installed PiST's read-only prefix is copied to
    /// `paths::suggestedDemoDir()` on first use, because opening a project has
    /// to be able to write its build outputs and saves beside the source. The
    /// copy is reused on later runs, so edits made to it are kept.
    static QList<Entry> demoProjects();

    /// Translate a document inside a read-only demo tree to its writable copy.
    ///
    /// `last/source` and the recent list can still name the installed path — a
    /// session that tried to build it before this copy existed wrote it there —
    /// and so can a command line or a project opened by hand. Returns `path`
    /// unchanged when it is not inside the demo tree, the tree is writable, or
    /// no copy could be made.
    static QString writableDemoPath(const QString &path);

signals:
    void openRequested(const QString &path);
    void newFileRequested();
    void newImageRequested();
    void browseRequested();
    void setupRequested();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildDoorways();

    QPixmap m_plate;

    QFrame *m_rule = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_doorTitle = nullptr;
    QLabel *m_readiness = nullptr;
    QLabel *m_stats = nullptr;
    /// Whether the readiness lines name a missing piece, so applyAppearance()
    /// can ink them the way setReadiness() did.
    bool m_readinessMissing = false;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rows = nullptr;
    QList<Entry> m_recents;
    QList<Entry> m_demos;
};

} // namespace pist
