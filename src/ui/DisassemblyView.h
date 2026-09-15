// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"

#include <QWidget>

class QTableWidget;

namespace pist {

/// Disassembly listing with symbol labels and the current PC highlighted.
class DisassemblyView : public QWidget
{
    Q_OBJECT

public:
    explicit DisassemblyView(QWidget *parent = nullptr);

public slots:
    void setState(const pist::MachineState &state);
    void goToAddress(quint32 address);

    /// Re-apply the theme font and the last listing's colours.
    void applyAppearance();

private:
    QTableWidget *m_table = nullptr;
    quint32 m_pc = 0;
    MachineState m_lastState;
    bool m_haveState = false;
};

} // namespace pist
