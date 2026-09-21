// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"

#include <QHash>
#include <QWidget>

class QLabel;
class QTableWidget;
class QTableWidgetItem;
class QWidget;

namespace pist {

/// D0-D7, A0-A7, PC/SR/USP/ISP with flag indicators.
///
/// Values are editable when the machine is stopped: D and A registers, PC and
/// SR can be written through the debugger (`r <reg>=<value>`). USP and ISP are
/// read-only, because Hatari cannot set them by name (its own todo notes SP/SSP
/// register names are unimplemented).
class RegistersView : public QWidget
{
    Q_OBJECT

public:
    explicit RegistersView(QWidget *parent = nullptr);

public slots:
    void setState(const pist::MachineState &state);

    /// Whether register values can be edited. Only meaningful when the machine
    /// is stopped — writes go through the debugger, which is only at a prompt
    /// then.
    void setEditingEnabled(bool enabled);

    /// Re-apply the theme font and the last shown values' colours.
    void applyAppearance();

signals:
    /// The user committed a new value for a register (e.g. "d0", "a7", "pc").
    void registerEdited(const QString &regName, quint32 value);

private:
    void setValue(int row, int column, quint32 value);
    void onCellEdited(QTableWidgetItem *item);

    QTableWidget *m_table = nullptr;
    QTableWidget *m_flags = nullptr;
    QWidget *m_flagRow = nullptr;
    QLabel *m_flagChip[5] = {};
    QLabel *m_placeholder = nullptr;

    /// Last shown value per cell, so invalid edit input can be reverted.
    QHash<QTableWidgetItem *, QString> m_lastValues;
    MachineState m_lastState;
    bool m_haveState = false;
};

} // namespace pist
