// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/WelcomeView.h"

#include "ui/Appearance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace pist {

namespace {

/// Paint a label in one of the theme's inks. Colours are read at call time —
/// the theme can change under a window that stays open.
void useInk(QLabel *label, const QColor &colour)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, colour);
    label->setPalette(palette);
}

} // namespace

WelcomeView::WelcomeView(QWidget *parent)
    : QWidget(parent)
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(28, 24, 28, 20);
    column->setSpacing(10);

    auto *title = new QLabel(tr("PiST"), this);
    title->setObjectName(QStringLiteral("welcomeTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.8);
    titleFont.setBold(true);
    title->setFont(titleFont);
    column->addWidget(title);

    m_subtitle = new QLabel(tr("Atari ST 68000 assembly — edit, build, run, debug"), this);
    m_subtitle->setObjectName(QStringLiteral("welcomeSubtitle"));
    column->addWidget(m_subtitle);
    column->addSpacing(6);

    m_doorTitle = new QLabel(this);
    m_doorTitle->setObjectName(QStringLiteral("welcomeDoorTitle"));
    column->addWidget(m_doorTitle);

    m_rowsHost = new QWidget(this);
    m_rowsHost->setObjectName(QStringLiteral("welcomeRows"));
    m_rows = new QVBoxLayout(m_rowsHost);
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(2);
    column->addWidget(m_rowsHost);

    auto *actions = new QHBoxLayout;
    // The teaching layer's doorway comes first: a person who has never written
    // 68000 assembly is the reason the lessons exist, and Open… assumes they
    // already have something to open (docs/tutorial-plan.md §8).
    m_learn = new QPushButton(tr("Learn 68000 assembly"), this);
    m_learn->setObjectName(QStringLiteral("welcomeLearn"));
    m_learn->setToolTip(tr("Work through a lesson with the debugger beside you"));
    connect(m_learn, &QPushButton::clicked, this, &WelcomeView::learnRequested);
    auto *open = new QPushButton(tr("Open…"), this);
    open->setObjectName(QStringLiteral("welcomeOpen"));
    connect(open, &QPushButton::clicked, this, &WelcomeView::browseRequested);
    auto *newFile = new QPushButton(tr("New File…"), this);
    newFile->setObjectName(QStringLiteral("welcomeNewFile"));
    connect(newFile, &QPushButton::clicked, this, &WelcomeView::newFileRequested);
    auto *newImage = new QPushButton(tr("New Image…"), this);
    newImage->setObjectName(QStringLiteral("welcomeNewImage"));
    connect(newImage, &QPushButton::clicked, this, &WelcomeView::newImageRequested);
    actions->addWidget(m_learn);
    actions->addWidget(open);
    actions->addWidget(newFile);
    actions->addWidget(newImage);
    actions->addStretch(1);
    column->addSpacing(6);
    column->addLayout(actions);

    auto *readiness = new QHBoxLayout;
    m_readiness = new QLabel(this);
    m_readiness->setObjectName(QStringLiteral("welcomeReadiness"));
    m_readiness->setWordWrap(true);
    auto *setup = new QPushButton(tr("Setup…"), this);
    setup->setObjectName(QStringLiteral("welcomeSetup"));
    connect(setup, &QPushButton::clicked, this, &WelcomeView::setupRequested);
    readiness->addWidget(m_readiness, 1);
    readiness->addWidget(setup, 0, Qt::AlignTop);
    column->addSpacing(6);
    column->addLayout(readiness);

    column->addStretch(1);

    // The shelf: a corner block, not a panel (docs/untitled-plan.md §6).
    m_stats = new QLabel(this);
    m_stats->setObjectName(QStringLiteral("welcomeStats"));
    m_stats->setAlignment(Qt::AlignRight);
    m_stats->setWordWrap(true);
    column->addWidget(m_stats);

    applyAppearance();

    rebuildDoorways();
}

void WelcomeView::setRecentSources(const QStringList &sources)
{
    m_recents.clear();
    for (const QString &path : sources) {
        // The Open Recent menu prunes vanished files at show time; a row here
        // has to as well, or the doorway opens nothing.
        if (!QFileInfo::exists(path))
            continue;
        m_recents.append({ QFileInfo(path).fileName(), path, path });
    }
    rebuildDoorways();
}

void WelcomeView::setDemoProjects(const QList<Entry> &demos)
{
    m_demos = demos;
    rebuildDoorways();
}

void WelcomeView::setReadiness(bool missing, const QStringList &lines)
{
    m_readinessMissing = missing;
    m_readiness->setText(lines.join(QLatin1Char('\n')));
    applyAppearance();
}

void WelcomeView::setLearnAvailable(bool available)
{
    if (m_learn)
        m_learn->setVisible(available);
}

void WelcomeView::setStats(const Stats &stats)
{
    const QLocale locale = QLocale::system();
    QStringList lines;
    lines << tr("%1 sessions · %2 builds · %3 runs · %4 files opened")
                 .arg(locale.toString(stats.sessions), locale.toString(stats.builds),
                      locale.toString(stats.runs), locale.toString(stats.opened));
    if (!stats.firstLaunch.isEmpty())
        lines << tr("counting since %1").arg(stats.firstLaunch);
    if (stats.lastSessionKnown) {
        const QString outcome = stats.lastBuildOk ? tr("succeeded") : tr("failed");
        QString summary = tr("last session: %1 build%2, the last one %3")
                              .arg(stats.lastSessionBuilds)
                              .arg(stats.lastSessionBuilds == 1 ? QString() : QStringLiteral("s"))
                              .arg(outcome);
        if (!stats.lastFile.isEmpty())
            summary += tr(" on %1").arg(QFileInfo(stats.lastFile).fileName());
        lines << summary;
    }
    m_stats->setText(lines.join(QLatin1Char('\n')));
    applyAppearance();
}

void WelcomeView::applyAppearance()
{
    // The one place the fixed labels take their inks: the constructor, the
    // setters that fill them, and MainWindow::applyAppearance() all come here,
    // so a theme change with the welcome view on show cannot leave the old
    // theme's greys behind.
    const appearance::Colors colors = appearance::colors();
    useInk(m_subtitle, colors.muted);
    useInk(m_doorTitle, colors.muted);
    useInk(m_readiness, m_readinessMissing ? colors.warning : colors.muted);
    useInk(m_stats, colors.muted);
}

QList<WelcomeView::Entry> WelcomeView::demoProjects()
{
    // Where the repository is, if this is a development build: the working
    // directory first (run.sh and ctest both start in the tree), then upwards
    // from the executable, which sits in build/ inside it.
    QStringList bases;
    bases << QDir::currentPath();
    QDir up(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 3; ++depth) {
        bases << up.absolutePath();
        if (!up.cdUp())
            break;
    }
    QString demoDir;
    for (const QString &base : bases) {
        const QString candidate = QDir(base).filePath(QStringLiteral("demo"));
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("hello.pistproject")))) {
            demoDir = candidate;
            break;
        }
    }
    if (demoDir.isEmpty())
        return {};

    const QDir dir(demoDir);
    const QList<Entry> shipped = {
        { tr("Hello world"), dir.filePath(QStringLiteral("hello.pistproject")), {} },
        { tr("Keyboard and sound"), dir.filePath(QStringLiteral("keyred.pistproject")), {} },
        { tr("Sprite pipeline"),
          dir.filePath(QStringLiteral("spritedemo/sprites.pistproject")), {} },
    };
    QList<Entry> present;
    for (const Entry &entry : shipped) {
        if (!QFileInfo::exists(entry.path))
            continue;
        Entry row = entry;
        row.detail = entry.path;
        present.append(row);
    }
    return present;
}

void WelcomeView::rebuildDoorways()
{
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    // The demos are the first run's doorway and step aside once the user has
    // documents of their own to come back to.
    const QList<Entry> &entries = m_recents.isEmpty() ? m_demos : m_recents;
    m_doorTitle->setVisible(!entries.isEmpty());
    m_rowsHost->setVisible(!entries.isEmpty());
    if (entries.isEmpty())
        return;

    m_doorTitle->setText(m_recents.isEmpty() ? tr("Start with a demo project")
                                             : tr("Recent files"));
    for (const Entry &entry : entries) {
        auto *row = new QToolButton(m_rowsHost);
        // Text-only and flat: a list of files, not a row of buttons.
        row->setObjectName(QStringLiteral("welcomeRow"));
        row->setToolButtonStyle(Qt::ToolButtonTextOnly);
        row->setAutoRaise(true);
        row->setCursor(Qt::PointingHandCursor);
        row->setText(entry.label);
        row->setToolTip(entry.detail.isEmpty() ? entry.path : entry.detail);
        const QString path = entry.path;
        connect(row, &QToolButton::clicked, this, [this, path] { emit openRequested(path); });
        m_rows->addWidget(row, 0, Qt::AlignLeft);
    }
}

} // namespace pist
