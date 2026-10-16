// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "ui/NewFileSpec.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace pist {

/// File ▸ New File: a name, a directory and a template — the whole create-file
/// transaction, and nothing else (docs/untitled-plan.md §4).
///
/// Shaped like `NewImageDialog`, which asks the same two questions for a
/// sprite. What the fields mean is `NewFileSpec`'s business; this class is the
/// shell that shows the verdict as it is typed, so a name that cannot be used
/// is refused before Accept rather than after (which is what the sidebar's
/// context menu used to do, with one flat "the name may be invalid" for every
/// cause).
class NewFileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewFileDialog(QWidget *parent = nullptr, const QString &directory = QString());

    /// The transaction the fields describe. The dialog's own Accept is gated on
    /// `spec().verify()`, and the caller creates the file through the same
    /// spec, so the two cannot disagree about the path or the bytes.
    NewFileSpec spec() const;

    /// The absolute path the file will be created at, or empty while the
    /// fields do not yet name one.
    QString filePath() const;

private:
    void browse();
    void updateState();

    QLineEdit *m_name = nullptr;
    QLineEdit *m_directory = nullptr;
    QComboBox *m_template = nullptr;
    QLabel *m_message = nullptr;
    QLabel *m_note = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace pist
