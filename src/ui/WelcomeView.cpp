// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/WelcomeView.h"

#include "ui/Appearance.h"

#include <QCoreApplication>
#include <QDir>
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
        QColor fill = palette().color(QPalette::Base);
        fill.setAlpha(underMouse() ? 235 : 210);
        const QColor border = palette().color(underMouse() ? QPalette::Highlight : QPalette::Mid);
        painter.setPen(QPen(border, 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -2, -2), 8, 8);

        const QRect textRect = rect().adjusted(16, 8, -14, -8);
        const QString path = toolTip();
        const bool twoLines = !path.isEmpty() && path != text();
        QFont title = font();
        title.setBold(true);
        painter.setFont(title);
        painter.setPen(palette().color(QPalette::Text));
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
    columnHost->setMinimumWidth(440);
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
    update();
}

QList<WelcomeView::Entry> WelcomeView::demoProjects()
{
    // The demos are in one of two places, and one walk of the same directories
    // finds either. In a source checkout they are the repository's `demo/`: the
    // working directory first (run.sh and ctest both start in the tree), then
    // upwards from the executable, which sits in build/ inside it. An installed
    // PiST carries the same files in its own data directory instead —
    // `share/pist/demo` under the prefix it was installed into — and the same
    // upwards walk reaches that without knowing where the prefix is: `bin/pist`
    // reaches `../share`, the AppImage's `usr/bin` reaches `usr/share`, a macOS
    // bundle's `Contents/MacOS` reaches `Contents/share`, and an installed
    // package's private `lib/pist/bin` reaches `lib/pist/share`. This is the
    // walk paths::bundledDataSearchPaths() makes for the ROM.
    QStringList bases;
    bases << QDir::currentPath();
    QDir up(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 3; ++depth) {
        bases << up.absolutePath();
        if (!up.cdUp())
            break;
    }

    // The checkout is asked first: a developer sitting in their tree wants the
    // tree's demos, and that keeps the source-checkout behaviour exactly what it
    // was. Either way the marker is the same file, and a directory that lacks it
    // is not the demo set — a row that opens nothing is worse than no row.
    const auto findDemos = [&bases](const QString &relative) {
        for (const QString &base : bases) {
            const QString candidate = QDir(base).filePath(relative);
            if (QFileInfo::exists(
                    QDir(candidate).filePath(QStringLiteral("hello.pistproject"))))
                return candidate;
        }
        return QString();
    };
    QString demoDir = findDemos(QStringLiteral("demo"));
    if (demoDir.isEmpty())
        demoDir = findDemos(QStringLiteral("share/pist/demo"));
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
        auto *row = new DoorButton(m_rowsHost);
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
