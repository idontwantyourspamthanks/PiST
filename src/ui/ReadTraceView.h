// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/ReadTrace.h"

#include <QStringList>
#include <QWidget>

class QLabel;
class QTableWidget;

namespace pist {

/// CPU data reads in the range a memory pane is tracing. Oldest first.
/// Double-clicking a row does not move the machine. Previous Read does.
class ReadTraceView : public QWidget
{
    Q_OBJECT

public:
    explicit ReadTraceView(QWidget *parent = nullptr);

    void setTrace(const ReadTraceLog &log, const QStringList &where);
    void clear();
    /// True when `row` is a CPU read. `pc` is the instruction that read.
    bool cpuPcAt(int row, quint32 *pc) const;

signals:
    void eventActivated(quint32 pc, quint32 address);
    void previousReadRequested(quint32 pc);

private:
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
    QVector<ReadEvent> m_events;
};

} // namespace pist
