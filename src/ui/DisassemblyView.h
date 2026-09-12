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
    void clear();
    void goToAddress(quint32 address);

private:
    QTableWidget *m_table = nullptr;
    quint32 m_pc = 0;
};

} // namespace pist
