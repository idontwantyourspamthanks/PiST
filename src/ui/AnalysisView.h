// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QLabel;
class QTableWidget;

namespace pist {

/// Read-only listing of a TOS executable. Addresses are image-relative, and the
/// rows are only the instructions a walk from the entry point can reach.
class AnalysisView : public QWidget
{
    Q_OBJECT

public:
    explicit AnalysisView(QWidget *parent = nullptr);

    void setIdentity(const QString &path, const QString &name);
    QString filePath() const;
    QString displayName() const;

    /// Replace the listing. A file that is not a TOS executable leaves the
    /// table empty and puts the reason in the summary.
    void setBytes(const QByteArray &bytes);

private:
    QString m_path;
    QString m_name;
    QLabel *m_summary = nullptr;
    QTableWidget *m_listing = nullptr;
};

} // namespace pist
