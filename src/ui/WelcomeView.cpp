// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/WelcomeView.h"

#include "emu/Paths.h"
#include "project/ProjectSettings.h"
#include "ui/Appearance.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLocale>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

// File scope, not a namespace: Q_INIT_RESOURCE's extern declaration binds in
// the innermost enclosing namespace, and the generated init function is global.
static void initWelcomePlate()
{
    Q_INIT_RESOURCE(welcome_plate);
}

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

/// A doorway row: the label, and the path under it. A tool button so the
/// welcome tests still find `welcomeRow`, painted as a card rather than a
/// flat chip — the style's own tool-button label is centred, which a list of
/// files should not be.
class DoorButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // Opaque. A translucent card lets the plate show through, and in the
        // light theme that turns the file name into dark type on the machine.
        const QPalette app = QApplication::palette();
        const QColor fill = app.color(underMouse() ? QPalette::AlternateBase : QPalette::Base);
        const QColor border = app.color(underMouse() ? QPalette::Highlight : QPalette::Mid);
        painter.setPen(QPen(border, 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -2, -2), 8, 8);
        // A row that can be reached with Tab must show it: the card is the only
        // thing on the welcome page with a keyboard path, and a QToolButton's
        // default style would draw that state under this paint.
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(app.color(QPalette::Highlight), 2));
            painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -3, -3), 7, 7);
        }

        const QRect textRect = rect().adjusted(16, 8, -14, -8);
        const QString path = toolTip();
        const bool twoLines = !path.isEmpty() && path != text();
        QFont title = font();
        title.setBold(true);
        painter.setFont(title);
        painter.setPen(app.color(QPalette::Text));
        if (!twoLines) {
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text());
            return;
        }
        const QRect titleRect(textRect.left(), textRect.top(), textRect.width(), textRect.height() / 2);
        painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, text());
        QFont sub = font();
        sub.setPointSizeF(sub.pointSizeF() * 0.92);
        painter.setFont(sub);
        painter.setPen(appearance::colors().muted);
        const QString shown = painter.fontMetrics().elidedText(
            QDir::toNativeSeparators(path), Qt::ElideMiddle, textRect.width());
        const QRect subRect(textRect.left(), textRect.center().y() - 1, textRect.width(),
                            textRect.height() / 2);
        painter.drawText(subRect, Qt::AlignVCenter | Qt::AlignLeft, shown);
    }
};

/// Copy what is missing from `from` into `to`, creating directories as needed.
///
/// Never overwrites: the destination is a working copy the user may have
/// edited, so an existing file wins. Filling in missing entries repairs a copy
/// that an earlier run was interrupted half-way through.
///
/// Symlinks are skipped. The installed demo tree contains none, and following
/// one could copy something outside the demo.
bool copyMissingDemoFiles(const QString &from, const QString &to, QStringList *created,
                          QString *error)
{
    if (!QDir().mkpath(to)) {
        *error = QObject::tr("cannot create '%1'").arg(to);
        return false;
    }

    const QFileInfoList entries =
        QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                     | QDir::Hidden,
                                 QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink())
            continue;
        const QString target = QDir(to).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyMissingDemoFiles(entry.absoluteFilePath(), target, created, error))
                return false;
            continue;
        }
        if (QFileInfo::exists(target))
            continue;
        // NotFound on a copy that a second PiST instance won the race for is
        // success: the file is there, which is all this copy promises.
        if (!QFile::copy(entry.absoluteFilePath(), target) && !QFileInfo::exists(target)) {
            *error = QObject::tr("cannot copy '%1' to '%2'")
                         .arg(entry.absoluteFilePath(), target);
            return false;
        }
        // Only a file this run created may be trusted: a destination file that
        // was already there is the user's working copy, not PiST's provenance.
        if (created)
            created->append(QFileInfo(target).absoluteFilePath());
        // A package may install its content read-only; the copy is a working
        // project, so it has to be editable and rebuildable.
        QFile::setPermissions(target, QFileInfo(target).permissions() | QFile::WriteOwner
                                          | QFile::ReadOwner);
    }
    return true;
}

