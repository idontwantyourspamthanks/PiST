// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/ProgramDiff.h"

#include <QDialog>

class QLabel;
class QTableWidget;

namespace pist {

/// Preview of what a rebuild changed. There is no way to apply it: writing
/// those bytes into a running program is a later, separate decision, and this
/// dialog does not offer it.
class ProgramDiffDialog : public QDialog
{
    Q_OBJECT

public:
    ProgramDiffDialog(const ProgramDiff &diff, const ProgramDiffHints &hints, QWidget *parent = nullptr);

private:
    QLabel *m_summary = nullptr;
    QLabel *m_footer = nullptr;
    QTableWidget *m_table = nullptr;
};

} // namespace pist
