// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/AnimationPreviewWidget.h"

#include "image/Palette.h"
#include "ui/Appearance.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QSize>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace pist {

namespace {

/// The preview box is square regardless of the sprite's shape.
constexpr int kPreviewSize = 96;

} // namespace

AnimationPreviewWidget::AnimationPreviewWidget(const ImageDocument *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *playRow = new QHBoxLayout;
    m_actPlay = new QAction(tr("Play"), this);
    m_actPlay->setIcon(appearance::icon(appearance::Icon::Run));
    m_actPlay->setCheckable(true);
    m_actPlay->setToolTip(tr("Play animation"));
    connect(m_actPlay, &QAction::toggled, this, &AnimationPreviewWidget::setPlaying);
    auto *playBtn = new QToolButton(this);
    playBtn->setObjectName(QStringLiteral("imagePlay"));
    playBtn->setDefaultAction(m_actPlay);
    playBtn->setAutoRaise(true);
    playBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    playRow->addWidget(playBtn);
    m_fpsBox = new QSpinBox(this);
    m_fpsBox->setObjectName(QStringLiteral("imageFps"));
    m_fpsBox->setRange(1, 60);
    m_fpsBox->setValue(m_fps);
    m_fpsBox->setSuffix(QStringLiteral(" fps"));
    m_fpsBox->setMaximumWidth(88);
    m_fpsBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_fpsBox, qOverload<int>(&QSpinBox::valueChanged), this,
            &AnimationPreviewWidget::fpsChanged);
    playRow->addWidget(m_fpsBox);
    // Play and fps pack to the left instead of stretching across the column.
    playRow->setSpacing(4);
    playRow->setContentsMargins(0, 0, 0, 0);
    playRow->addStretch(1);
    layout->addLayout(playRow);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("imagePreview"));
    m_preview->setFixedSize(kPreviewSize, kPreviewSize);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setScaledContents(false);
    m_preview->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_preview);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AnimationPreviewWidget::tick);
}

void AnimationPreviewWidget::setPlaying(bool on)
{
    m_playing = on;
    if (m_playing) {
        m_frame = 0;
        m_timer->start(qMax(1, 1000 / qMax(1, m_fps)));
        m_actPlay->setIcon(appearance::icon(appearance::Icon::Pause));
        m_actPlay->setToolTip(tr("Pause animation"));
        refresh();
    } else {
        m_timer->stop();
        m_frame = m_doc->currentFrame();
        m_actPlay->setIcon(appearance::icon(appearance::Icon::Run));
        m_actPlay->setToolTip(tr("Play animation"));
        refresh();
    }
}

void AnimationPreviewWidget::tick()
{
    m_frame = nextPreviewFrame(m_frame, m_doc->frameCount(), 0, m_doc->frameCount() - 1);
    refresh();
}

void AnimationPreviewWidget::fpsChanged(int fps)
{
    m_fps = qBound(1, fps, 60);
    if (m_playing)
        m_timer->start(qMax(1, 1000 / m_fps));
}

void AnimationPreviewWidget::refresh()
{
    if (!m_preview)
        return;
    if (!m_playing)
        m_frame = m_doc->currentFrame();
    m_frame = qBound(0, m_frame, m_doc->frameCount() - 1);
    const QSize box(kPreviewSize, kPreviewSize);
    m_preview->setPixmap(QPixmap::fromImage(
        indicesToImage(m_doc->frame(m_frame), m_doc->width(), m_doc->height(),
                       m_doc->paletteKind(), EmptyStyle::Checkerboard)
            .scaled(box, Qt::KeepAspectRatio, Qt::FastTransformation)));
}

void AnimationPreviewWidget::setFrame(int frame)
{
    if (!m_playing)
        m_frame = frame;
}

void AnimationPreviewWidget::stop()
{
    m_actPlay->setChecked(false);
}

void AnimationPreviewWidget::applyAppearance()
{
    m_actPlay->setIcon(appearance::icon(m_playing ? appearance::Icon::Pause
                                                  : appearance::Icon::Run));
}

} // namespace pist
