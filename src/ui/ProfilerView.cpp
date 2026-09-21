// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ProfilerView.h"

#include "build/LineMap.h"
#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
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
#include <algorithm>

namespace pist {

namespace {

constexpr int kColName = 0;
constexpr int kColCount = 1;
constexpr int kColCycles = 2;
constexpr int kColCountPct = 3;
constexpr int kColCyclesPct = 4;

/// A code label with its resolved address: the anchor a hot address's routine
/// is read from.
struct Anchor
{
    quint32 address = 0;
    QString name;
    int defLine = 0;
};

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
        m_buttonRow->addWidget(button);
    }
}

void ProfilerView::clear()
{
    m_routines.clear();
    m_rom.clear();
    m_unmapped = 0;
    m_unmappedCycles = 0;
    m_totalCount = 0;
    m_totalCycles = 0;
    m_clockHz = 0;
    m_tree->clear();
    m_status->setText(tr("Set a breakpoint where measuring ends, Profile Start, "
                         "continue — the hot lines appear here."));
}

void ProfilerView::setProfile(const ProfileData &profile, const ProgramLineMap *map,
                              const QString &sourceFile, const QVector<SymbolEntry> &symbols)
{
    clear();
    if (profile.lines.isEmpty()) {
        m_status->setText(tr("no profile"));
        return;
    }
    m_totalCount = profile.totalCount;
    m_totalCycles = profile.totalCycles;
    m_clockHz = profile.clockHz;

    // Without a resolved map there is no address-to-line mapping at all, which
    // is the normal state before the program has been built and run. Saying so
    // is the difference between "the profiler found nothing" and "profiling is
    // not wired up for this program yet".
    if (!map || map->isEmpty() || !map->isResolved()) {
        m_status->setText(tr("%1 samples, no source map").arg(m_totalCount));
        return;
    }

    // Routine anchors: the current file's code labels with their resolved
    // addresses. A label's own line often emits no code, so the anchor is the
    // first line at or after it that did.
    QList<Anchor> anchors;
    for (const SymbolEntry &sym : symbols) {
        if (sym.file.isEmpty() || !LineMap::sameSource(sym.file, sourceFile))
            continue;
        const int codeLine = map->nextCodeLine(sym.file, sym.line);
        if (codeLine <= 0)
            continue;
        quint32 address = 0;
        if (!map->codeAddressFor(sym.file, codeLine, &address))
            continue;
        anchors.append({address, sym.name, sym.line});
    }
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor &a, const Anchor &b) { return a.address < b.address; });

    const auto routineFor = [&anchors](quint32 address) -> const Anchor * {
        const Anchor *found = nullptr;
        for (const Anchor &anchor : anchors) {
            if (anchor.address > address)
                break;
            found = &anchor;
        }
        return found;
    };

    // The ROM areas an address can belong to: time in trap handlers is the
    // OS's, not any source line's. Name-based, with the well-known ROM floor
    // as the fallback for a save with no area lines.
    const auto romRegionFor = [&profile](quint32 address) -> QString {
        for (const ProfileRegion &region : profile.regions) {
            if (address >= region.first && address <= region.last
                && (region.name == QLatin1String("ROM_TOS")
                    || region.name == QLatin1String("CARTRIDGE")))
                return region.name;
        }
        if (profile.regions.isEmpty() && address >= 0xfa0000)
            return QStringLiteral("ROM");
        return QString();
    };

    QHash<int, int> lineIndex;     // line -> index in its routine's lines
    QHash<QString, int> routineOf; // routine name -> index in m_routines
    QHash<QString, int> romOf;     // region name -> index in m_rom
    for (const ProfileLine &entry : profile.lines) {
        LineMap::Address where;
        if (map->lineFor(entry.address, &where) && where.line > 0
            && LineMap::sameSource(where.file, sourceFile)) {
            const Anchor *anchor = routineFor(entry.address);
            const QString routine = anchor ? anchor->name : tr("(no routine)");
            int ri = routineOf.value(routine, -1);
            if (ri < 0) {
                ri = m_routines.size();
                routineOf.insert(routine, ri);
                RoutineCost cost;
                cost.name = routine;
                cost.defLine = anchor ? anchor->defLine : 0;
                m_routines.append(cost);
            }
            RoutineCost &routine_ = m_routines[ri];
            routine_.count += entry.count;
            routine_.cycles += entry.cycles;

            const int li = lineIndex.value(where.line, -1);
            if (li >= 0) {
                routine_.lines[li].count += entry.count;
                routine_.lines[li].cycles += entry.cycles;
            } else {
                lineIndex.insert(where.line, routine_.lines.size());
                routine_.lines.append({where.line, routine, entry.count, entry.cycles});
            }
            continue;
        }

        const QString region = romRegionFor(entry.address);
        if (!region.isEmpty()) {
            int ri = romOf.value(region, -1);
            if (ri < 0) {
                ri = m_rom.size();
                romOf.insert(region, ri);
                RoutineCost cost;
                cost.name = region;
                m_rom.append(cost);
            }
            m_rom[ri].count += entry.count;
            m_rom[ri].cycles += entry.cycles;
            continue;
        }

        ++m_unmapped;
        m_unmappedCycles += entry.cycles;
    }

    for (RoutineCost &routine : m_routines) {
        std::sort(routine.lines.begin(), routine.lines.end(),
                  [](const LineCost &a, const LineCost &b) {
                      return a.cycles != b.cycles ? a.cycles > b.cycles : a.line < b.line;
                  });
    }
    const auto byCyclesDesc = [](const RoutineCost &a, const RoutineCost &b) {
        return a.cycles != b.cycles ? a.cycles > b.cycles : a.name < b.name;
    };
    std::sort(m_routines.begin(), m_routines.end(), byCyclesDesc);
    std::sort(m_rom.begin(), m_rom.end(), byCyclesDesc);

    populate();
}

