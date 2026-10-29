// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MemoryDump.h"

#include <QByteArray>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTableWidget;
class QTableWidgetItem;
class QToolButton;

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

    /// Display a parsed `memdump` reply. The rows arrive already parsed (the
    /// backend that read the text parsed it: MAJ-45), so nothing here reads a
    /// Hatari transcript.
    void applyDump(const QList<MemoryRow> &rows);

    /// Whether byte cells can be edited. Only meaningful when the machine is
    /// stopped — writes go through the debugger.
    void setEditingEnabled(bool enabled);

    /// The stop this window was last refreshed for. A mark records it, so the
    /// comparison can say where the earlier bytes came from. `where` is a
    /// source location (`file.s:12`) when the line map has one, otherwise empty.
    void setStopContext(quint32 pc, const QString &where);

    /// Text, data and bss as half-open live ranges. A zero end means that
    /// section is unknown, and then nothing is tinted: an inferred screen or
    /// asset region is not a section the basepage named.
    void setProgramRegions(quint32 textBase, quint32 textEnd, quint32 dataBase, quint32 dataEnd,
                           quint32 bssBase, quint32 bssEnd);

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
    void setMark();
    void onCompareToggled(bool on);
    void onPreviousToggled(bool on);

private:
    void clear();
    /// Paint `rows`. `advancePrevious` is true only for a dump the emulator
    /// just returned: that is the moment "compare with previous stop" moves
    /// its mark. A repaint (appearance, a region tint, toggling the mark)
    /// must not, or the highlight of the stop just shown would vanish.
    void paintDump(const QList<MemoryRow> &rows, bool advancePrevious);

    struct Mark {
        bool valid = false;
        quint32 base = 0;
        QByteArray bytes;
        quint32 pc = 0;
        QString where;
        bool hasContext = false;
    };

    Mark captureMark() const;
    void writeComparison(const Mark &compared);
    QString comparisonStatus(const Mark &compared) const;
    /// `text`, `data`, `bss`, `outside` or empty when no section is known.
    QString regionName(quint32 address) const;

    static constexpr int kRowBytes = 16;
    static constexpr int kRows = 16;

    /// Whether byte cells accept edits (gated on the machine being stopped).
    bool m_editingEnabled = false;

    quint32 m_base = 0;
    /// After a user edit, dumps that still show the old byte were requested
    /// before the write landed (onStateUpdated refreshes the pane on every
    /// register dump). Discard them so they cannot paint over the new value.
    bool m_pendingEdit = false;
    quint32 m_pendingAddress = 0;
    quint8 m_pendingValue = 0;
    int m_staleDumps = 0;
    static constexpr int kMaxStaleDumps = 12;
    /// The last dump as parsed, so an appearance change can re-render it
    /// without going back to the emulator.
    QList<MemoryRow> m_lastRows;
    /// The displayed bytes in memory order, indexed relative to m_base, so a
    /// cell's click can read the pointer at it without reparsing the table.
    QByteArray m_bytes;
    QLineEdit *m_addressEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;

    QToolButton *m_compareButton = nullptr;
    QToolButton *m_previousButton = nullptr;

    Mark m_mark;
    /// The mark a previous-stop comparison was painted against. The stored
    /// mark has already moved on to the dump just shown, and a later repaint
    /// has to keep highlighting against this one.
    Mark m_shownAgainst;
    bool m_haveShown = false;
    bool m_compare = false;
    bool m_previousStop = false;

    quint32 m_stopPc = 0;
    QString m_stopWhere;
    bool m_haveStopContext = false;

    quint32 m_textBase = 0;
    quint32 m_textEnd = 0;
    quint32 m_dataBase = 0;
    quint32 m_dataEnd = 0;
    quint32 m_bssBase = 0;
    quint32 m_bssEnd = 0;
};

} // namespace pist
