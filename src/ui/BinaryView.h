// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace pist {

class ImageDocument;

/// A file shown as hex and ASCII. Image mode draws the current range beside it;
/// Audio mode offers the sample; Data mode is the hex alone.
///
/// It is a read-only document tab. Closing it closes the view. It does not
/// participate in Save. Edit image writes a `.pim` in the temp directory and
/// tells the window to open that path. A host file is identified by its path;
/// bytes taken from a disk image have a name and an origin key, and no path.
class BinaryView : public QWidget
{
    Q_OBJECT

public:
    explicit BinaryView(QWidget *parent = nullptr);

    /// Replace the buffer and show it from offset 0. `name` is the tab label.
    void setBytes(const QByteArray &bytes, const QString &name);

    /// The host file, or empty when the bytes came from a disk image.
    void setIdentity(const QString &path, const QString &originKey, const QString &name);
    QString filePath() const;
    QString originKey() const;
    QString displayName() const;

    /// Write the current range as a `.pim` and ask the window to open it.
    void preview();

    /// Write the sample range at the current offset. False, with `*error` set,
    /// when the range or the rate is refused or the write fails. Does not open a dialog.
    bool writeSample(const QString &path, QString *error);

    /// Name an AVR, Ice, Atomik, or SNDH header at the current offset, when one
    /// is there. A bad offset sets the status and returns. No match leaves the
    /// status alone.
    void identifyAtOffset();

    /// Moves the offset field to \a offset and names whatever header sits there.
    void showHit(int offset, const QString &kind);

signals:
    /// A .pim was written at `path`. The window opens it. Emitted only after
    /// files::write succeeded.
    void previewReady(const QString &path);

    /// An IFF hit was chosen. `bytes` start at the magic. The window imports
    /// them and opens the image when they are a real ILBM.
    void iffHitReady(const QByteArray &bytes);

private slots:
    void showOffset();
    void scan();
    void firstData();
    void offsetEdited();
    void refreshPreview();
    void setViewMode(int mode);

private:
    bool imageAtControls(ImageDocument *doc, QString *error) const;
    void applyKind(const QString &kind);

    enum class Focus { Image, Audio, Data };
    Focus m_focus = Focus::Image;

    QByteArray m_bytes;
    QString m_path;
    QString m_key;
    int m_previewSerial = 0;
    QLabel *m_name = nullptr;
    QComboBox *m_mode = nullptr;
    QSpinBox *m_offset = nullptr;
    QWidget *m_audioRow = nullptr;
    QWidget *m_previewColumn = nullptr;
    QLabel *m_image = nullptr;
    QPushButton *m_firstData = nullptr;
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QSpinBox *m_palette = nullptr;
    QPushButton *m_preview = nullptr;
    QSpinBox *m_sampleLength = nullptr;
    QSpinBox *m_rate = nullptr;
    QCheckBox *m_signed = nullptr;
    QPushButton *m_saveWav = nullptr;
    QPushButton *m_scan = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_hex = nullptr;
    QListWidget *m_hits = nullptr;
};

} // namespace pist
