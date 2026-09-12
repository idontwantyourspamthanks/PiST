// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QPlainTextEdit;

namespace pist {

/// Shows the ST's hardware state, as reported by Hatari's `info` debugger
/// commands: the shifter/video, the MFP, the ACIA/IKBD, the sound chip, the
/// blitter, and so on. Most of that state cannot be read from a plain memory
/// dump — the I/O registers either have read side effects or only make sense
/// interpreted — so the emulator's own view of it is the useful one.
///
/// Read-only; refreshed from the emulator each time the machine stops.
class HardwareView : public QWidget
{
    Q_OBJECT

public:
    explicit HardwareView(QWidget *parent = nullptr);

    /// The currently selected info subject (e.g. "video").
    QString subject() const;

public slots:
    /// Fill the view with the response to an `info <subject>` command.
    void setInfo(const QString &text);

    /// Clear the view (no session).
    void clear();

signals:
    /// The user picked a different subject; the host should re-request it.
    void subjectChanged(const QString &subject);

private:
    QComboBox *m_subject = nullptr;
    QPlainTextEdit *m_text = nullptr;
};

} // namespace pist
