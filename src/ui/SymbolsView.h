// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/SymbolTable.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace pist {

class ProgramLineMap;

/// Browser of the program's symbols, fed from vasm's own listing rather than
/// from the emulator's debugger.
///
/// The listing is available as soon as the assembler has run, while a debugger
/// symbol table is not loaded until the program has started and stopped — so
/// this is the only way to browse names before a session. Addresses are a
/// different matter: they are unknown until the program is relocated at load
/// time, so a symbol's address resolves only once the program map has live
/// section bases. Until then the column stays empty rather than showing an
/// offset, which would look like an address and be wrong by the load address.
///
/// The address is the one on the definition's own line, so a label written
/// beside its first instruction or data item shows an address while one written
/// on a line by itself does not: vasm emits no bytes on that line, and the
/// address of the code that follows is the next line's, which the map reports
/// under that line. The alternative — scanning forward for the next emitted
/// byte — would be right for such a label but silently wrong for an `equ`,
/// whose value is an expression (a data offset, a bit mask) that no source
/// line's address equals. A blank is honest; a wrong address is not.
///
/// Filtering is by name, case-insensitively: on a large listing the names are
/// what the developer has in mind ("which routine was it?"), not the line.
class SymbolsView : public QWidget
{
    Q_OBJECT

public:
    explicit SymbolsView(QWidget *parent = nullptr);

    /// Replace the list and resolve addresses against `map`.
    ///
    /// `map` may be null or unresolved (no session yet), in which case every
    /// address is blank. Call again with the same symbols once the program map
    /// has been resolved and the addresses fill in.
    void setSymbols(const QVector<SymbolEntry> &symbols, const ProgramLineMap *map);

    /// Re-apply the theme colours to the current rows.
    void applyAppearance();

signals:
    /// A symbol was activated (double-clicked or Enter). Only emitted for a
    /// symbol with a source position, since there is nowhere else to go.
    void symbolActivated(const QString &file, int line);

private:
    void onFilterChanged(const QString &text);
    void onItemActivated(QTreeWidgetItem *item);
    void updateCount();

    static constexpr int kColName = 0;
    static constexpr int kColAddress = 1;
    static constexpr int kColLocation = 2;

    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_count = nullptr;

    /// Rows passing the current filter, for the count label. The total comes
    /// from the tree itself, so it cannot disagree with what is on screen.
    int m_shown = 0;
};

} // namespace pist
