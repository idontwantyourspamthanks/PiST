// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ProfilerView.h"

#include "ui/Appearance.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

constexpr int kColName = 0;
constexpr int kColCount = 1;
constexpr int kColCycles = 2;
constexpr int kColCountPct = 3;
constexpr int kColCyclesPct = 4;

} // namespace

ProfilerView::ProfilerView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(4);
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("profilerFilter"));
    m_filter->setPlaceholderText(tr("Filter routines and lines"));
    m_filter->setClearButtonEnabled(true);
    filterRow->addWidget(m_filter, 1);

    m_showAll = new QCheckBox(tr("Show all"), this);
    m_showAll->setObjectName(QStringLiteral("profilerShowAll"));
    m_showAll->setToolTip(tr("Also show rows under 0.1% of the run"));
    filterRow->addWidget(m_showAll);

    layout->addLayout(filterRow);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("profilerTree"));
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("Routine / Line"), tr("Count"), tr("Cycles"), tr("% cnt"), tr("% cyc")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    appearance::markMono(m_tree);
    layout->addWidget(m_tree, 1);

    // The actions sit at the bottom, where a button belongs in a dock; the row
    // is created empty and setActions fills it with the window's actions.
    m_buttonRow = new QHBoxLayout;
    m_buttonRow->setSpacing(4);
    layout->addLayout(m_buttonRow);

    // The status gets the bottom line to itself: full width, so a real
    // sentence fits without stretching the filter row.
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("profilerStatus"));
    layout->addWidget(m_status);

    // Double-click is what every other list in the IDE uses for "go there"
    // (BreakpointPanel, StackView, MemoryView), so a hot line — or the routine
    // holding it — behaves the same.
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (!item)
            return;
        const int line = item->data(kColName, Qt::UserRole).toInt();
        if (line > 0)
            emit lineActivated(line);
    });
    connect(m_filter, &QLineEdit::textChanged, this, &ProfilerView::applyFilter);
    connect(m_showAll, &QCheckBox::toggled, this, &ProfilerView::applyFilter);

    // Until the first profile arrives, the status line is the manual: the
    // choreography is short, but nobody should have to find it in a tooltip.
    clear();
}

void ProfilerView::setActions(QAction *start, QAction *stop, QAction *toCursor)
{
    const std::pair<QAction *, const char *> buttons[] = {
        {start, "profilerStartButton"},
        {stop, "profilerStopButton"},
        {toCursor, "profilerToCursorButton"},
    };
    for (const auto &[action, name] : buttons) {
        if (!action)
            continue;
        auto *button = new QToolButton(this);
        button->setDefaultAction(action);
        button->setObjectName(QString::fromLatin1(name));
        // Glyph + tooltip only: the action's text would blow out the row.
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_buttonRow->addWidget(button);
    }
    m_buttonRow->addStretch(1);
}

void ProfilerView::clear()
{
    m_profile = AttributedProfile();
    m_tree->clear();
    m_status->setText(tr("Set a breakpoint where measuring ends, Profile Start, "
                         "continue — the hot lines appear here."));
}

void ProfilerView::setProfile(const AttributedProfile &profile)
{
    // The analysis was done by attributedProfile() before this was called
    // (MAJ-44); what is left here is showing it. The widget keeps the value so
    // filtering and a theme change can redraw, and nothing outside reads it.
    m_profile = profile;
    m_tree->clear();

    // An empty profile and an unmappable one are different stories for the
    // user — "the profiler found nothing" against "profiling is not wired up
    // for this program yet" — and neither shows a table.
    if (!profile.hasSamples) {
        m_status->setText(tr("no profile"));
        return;
    }
    if (!profile.resolved) {
        m_status->setText(tr("%1 samples, no source map").arg(profile.totalCount));
        return;
    }

    populate();
}

void ProfilerView::showMessage(const QString &message)
{
    m_status->setText(message);
}

void ProfilerView::setRow(QTreeWidgetItem *item, const QString &name, quint64 count,
                          quint64 cycles) const
{
    const QLocale locale;
    item->setText(kColName, name);
    item->setText(kColCount, locale.toString(count));
    item->setText(kColCycles, locale.toString(cycles));
    item->setText(kColCountPct,
                  QString::number(m_profile.totalCount ? 100.0 * double(count) / double(m_profile.totalCount) : 0.0,
                                  'f', 2));
    item->setText(kColCyclesPct,
                  QString::number(m_profile.totalCycles ? 100.0 * double(cycles) / double(m_profile.totalCycles)
                                                : 0.0,
                                  'f', 2));
    for (int col = kColCount; col <= kColCyclesPct; ++col)
        item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);
}

