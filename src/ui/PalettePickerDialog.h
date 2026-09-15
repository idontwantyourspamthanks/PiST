// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"

#include <QDialog>
#include <QVector>

class QSlider;
class QLabel;

namespace pist {

/// Pick up to 16 colours from the STfm/STe cube, with an RGB mixer that snaps
/// to the hardware channel levels.
class PalettePickerDialog : public QDialog
{
    Q_OBJECT

public:
    PalettePickerDialog(PaletteKind kind, const QVector<int> &active, QWidget *parent = nullptr);

    QVector<int> active() const { return m_active; }

private:
    void updateMixerFromColour(int cubeIndex);
    void addMixed();
    void refreshCount();

    PaletteKind m_kind = PaletteKind::Ste;
    QVector<int> m_active;
    QLabel *m_count = nullptr;
    QLabel *m_preview = nullptr;
    QSlider *m_r = nullptr;
    QSlider *m_g = nullptr;
    QSlider *m_b = nullptr;
    class CubeWidget *m_cube = nullptr;
};

} // namespace pist
