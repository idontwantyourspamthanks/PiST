// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace pist {

/// A file shown as hex, with a preview of one range as a sprite document.
///
/// It is a dock, not a document tab: closing it does not close a file, and it
/// does not participate in Save. Preview writes a `.pim` in the temp directory
/// and tells the window to open that path.
class BinaryView : public QWidget
{
    Q_OBJECT

public:
    explicit BinaryView(QWidget *parent = nullptr);

    /// Replace the buffer and show it from offset 0. `name` is a label only.
    void setBytes(const QByteArray &bytes, const QString &name);

    /// The preview slot the button and the test call.
    void preview();

signals:
    /// A .pim was written at `path`. The window opens it. Emitted only after
    /// files::write succeeded.
    void previewReady(const QString &path);

private slots:
    void showOffset();

private:
    bool parseHex(const QString &text, quint32 *value) const;

    QByteArray m_bytes;
    int m_previewSerial = 0;
    QLabel *m_name = nullptr;
    QLineEdit *m_offset = nullptr;
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QLineEdit *m_palette = nullptr;
    QPushButton *m_preview = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_hex = nullptr;
};

} // namespace pist
