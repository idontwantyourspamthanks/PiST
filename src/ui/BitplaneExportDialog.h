// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/StFormats.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace pist {

/// One phase of the document, as the export dialog offers it.
struct BitplaneExportPhase {
    QString name;
    int width = 1;
    int height = 1;
    int frames = 1;
};

/// The sprite editor's bitplane export options: which phase goes out, which
/// blocks the `.dat` holds, how finely the sprite is pre-shifted, and whether a
/// scroller comes with it. It also prints the file map the assembler source's
/// `equ`s come from, since a raw blob carries no offsets of its own.
class BitplaneExportDialog : public QDialog
{
    Q_OBJECT

public:
    /// `kind` and `active` are the document's palette, so the transparent
    /// colour can be offered as its swatches; `transparent` is the register to
    /// start on (the document's background).
    BitplaneExportDialog(const QVector<BitplaneExportPhase> &phases, int current,
                         PaletteKind kind, const QVector<int> &active, int transparent,
                         QWidget *parent = nullptr);

    /// The phase to export, indexing the list handed to the constructor.
    int phase() const;
    BitplaneDataOptions options() const;

    /// Whether the export should also write a ready-to-assemble scroller that
    /// shows these blocks moving across the screen.
    bool writesScrollDemo() const;

private:
    void refreshMap();

    QVector<BitplaneExportPhase> m_phases;
    QComboBox *m_phase = nullptr;
    QLabel *m_source = nullptr;
    QCheckBox *m_palette = nullptr;
    QCheckBox *m_sprite = nullptr;
    QCheckBox *m_masked = nullptr;
    QCheckBox *m_shifted = nullptr;
    QCheckBox *m_shiftedMasked = nullptr;
    QCheckBox *m_scrollDemo = nullptr;
    QComboBox *m_preShifts = nullptr;
    QComboBox *m_transparent = nullptr;
    QPlainTextEdit *m_map = nullptr;
    QPushButton *m_ok = nullptr;
};

} // namespace pist
