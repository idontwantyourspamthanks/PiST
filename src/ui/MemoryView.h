// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTableWidget;

namespace pist {

/// Hex memory viewer.
///
/// Reads via Hatari's `memdump`. This matters more for assembly than it might
/// for other targets: hardware registers live in the `$ffxxxx` range and are the
/// only way to observe most ST hardware state, so address navigation is the
/// primary interaction rather than an extra.
class MemoryView : public QWidget
{
    Q_OBJECT

public:
    explicit MemoryView(QWidget *parent = nullptr);

    /// The base of the currently-displayed window.
    quint32 currentAddress() const;

public slots:
    /// Move the window and refresh from the emulator.
    void goToAddress(quint32 address);

    /// Re-read the currently-displayed window from the emulator.
    ///
    /// Unlike goToAddress this does not change the base or the address field; it
    /// is what a step needs, because the bytes underneath the view change while
    /// the address stays put.
    void refresh();

    /// Parse a `memdump` response and display it.
    void applyDump(const QString &response);

signals:
    /// The view wants a dump of `address`, `length` bytes.
    void dumpRequested(quint32 address, int length);

private slots:
    void onAddressEntered();
    void onCellDoubleClicked(int row, int column);

private:
    void clear();

    static constexpr int kRowBytes = 16;
    static constexpr int kRows = 16;

    quint32 m_base = 0;
    /// The displayed bytes in memory order, indexed relative to m_base, so a
    /// cell's click can read the pointer at it without reparsing the table.
    QByteArray m_bytes;
    QLineEdit *m_addressEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace pist