void ProfilerView::showMessage(const QString &message)
{
    m_status->setText(message);
}

QHash<int, quint64> ProfilerView::lineCounts() const
{
    QHash<int, quint64> counts;
    for (const RoutineCost &routine : m_routines) {
        for (const LineCost &line : routine.lines)
            counts.insert(line.line, line.count);
    }
    return counts;
}

void ProfilerView::setRow(QTreeWidgetItem *item, const QString &name, quint64 count,
                          quint64 cycles) const
{
    const QLocale locale;
    item->setText(kColName, name);
    item->setText(kColCount, locale.toString(count));
    item->setText(kColCycles, locale.toString(cycles));
    item->setText(kColCountPct,
                  QString::number(m_totalCount ? 100.0 * double(count) / double(m_totalCount) : 0.0,
                                  'f', 2));
    item->setText(kColCyclesPct,
                  QString::number(m_totalCycles ? 100.0 * double(cycles) / double(m_totalCycles)
                                                : 0.0,
                                  'f', 2));
    for (int col = kColCount; col <= kColCyclesPct; ++col)
        item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);
}

void ProfilerView::populate()
{
    m_tree->clear();

    for (const RoutineCost &routine : m_routines) {
        auto *root = new QTreeWidgetItem(m_tree);
        setRow(root, routine.name, routine.count, routine.cycles);
        // A routine row activates to its label's line; a zero stays inert.
        root->setData(kColName, Qt::UserRole, routine.defLine);
        for (const LineCost &line : routine.lines) {
            auto *child = new QTreeWidgetItem(root);
            setRow(child, QString::number(line.line), line.count, line.cycles);
            child->setData(kColName, Qt::UserRole, line.line);
        }
    }

    if (!m_rom.isEmpty()) {
        quint64 romCount = 0, romCycles = 0;
        for (const RoutineCost &region : m_rom) {
            romCount += region.count;
            romCycles += region.cycles;
        }
        auto *root = new QTreeWidgetItem(m_tree);
        setRow(root, tr("TOS/ROM"), romCount, romCycles);
        root->setData(kColName, Qt::UserRole, -1); // no source to go to
        for (const RoutineCost &region : m_rom) {
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
        if (showAll || m_totalCycles == 0)
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
    if (m_totalCount == 0) {
        status = m_unmapped ? tr("%1 addresses, none in this file").arg(m_unmapped)
                            : tr("no profiled instructions in this file");
    } else if (m_routines.isEmpty() && m_rom.isEmpty()) {
        // A profile landed but nothing in it belongs to this source: the tree
        // is empty for a reason, and the user should not read it as broken.
        status = tr("nothing in the run maps to this file");
        if (m_unmapped)
            status += tr(" · %1 addresses unmapped").arg(m_unmapped);
    } else if (shownRoots == 0) {
        status = !needle.isEmpty() ? tr("no routines match the filter")
                                   : tr("everything is under 0.1% — Show all to see it");
    } else {
        const QLocale locale;
        status = tr("%1 instructions, %2 cycles")
                     .arg(locale.toString(m_totalCount), locale.toString(m_totalCycles));
        if (m_clockHz) {
            const double ms = 1000.0 * double(m_totalCycles) / double(m_clockHz);
            const double frames = 50.0 * double(m_totalCycles) / double(m_clockHz);
            status += tr(" ≈ %1 ms (%2 frames)")
                          .arg(ms, 0, 'f', 1)
                          .arg(frames, 0, 'f', 2);
        }
        status += tr(" · %1 of %2 routines").arg(shownRoots).arg(m_routines.size() + (m_rom.isEmpty() ? 0 : 1));
        if (m_unmapped)
            status += tr(" · %1 addresses unmapped").arg(m_unmapped);
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
