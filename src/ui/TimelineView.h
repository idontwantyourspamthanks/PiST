// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QListWidget;

namespace pist {

/// The recorded stops, and the recent instructions Hatari's history ring
/// still holds. Choosing a stop shows it. Choosing an instruction returns
/// the machine to the first reach of that program counter.
class TimelineView : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineView(QWidget *parent = nullptr);

    void setStops(const QStringList &labels);
    void setInstructions(const QStringList &labels, const QList<quint32> &pcs);
    void clear();

signals:
    void stopActivated(int index);
    void instructionActivated(quint32 pc);

private:
    QListWidget *m_stops = nullptr;
    QListWidget *m_instructions = nullptr;
    QList<quint32> m_pcs;
};

} // namespace pist
