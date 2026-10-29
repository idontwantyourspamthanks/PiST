// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QAudioSink;
class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QListWidget;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace pist {

class ImageDocument;

/// The sample in Audio mode: one vertical bar per column, and a playhead the
/// pointer can drag. Playback is owned by the view, not by this widget.
class BinaryWaveform : public QWidget
{
    Q_OBJECT

public:
    explicit BinaryWaveform(QWidget *parent = nullptr);

    void setSamples(const QByteArray &samples, bool signedSamples);
    void setPlayhead(int index);
    int playhead() const;
    int sampleCount() const;

signals:
    /// The pointer moved the playhead. `scrubFinished` follows on release.
    void scrubbed(int index);
    void scrubFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    int indexAt(int x) const;
    int amplitude(int index) const;
    void scrubAt(int x);

    QByteArray m_samples;
    bool m_signed = false;
    int m_playhead = 0;
};

/// A file shown as hex and ASCII. Image mode draws the current range beside it;
/// Audio mode plays the sample and draws its waveform; Data mode is the hex alone.
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
    ~BinaryView() override;

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

protected:
    void hideEvent(QHideEvent *event) override;

private slots:
    void showOffset();
    void scan();
    void firstData();
    void offsetEdited();
    void refreshPreview();
    void refreshWaveform();
    void setViewMode(int mode);

private:
    bool imageAtControls(ImageDocument *doc, QString *error) const;
    bool sampleBytes(QByteArray *out, QString *error) const;
    void applyKind(const QString &kind);
    void startPlayback();
    void stopPlayback();
    void finishPlayback();
    void advancePlayhead();
    void updatePlayTime();

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
    QPushButton *m_play = nullptr;
    QLabel *m_playTime = nullptr;
    BinaryWaveform *m_wave = nullptr;
    QPushButton *m_saveWav = nullptr;
    QAudioSink *m_sink = nullptr;
    QTimer *m_playTimer = nullptr;
    qint64 m_playStartMs = 0;
    int m_playFrom = 0;
    bool m_resumeAfterScrub = false;
    QPushButton *m_scan = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_hex = nullptr;
    QListWidget *m_hits = nullptr;
};

} // namespace pist