/// The directory the demos come from, before any writable-copy policy.
///
/// The demos are in one of two places, and one walk of the same directories
/// finds either. In a source checkout they are the repository's `demo/`: the
/// working directory first (run.sh and ctest both start in the tree), then
/// upwards from the executable, which sits in build/ inside it. An installed
/// PiST carries the same files in its own data directory instead —
/// `share/pist/demo` under the prefix it was installed into — and the same
/// upwards walk reaches that without knowing where the prefix is: `bin/pist`
/// reaches `../share`, the AppImage's `usr/bin` reaches `usr/share`, a macOS
/// bundle's `Contents/MacOS` reaches `Contents/share`, and an installed
/// package's private `lib/pist/bin` reaches `lib/pist/share`. This is the
/// walk paths::bundledDataSearchPaths() makes for the ROM.
///
/// Empty when neither place has the marker file.
QString findDemoSourceDir(const QString &applicationDir, bool *installed,
                          bool includeWorkingDirectory = true)
{
    QStringList bases;
    // The welcome view may use a checkout named by the launch directory. Hook
    // provenance may not: that directory belongs to the caller, not to PiST.
    if (includeWorkingDirectory)
        bases << QDir::currentPath();
    QDir up(applicationDir);
    for (int depth = 0; depth < 3; ++depth) {
        bases << up.absolutePath();
        if (!up.cdUp())
            break;
    }

    // The closest base wins, and within one base the checkout's `demo/` wins
    // over its `share/pist/demo/`. Asking every base for `demo/` first would let
    // a checkout found above an installed tree decide that an installed run is a
    // checkout — the distinction this walk exists to make. The marker is the same
    // file either way, and a directory that lacks it is not the demo set: a row
    // that opens nothing is worse than no row.
    const auto hasMarker = [](const QString &dir) {
        return QFileInfo::exists(QDir(dir).filePath(QStringLiteral("hello.pistproject")));
    };
    QString demoDir;
    bool fromInstall = false;
    for (const QString &base : bases) {
        const QString checkout = QDir(base).filePath(QStringLiteral("demo"));
        if (hasMarker(checkout)) {
            demoDir = checkout;
            break;
        }
        const QString shipped = QDir(base).filePath(QStringLiteral("share/pist/demo"));
        if (hasMarker(shipped)) {
            demoDir = shipped;
            fromInstall = true;
            break;
        }
    }
    if (installed)
        *installed = fromInstall;
    return demoDir;
}

/// Whether a demo directory must be copied before use.
///
/// An installed tree is always copied, even when the user can write there: an
/// administrator can usually write inside `/Applications/PiST.app`, and writing
/// listings into a signed bundle breaks its signature and loses the work on the
/// next update. A source checkout is used in place when it is writable; the
/// permission test remains only for a genuinely read-only checkout.
bool demoNeedsCopy(const QString &demoDir, bool installed)
{
    return installed || !QFileInfo(demoDir).isWritable();
}

} // namespace

