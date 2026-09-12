// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"

#include <QWidget>

class QTableWidget;

namespace pist {

/// D0-D7, A0-A7, PC/SR/USP/ISP with flag indicators.
class RegistersView : public QWidget
{
    Q_OBJECT

public:
    explicit RegistersView(QWidget *parent = nullptr);

public slots:
    void setState(const pist::MachineState &state);
    void clear();

private:
    void setValue(int row, int column, quint32 value);

    QTableWidget *m_table = nullptr;
    QTableWidget *m_flags = nullptr;
};

} // namespace pist
