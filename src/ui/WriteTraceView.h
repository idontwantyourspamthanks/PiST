// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/WriteTrace.h"

#include <QStringList>
#include <QWidget>

class QLabel;
class QTableWidget;

namespace pist {

/// Who wrote the range a memory pane is tracing, since Trace was turned on.
/// Oldest first, which is the order the ring keeps: a drop count means later
/// writes are not in the list. Choosing a row does not move the machine.
class WriteTraceView : public QWidget
{
    Q_OBJECT

public:
    explicit WriteTraceView(QWidget *parent = nullptr);

    void setTrace(const WriteTraceLog &log, const QStringList &where);
    void clear();

signals:
    /// A row was double-clicked. `hasPc` is true only for a CPU store. The
    /// host opens that PC's source line when the program map knows it.
    void eventActivated(quint32 pc, bool hasPc, quint32 address);

private:
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
    QVector<WriteEvent> m_events;
};

} // namespace pist