WelcomeView::WelcomeView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    initWelcomePlate();
    m_plate.load(QStringLiteral(":/pist/desk.png"));

    // The words stay in a column. The plate occupies the rest of the view,
    // painted behind this layout, so the column must not paint its own fill.
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(56, 48, 48, 32);

    auto *columnHost = new QWidget(this);
    columnHost->setObjectName(QStringLiteral("welcomeColumn"));
    // This column's minimum is the whole window's: with the factory docks (268 and
    // 280 wide) a 440 px floor plus the margins left PiST unshrinkable below about
    // 1100 px. The words wrap, so the column's width is a preference, not a floor.
    columnHost->setMinimumWidth(320);
    columnHost->setMaximumWidth(520);
    columnHost->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *column = new QVBoxLayout(columnHost);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(12);

    auto *title = new QLabel(tr("PiST"), columnHost);
    title->setObjectName(QStringLiteral("welcomeTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 2.4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    column->addWidget(title);

    m_rule = new QFrame(columnHost);
    m_rule->setObjectName(QStringLiteral("welcomeRule"));
    m_rule->setFixedSize(36, 3);
    m_rule->setAutoFillBackground(true);
    column->addWidget(m_rule);

    m_subtitle = new QLabel(tr("Atari ST 68000 assembly — edit, build, run, debug"), columnHost);
    m_subtitle->setObjectName(QStringLiteral("welcomeSubtitle"));
    m_subtitle->setWordWrap(true);
    column->addWidget(m_subtitle);
    column->addSpacing(12);

    m_doorTitle = new QLabel(columnHost);
    m_doorTitle->setObjectName(QStringLiteral("welcomeDoorTitle"));
    column->addWidget(m_doorTitle);

    m_rowsHost = new QWidget(columnHost);
    m_rowsHost->setObjectName(QStringLiteral("welcomeRows"));
    m_rows = new QVBoxLayout(m_rowsHost);
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(8);
    column->addWidget(m_rowsHost);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *open = new QPushButton(tr("Open…"), columnHost);
    open->setObjectName(QStringLiteral("welcomeOpen"));
    open->setMinimumHeight(34);
    connect(open, &QPushButton::clicked, this, &WelcomeView::browseRequested);
    auto *newFile = new QPushButton(tr("New File…"), columnHost);
    newFile->setObjectName(QStringLiteral("welcomeNewFile"));
    newFile->setMinimumHeight(34);
    connect(newFile, &QPushButton::clicked, this, &WelcomeView::newFileRequested);
    auto *newImage = new QPushButton(tr("New Image…"), columnHost);
    newImage->setObjectName(QStringLiteral("welcomeNewImage"));
    newImage->setMinimumHeight(34);
    connect(newImage, &QPushButton::clicked, this, &WelcomeView::newImageRequested);
    actions->addWidget(open);
    actions->addWidget(newFile);
    actions->addWidget(newImage);
    actions->addStretch(1);
    column->addSpacing(8);
    column->addLayout(actions);

    auto *readiness = new QHBoxLayout;
    readiness->setSpacing(12);
    m_readiness = new QLabel(columnHost);
    m_readiness->setObjectName(QStringLiteral("welcomeReadiness"));
    m_readiness->setWordWrap(true);
    auto *setup = new QPushButton(tr("Setup…"), columnHost);
    setup->setObjectName(QStringLiteral("welcomeSetup"));
    connect(setup, &QPushButton::clicked, this, &WelcomeView::setupRequested);
    readiness->addWidget(m_readiness, 0, Qt::AlignTop);
    readiness->addWidget(setup, 0, Qt::AlignVCenter);
    readiness->addStretch(1);
    column->addSpacing(8);
    column->addLayout(readiness);

    column->addStretch(1);

    // The shelf: a corner block, not a panel (docs/untitled-plan.md §6).
    m_stats = new QLabel(columnHost);
    m_stats->setObjectName(QStringLiteral("welcomeStats"));
    m_stats->setAlignment(Qt::AlignLeft);
    m_stats->setWordWrap(true);
    column->addWidget(m_stats);

    outer->addWidget(columnHost);
    outer->addStretch(1);

    applyAppearance();

    rebuildDoorways();
}

void WelcomeView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QColor window = palette().color(QPalette::Window);
    painter.fillRect(rect(), window);

    if (!m_plate.isNull()) {
        // Cover the view and pin the right edge, which is where the machine is.
        const qreal scale = qMax(width() / qreal(m_plate.width()), height() / qreal(m_plate.height()));
        const int w = qRound(m_plate.width() * scale);
        const int h = qRound(m_plate.height() * scale);
        painter.drawPixmap(QRect(width() - w, (height() - h) / 2, w, h), m_plate);
    } else {
        QRadialGradient glow(width() * 0.78, height() * 0.62, width() * 0.42);
        QColor green = palette().color(QPalette::Highlight);
        green.setAlpha(64);
        glow.setColorAt(0.0, green);
        green.setAlpha(0);
        glow.setColorAt(1.0, green);
        painter.fillRect(rect(), glow);
    }

    // The column has to stay readable over whatever the plate is doing.
    QLinearGradient scrim(width() * 0.18, 0, width() * 0.62, 0);
    QColor solid = window;
    QColor clear = window;
    clear.setAlpha(0);
    scrim.setColorAt(0.0, solid);
    scrim.setColorAt(0.42, solid);
    scrim.setColorAt(1.0, clear);
    painter.fillRect(rect(), scrim);
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
    if (m_rule) {
        QPalette rule = m_rule->palette();
        rule.setColor(QPalette::Window, palette().color(QPalette::Highlight));
        m_rule->setPalette(rule);
    }
    for (QToolButton *row : findChildren<QToolButton *>(QStringLiteral("welcomeRow")))
        row->update();
    update();
}

QString WelcomeView::demoSourceDir()
{
    return findDemoSourceDir(QCoreApplication::applicationDirPath(), nullptr, false);
}

