// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QFrame>

class QLabel;
class QVBoxLayout;

namespace pist {

/// One tile in a chooser: a title, a sentence, an optional monospace preview,
/// and whatever else the caller drops into body(). Click or Enter selects it.
/// The dialog owns the meaning; this only draws the tile and reports the click.
class GalleryCard : public QFrame
{
    Q_OBJECT

public:
    explicit GalleryCard(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setDetail(const QString &detail);
    /// A few lines of monospaced text under the sentence. Empty hides the line.
    void setPreview(const QString &text);
    QVBoxLayout *body() const { return m_body; }

    void setSelected(bool selected);
    bool isSelected() const { return m_selected; }

signals:
    void activated();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QVBoxLayout *m_body = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_preview = nullptr;
    bool m_selected = false;
    bool m_hover = false;
};

} // namespace pist
