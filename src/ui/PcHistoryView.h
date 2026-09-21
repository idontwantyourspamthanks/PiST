// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QWidget>

class QPlainTextEdit;

namespace pist {

/// Shows the recent execution path: the last N program counters, as reported by
/// Hatari's `history` command (enabled at session start with `history cpu`).
/// For an assembly developer this answers "how did I get here" — the path of
/// instructions that led to the current stop.
///
/// Read-only; refreshed from the emulator each time the machine stops.
class PcHistoryView : public QWidget
{
    Q_OBJECT

public:
    explicit PcHistoryView(QWidget *parent = nullptr);

public slots:
    /// Fill the view with the response to a `history <count>` command.
    void setHistory(const QString &text);

    /// Clear the view (no session).
    void clear();

signals:
    /// A line was double-clicked. The host opens the source line when the
    /// program map knows it, and otherwise opens that address in memory.
    void addressActivated(quint32 address);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPlainTextEdit *m_text = nullptr;
};

} // namespace pist
