// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/AnalysisNotes.h"

#include <QWidget>

class QAction;
class QLabel;
class QTableWidget;

namespace pist {

/// Listing of a TOS executable. The walk's rows are fixed text. Names, comments
/// and code/data marks are edited here and stored beside the program.
class AnalysisView : public QWidget
{
    Q_OBJECT

public:
    explicit AnalysisView(QWidget *parent = nullptr);

    void setIdentity(const QString &path, const QString &name);
    QString filePath() const;
    QString displayName() const;

    /// Replace the listing. Notes are read from the sidecar of `filePath`
    /// when that path is set. A file that is not a TOS executable, or notes
    /// that cannot be read, leaves the table empty and puts the reason in
    /// the summary.
    void setBytes(const QByteArray &bytes);

    /// Persist one change and rebuild the listing. `name` or `text` empty
    /// removes that note. False leaves the previous notes in place.
    bool renameAddress(quint32 address, const QString &name, QString *error);
    bool setAddressComment(quint32 address, const QString &text, QString *error);
    bool markBytes(quint32 address, quint32 length, AnalysisOverride::Kind kind, QString *error);
    bool clearMark(quint32 address, quint32 length, QString *error);

private slots:
    void showMenu(const QPoint &pos);
    void renameSelected();
    void commentSelected();
    void markSelectedCode();
    void markSelectedData();
    void clearSelectedMark();

private:
    bool selectedSpan(quint32 *address, quint32 *length) const;
    bool apply(const AnalysisNotes &next, QString *error);
    void reload();
    void updateActions();

    QString m_path;
    QString m_name;
    QByteArray m_bytes;
    AnalysisNotes m_notes;
    quint32 m_imageSize = 0;
    quint32 m_endAddress = 0;
    bool m_ready = false;
    QLabel *m_summary = nullptr;
    QTableWidget *m_listing = nullptr;
    QAction *m_rename = nullptr;
    QAction *m_comment = nullptr;
    QAction *m_markCode = nullptr;
    QAction *m_markData = nullptr;
    QAction *m_clearMark = nullptr;
};

} // namespace pist
