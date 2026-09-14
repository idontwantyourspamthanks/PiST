// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QTableWidget;

namespace pist {

/// Shows the call stack: the 32-bit values at the stack pointer, with the ones
/// that could be return addresses or pointers marked. For assembly debugging
/// this is often the fastest way to see who called the current routine, because
/// a `jsr`/`bsr` pushes the return address as a long.
///
/// The view is read-only and refreshed from a `memdump` at the stack pointer
/// each time the machine stops.
class StackView : public QWidget
{
    Q_OBJECT

public:
    explicit StackView(QWidget *parent = nullptr);

public slots:
    /// Populate from a `memdump` response taken at the stack pointer. The text
    /// segment runs from textBase to textEnd, so values inside it can be marked
    /// as likely return addresses.
    void setStackDump(quint32 sp, const QString &response,
                      quint32 textBase, quint32 textEnd);

    /// Clear the view (no session, or registers not yet valid).
    void clear();


signals:
    /// The user double-clicked a row. For a value that looks like a pointer
    /// this carries the pointed-to address (follow a return address);
    /// otherwise the stack slot's own address (inspect the bytes at SP+n).
    void addressActivated(quint32 address);

private slots:
    void onCellDoubleClicked(int row, int column);
private:
    QTableWidget *m_table = nullptr;
    quint32 m_sp = 0;
};

} // namespace pist
