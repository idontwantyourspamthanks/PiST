// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"

#include <QWidget>

class QAction;
class QLabel;
class QSpinBox;
class QTimer;

namespace pist {

/// The sprite editor's animation player: a fixed preview box of a frame, a
/// frames-per-second box, and a play/pause action that steps the preview
/// through the document's frames on a timer. Playback is a preview only — it
/// never moves the document's own current frame.
///
/// The document is held as a pointer to the editor's document member, which the
/// editor reassigns in place, so it stays valid across loads and new documents.
class AnimationPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AnimationPreviewWidget(const ImageDocument *doc, QWidget *parent = nullptr);

    /// The play/pause action, so the editor can stop playback with the document
    /// it described (a new document drops the old playback).
    QAction *playAction() const { return m_actPlay; }
    /// Repaint the shown frame — the document, or the preview frame, changed.
    void refresh();
    /// Follow the document's frame selection; ignored while playing.
    void setFrame(int frame);
    /// Stop playback (the play action unchecks) and show the document's current
    /// frame again.
    void stop();
    /// Rebuild the play/pause icon after a theme change.
    void applyAppearance();

private:
    void setPlaying(bool on);
    void tick();
    void fpsChanged(int fps);

    const ImageDocument *m_doc = nullptr;
    QLabel *m_preview = nullptr;
    QSpinBox *m_fpsBox = nullptr;
    QAction *m_actPlay = nullptr;
    QTimer *m_timer = nullptr;
    /// The frame shown; the document's current frame between plays.
    int m_frame = 0;
    int m_fps = 8;
    bool m_playing = false;
};

} // namespace pist
