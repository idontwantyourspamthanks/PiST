// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTableWidget;
class QTableWidgetItem;

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

    /// Whether byte cells can be edited. Only meaningful when the machine is
    /// stopped — writes go through the debugger.
    void setEditingEnabled(bool enabled);

    /// Re-apply the theme font and the last dump's colours.
    void applyAppearance();

signals:
    /// The view wants a dump of `address`, `length` bytes.
    void dumpRequested(quint32 address, int length);

    /// The user committed a new byte value at `address` (a single byte).
    void memoryEdited(quint32 address, quint32 value);

    /// The user asked for another memory pane (the "+" button).
    void addPaneRequested();

private slots:
    void onAddressEntered();
    void onCellDoubleClicked(int row, int column);
    void onByteEdited(QTableWidgetItem *item);

private:
    void clear();

    static constexpr int kRowBytes = 16;
    static constexpr int kRows = 16;

    /// Whether byte cells accept edits (gated on the machine being stopped).
    bool m_editingEnabled = false;

    quint32 m_base = 0;
    QString m_lastDump;
    /// The displayed bytes in memory order, indexed relative to m_base, so a
    /// cell's click can read the pointer at it without reparsing the table.
    QByteArray m_bytes;
    QLineEdit *m_addressEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace pist
