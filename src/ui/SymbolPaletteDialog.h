// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "editor/CompletionModel.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace pist {

/// The symbol palette (Ctrl+Shift+O): a filter-as-you-type jump list over the
/// project's labels and equates (docs/glm-features.md §4).
///
/// The candidates are the same ones completion filters — the window builds them
/// from the build's symbols — so this is the second presentation of one model, not
/// a second list. Deliberately not modal: a jump list is a gesture, and opening the
/// file is the window's business, so the palette reports the definition and closes.
class SymbolPaletteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SymbolPaletteDialog(QWidget *parent = nullptr);

    /// The candidates to jump to. Only symbols with a definition are listed — a
    /// command-line define has no source position, and a jump list that cannot
    /// jump is noise.
    void setSymbols(const QList<CompletionItem> &candidates);

    /// Put the caret in the filter, ready to type.
    void focusFilter();

signals:
    /// A symbol was chosen: where it is defined. The window opens it.
    void symbolActivated(const QString &file, int line);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void activateCurrent();

    CompletionModel m_model;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_empty = nullptr;
};

} // namespace pist
