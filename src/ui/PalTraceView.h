// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/PalTrace.h"

#include <QStringList>
#include <QWidget>

class QLabel;
class QTableWidget;
class QToolButton;

namespace pist {

/// Palette writes since Trace was turned on, each with the scanline and the
/// position on that line taken when the color was stored. Choosing a row
/// does not move the machine.
class PalTraceView : public QWidget
{
    Q_OBJECT

public:
    explicit PalTraceView(QWidget *parent = nullptr);

    void setTrace(const PaletteLog &log, const QStringList &where);
    void clear();
    /// The Trace button, without asking the emulator again.
    void setTraceArmed(bool armed);

signals:
    void traceToggled(bool on);
    /// A row was double-clicked. The host opens that PC's source line when
    /// the program map knows it.
    void eventActivated(quint32 pc);

private:
    QToolButton *m_trace = nullptr;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
    QVector<PaletteEvent> m_events;
};

} // namespace pist
