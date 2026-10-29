// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/AnalysisNotes.h"
#include "analysis/Coverage.h"
#include "image/StFormats.h"

#include <QSet>
#include <QWidget>

#include <optional>

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

    /// Mark the instruction that contains this live PC. The bases are the
    /// ones `info basepage` reported; the section sizes are this file's.
    /// The row is painted, and the user's selection is left where it is.
    /// A PC that is not in this image marks nothing.
    void setExecutionPc(quint32 pc, quint32 textBase, quint32 dataBase, quint32 bssBase);
    void clearExecutionPc();
    /// The image address currently marked, or none.
    std::optional<quint32> executionAddress() const;

    /// Note which of this file's bytes the coverage file says have executed.
    /// The samples are absolute PCs; the bases are the live basepage. A row
    /// that contains one is prefixed `ran`. Nothing is written to the notes,
    /// and the selection stays where it is. An empty sample list clears the
    /// notes.
    void setCoverage(const QVector<CoverageSample> &samples, quint32 textBase, quint32 dataBase,
                     quint32 bssBase);

    /// Write Motorola source for this listing. Does not assemble it, and does
    /// not create or change the notes file.
    bool exportAssemblyTo(const QString &path, QString *error);

    /// Write the bytes at `address` as a picture file. `width` and `height`
    /// are the sprite size; the still readings ignore them. `*note` says when
    /// the reading is a guess. Does not open a dialog or the image editor,
    /// and does not create or change the notes file.
    bool openPicture(PictureReading reading, quint32 address, int width, int height, QString *path,
                     QString *note, QString *error);

signals:
    /// A picture file was written at `path`. The window opens it.
    void pictureReady(const QString &path);

private slots:
    void showMenu(const QPoint &pos);
    void renameSelected();
    void commentSelected();
    void markSelectedCode();
    void markSelectedData();
    void clearSelectedMark();
    void exportSelected();
    void openSelectedDegas();
    void openSelectedNeo();
    void openSelectedIff();
    void openSelectedSprite();

private:
    bool selectedSpan(quint32 *address, quint32 *length) const;
    void openSelectedPicture(PictureReading reading, const QString &title, int width, int height);
    bool apply(const AnalysisNotes &next, QString *error);
    void reload();
    void updateActions();
    /// Row colour, the note column, and the gutter: "pc" on the stopped
    /// instruction, "ran" on one coverage has seen finish. Scrolls the stopped
    /// row into view.
    void paintMarks();
    int rowForImage(quint32 address) const;

    QString m_path;
    QString m_name;
    QByteArray m_bytes;
    AnalysisNotes m_notes;
    quint32 m_imageSize = 0;
    quint32 m_endAddress = 0;
    quint32 m_textSize = 0;
    quint32 m_dataSize = 0;
    quint32 m_bssSize = 0;
    QSet<quint32> m_observed;
    bool m_havePc = false;
    quint32 m_pc = 0;
    quint32 m_textBase = 0;
    quint32 m_dataBase = 0;
    quint32 m_bssBase = 0;
    bool m_ready = false;
    QLabel *m_summary = nullptr;
    QTableWidget *m_listing = nullptr;
    QAction *m_rename = nullptr;
    QAction *m_comment = nullptr;
    QAction *m_markCode = nullptr;
    QAction *m_markData = nullptr;
    QAction *m_clearMark = nullptr;
    QAction *m_export = nullptr;
    QAction *m_openDegas = nullptr;
    QAction *m_openNeo = nullptr;
    QAction *m_openIff = nullptr;
    QAction *m_openSprite = nullptr;
    int m_pictureSerial = 0;
};

} // namespace pist