QString WelcomeView::demoSourceDirFrom(const QString &applicationDir)
{
    return findDemoSourceDir(applicationDir, nullptr, false);
}

QList<WelcomeView::Entry> WelcomeView::demoProjects()
{
    bool installed = false;
    QString demoDir = findDemoSourceDir(QCoreApplication::applicationDirPath(), &installed);
    if (demoDir.isEmpty())
        return {};

    // An installed PiST carries its demos under the read-only prefix
    // (`share/pist/demo`). A demo is a project, not a viewer: the build writes
    // the listing and the program beside the source, and an edit writes the
    // source itself. An installed tree is always copied, even when an
    // administrator can write into it — a source checkout keeps using its own
    // `demo/` when that is writable.
    if (demoNeedsCopy(demoDir, installed)) {
        const QString writable = paths::suggestedDemoDir();
        const QString marker = QDir(writable).filePath(QStringLiteral("hello.pistproject"));

        QStringList created;
        QString error;
        if (!copyMissingDemoFiles(demoDir, writable, &created, &error)
            || !QFileInfo::exists(marker)) {
            // No row is better than one whose first build cannot write its own
            // listing and program. Say why in the log; the GUI has no place for
            // this failure that a first run would show.
            qWarning("cannot make a writable copy of the demo projects: %s",
                     qPrintable(error.isEmpty()
                                    ? QStringLiteral("the copy has no hello.pistproject")
                                    : error));
            return {};
        }
        // Only a project this run copied is PiST's to vouch for. A file that was
        // already in the user's working copy may have been edited or replaced,
        // and must not be trusted just because a sibling was repaired.
        const QString projects[] = {
            QStringLiteral("hello.pistproject"),
            QStringLiteral("keyred.pistproject"),
            QStringLiteral("spritedemo/sprites.pistproject"),
        };
        for (const QString &relative : projects) {
            const QString projectPath = QDir(writable).filePath(relative);
            if (!created.contains(QFileInfo(projectPath).absoluteFilePath()))
                continue;
            ProjectSettings copied;
            QString loadError;
            if (!settings::load(&copied, projectPath, &loadError)
                || copied.stopHookPath.isEmpty())
                continue;
            // The script has to be ours too; a pre-existing script is the
            // user's code, not the shipped file we just copied.
            if (!created.contains(QFileInfo(copied.stopHookPath).absoluteFilePath()))
                continue;
            settings::trustHookProject(projectPath, copied.stopHookPath);
        }
        demoDir = writable;
    }

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

QString WelcomeView::writableDemoPath(const QString &path)
{
    if (path.isEmpty())
        return path;

    bool installed = false;
    const QString sourceDir = findDemoSourceDir(QCoreApplication::applicationDirPath(), &installed);
    if (sourceDir.isEmpty() || !demoNeedsCopy(sourceDir, installed))
        return path;

    // Canonical spellings on both sides, so `..` or a symlinked prefix cannot
    // make a document outside the demo tree look as if it were inside.
    const QString root = QFileInfo(sourceDir).canonicalFilePath();
    const QString document = QFileInfo(path).canonicalFilePath();
    if (root.isEmpty() || document.isEmpty()
        || !document.startsWith(root + QLatin1Char('/')))
        return path;

    // Ask for the projects first: this is what creates the copy (and what
    // repairs one that lost a file). The empty answer means it could not be
    // made, in which case the original path is the only one that exists.
    if (demoProjects().isEmpty())
        return path;

    const QString copy = QDir(paths::suggestedDemoDir())
                             .absoluteFilePath(document.mid(root.size() + 1));
    return QFileInfo::exists(copy) ? copy : path;
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
        auto *row = new DoorButton(m_rowsHost);
        // Reachable with Tab, and the press ring in paintEvent shows where the
        // focus is; without a policy the row is mouse-only.
        row->setFocusPolicy(Qt::StrongFocus);
        row->setObjectName(QStringLiteral("welcomeRow"));
        row->setCursor(Qt::PointingHandCursor);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        row->setMinimumHeight(52);
        row->setText(entry.label);
        row->setToolTip(entry.detail.isEmpty() ? entry.path : entry.detail);
        const QString path = entry.path;
        connect(row, &QToolButton::clicked, this, [this, path] { emit openRequested(path); });
        m_rows->addWidget(row);
    }
}

} // namespace pist
