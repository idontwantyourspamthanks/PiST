// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;

namespace pist {

/// Run ▸ Stop hooks… — which script runs at every debugger stop, and whether it
/// may continue on its own (docs/stop-hooks-plan.md §9).
///
/// A plain value dialog: the caller reads the three answers back on accept() and
/// persists them in the project file. Nothing here knows what a stop hook is,
/// which is what keeps it testable with no session at all.
class StopHooksDialog : public QDialog
{
    Q_OBJECT

public:
    /// `scriptPath`, `enabled` and `autoContinue` are the project's current
    /// answers. `lastRun` is how the hook ended the last time it ran in this
    /// session (`StopHookRunner::lastRunSummary()`), or empty when it has not —
    /// shown read-only, because "did my script fire?" is otherwise unanswerable
    /// until it does something noisy.
    StopHooksDialog(const QString &scriptPath, bool enabled, bool autoContinue,
                    const QString &lastRun, QWidget *parent = nullptr);

    /// The script path, as typed (or empty).
    QString scriptPath() const;
    /// Whether the hook runs at all.
    bool enabled() const;
    /// Whether the script's `continue()` is honoured without asking.
    bool autoContinue() const;

protected:
    /// Refuses to close with the hook enabled and no readable script: a hook
    /// that cannot load would only report itself at a stop, which is the worst
    /// moment to learn it.
    void accept() override;

private:
    void browse();

    QCheckBox *m_enabled = nullptr;
    QLineEdit *m_path = nullptr;
    QCheckBox *m_autoContinue = nullptr;
};

} // namespace pist
