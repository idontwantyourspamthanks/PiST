// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

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

public slots:
    /// Move the window and refresh from the emulator.
    void goToAddress(quint32 address);

    /// Parse a `memdump` response and display it.
    void applyDump(const QString &response);

signals:
    /// The view wants a dump of `address`, `length` bytes.
    void dumpRequested(quint32 address, int length);

private slots:
    void onAddressEntered();

private:
    void clear();

    static constexpr int kRowBytes = 16;
    static constexpr int kRows = 16;

    quint32 m_base = 0;
    QLineEdit *m_addressEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace pist