void ProfilerView::populate()
{
    m_tree->clear();

    for (const AttributedRoutine &routine : m_profile.routines) {
        auto *root = new QTreeWidgetItem(m_tree);
        setRow(root, routine.name, routine.count, routine.cycles);
        // A routine row activates to its label's line; a zero stays inert.
        root->setData(kColName, Qt::UserRole, routine.defLine);
        for (const AttributedLine &line : routine.lines) {
            auto *child = new QTreeWidgetItem(root);
            setRow(child, QString::number(line.line), line.count, line.cycles);
            child->setData(kColName, Qt::UserRole, line.line);
        }
    }

    if (!m_profile.rom.isEmpty()) {
        quint64 romCount = 0, romCycles = 0;
        for (const AttributedRoutine &region : m_profile.rom) {
            romCount += region.count;
            romCycles += region.cycles;
        }
        auto *root = new QTreeWidgetItem(m_tree);
        setRow(root, tr("TOS/ROM"), romCount, romCycles);
        root->setData(kColName, Qt::UserRole, -1); // no source to go to
        for (const AttributedRoutine &region : m_profile.rom) {
            auto *child = new QTreeWidgetItem(root);
            setRow(child, region.name, region.count, region.cycles);
            child->setData(kColName, Qt::UserRole, -1);
        }
    }

    m_tree->expandAll();
    for (int col = 0; col < m_tree->columnCount(); ++col)
        m_tree->resizeColumnToContents(col);

    applyFilter();
}

void ProfilerView::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    const bool showAll = m_showAll->isChecked();

    const auto belowFloor = [this, showAll](const QTreeWidgetItem *item) {
        if (showAll || m_profile.totalCycles == 0)
            return false;
        bool ok = false;
        const double share = item->text(kColCyclesPct).toDouble(&ok) / 100.0;
        return ok && share < kMinShare;
    };

    int shownRoots = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *root = m_tree->topLevelItem(i);
        const bool rootMatches = needle.isEmpty()
                                 || root->text(kColName).contains(needle, Qt::CaseInsensitive);
        int matchingChildren = 0;
        for (int c = 0; c < root->childCount(); ++c) {
            QTreeWidgetItem *child = root->child(c);
            const bool childMatches = needle.isEmpty()
                                      || child->text(kColName).contains(needle, Qt::CaseInsensitive)
                                      || rootMatches;
            const bool visible = childMatches && !belowFloor(child);
            child->setHidden(!visible);
            if (childMatches)
                ++matchingChildren;
        }
        const bool rootVisible = rootMatches || matchingChildren > 0;
        root->setHidden(!rootVisible || belowFloor(root));
        if (!root->isHidden())
            ++shownRoots;
    }

    QString status;
    if (m_profile.totalCount == 0) {
        status = m_profile.unmapped ? tr("%1 addresses, none in this file").arg(m_profile.unmapped)
                            : tr("no profiled instructions in this file");
    } else if (m_profile.routines.isEmpty() && m_profile.rom.isEmpty()) {
        // A profile landed but nothing in it belongs to this source: the tree
        // is empty for a reason, and the user should not read it as broken.
        status = tr("nothing in the run maps to this file");
        if (m_profile.unmapped)
            status += tr(" · %1 addresses unmapped").arg(m_profile.unmapped);
    } else if (shownRoots == 0) {
        status = !needle.isEmpty() ? tr("no routines match the filter")
                                   : tr("everything is under 0.1% — Show all to see it");
    } else {
        const QLocale locale;
        status = tr("%1 instructions, %2 cycles")
                     .arg(locale.toString(m_profile.totalCount), locale.toString(m_profile.totalCycles));
        if (m_profile.clockHz) {
            const double ms = 1000.0 * double(m_profile.totalCycles) / double(m_profile.clockHz);
            const double frames = 50.0 * double(m_profile.totalCycles) / double(m_profile.clockHz);
            status += tr(" ≈ %1 ms (%2 frames)")
                          .arg(ms, 0, 'f', 1)
                          .arg(frames, 0, 'f', 2);
        }
        status += tr(" · %1 of %2 routines").arg(shownRoots).arg(m_profile.routines.size() + (m_profile.rom.isEmpty() ? 0 : 1));
        if (m_profile.unmapped)
            status += tr(" · %1 addresses unmapped").arg(m_profile.unmapped);
    }
    m_status->setText(status);
}

void ProfilerView::applyAppearance()
{
    appearance::markMono(m_tree);
    m_tree->setUniformRowHeights(true);
    applyFilter();
}

} // namespace pist
