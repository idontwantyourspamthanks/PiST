// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/PalettePickerDialog.h"

#include "image/Palette.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <functional>

namespace pist {

class CubeWidget : public QWidget
{
public:
    CubeWidget(PaletteKind kind, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_kind(kind)
    {
        setMinimumSize(256, kind == PaletteKind::Ste ? 256 : 128);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        rebuild();
    }

    void setKind(PaletteKind kind)
    {
        m_kind = kind;
        rebuild();
        update();
    }

    void setActive(const QVector<int> &active)
    {
        m_active = active;
        update();
    }

    int lastHit() const { return m_last; }

    std::function<void(int)> clicked;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (m_image.isNull())
            return;
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(rect(), m_image);
        const int n = channelCount(m_kind);
        if (n <= 0)
            return;
        p.setPen(QPen(Qt::white, 1));
        for (int idx : m_active) {
            if (idx < 0 || idx >= cubeSize(m_kind))
                continue;
            const int r = idx / (n * n);
            const int g = (idx / n) % n;
            const int b = idx % n;
            const int ix = g * n + b;
            const QRect cell(ix * width() / m_image.width(),
                             r * height() / m_image.height(),
                             qMax(1, width() / m_image.width()),
                             qMax(1, height() / m_image.height()));
            p.drawRect(cell.adjusted(0, 0, -1, -1));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_image.isNull())
            return;
        const int x = event->pos().x() * m_image.width() / qMax(1, width());
        const int y = event->pos().y() * m_image.height() / qMax(1, height());
        if (x < 0 || y < 0 || x >= m_image.width() || y >= m_image.height())
            return;
        const int n = channelCount(m_kind);
        // Image is n*n wide (g,b) and n tall (r).
        const int r = y;
        const int g = x / n;
        const int b = x % n;
        m_last = r * n * n + g * n + b;
        if (clicked)
            clicked(m_last);
    }

private:
    void rebuild()
    {
        const int n = channelCount(m_kind);
        m_image = QImage(n * n, n, QImage::Format_RGB32);
        for (int r = 0; r < n; ++r) {
            for (int g = 0; g < n; ++g) {
                for (int b = 0; b < n; ++b) {
                    const int idx = r * n * n + g * n + b;
                    const Rgb rgb = cubeRgb(m_kind, idx);
                    m_image.setPixel(g * n + b, r, qRgb(rgb.r, rgb.g, rgb.b));
                }
            }
        }
    }

    PaletteKind m_kind;
    QVector<int> m_active;
    QImage m_image;
    int m_last = 0;
};

PalettePickerDialog::PalettePickerDialog(PaletteKind kind, const QVector<int> &active,
                                         QWidget *parent)
    : QDialog(parent)
    , m_kind(kind)
    , m_active(active)
{
    setWindowTitle(tr("Active colours"));
    auto *layout = new QVBoxLayout(this);

    m_count = new QLabel(this);
    layout->addWidget(m_count);

    m_cube = new CubeWidget(kind, this);
    m_cube->clicked = [this](int index) {
        const int pos = m_active.indexOf(index);
        if (pos >= 0) {
            if (m_active.size() > 1)
                m_active.removeAt(pos);
        } else if (m_active.size() < kMaxActive) {
            m_active.append(index);
        }
        updateMixerFromColour(index);
        refreshCount();
    };
    layout->addWidget(m_cube, 1);

    auto *mixer = new QHBoxLayout;
    auto addSlider = [&](const QString &name) {
        auto *box = new QVBoxLayout;
        box->addWidget(new QLabel(name, this));
        auto *slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(0, 255);
        box->addWidget(slider);
        mixer->addLayout(box);
        return slider;
    };
    m_r = addSlider(tr("R"));
    m_g = addSlider(tr("G"));
    m_b = addSlider(tr("B"));
    layout->addLayout(mixer);

    m_preview = new QLabel(this);
    m_preview->setMinimumHeight(24);
    m_preview->setAutoFillBackground(true);
    layout->addWidget(m_preview);

    auto *add = new QPushButton(tr("Add mixed colour"), this);
    connect(add, &QPushButton::clicked, this, &PalettePickerDialog::addMixed);
    layout->addWidget(add);

    auto snap = [this] {
        m_r->blockSignals(true);
        m_g->blockSignals(true);
        m_b->blockSignals(true);
        m_r->setValue(snapChannel(m_kind, m_r->value()));
        m_g->setValue(snapChannel(m_kind, m_g->value()));
        m_b->setValue(snapChannel(m_kind, m_b->value()));
        m_r->blockSignals(false);
        m_g->blockSignals(false);
        m_b->blockSignals(false);
        const QColor c(m_r->value(), m_g->value(), m_b->value());
        QPalette pal = m_preview->palette();
        pal.setColor(QPalette::Window, c);
        m_preview->setPalette(pal);
    };
    connect(m_r, &QSlider::valueChanged, this, snap);
    connect(m_g, &QSlider::valueChanged, this, snap);
    connect(m_b, &QSlider::valueChanged, this, snap);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    refreshCount();
    if (!m_active.isEmpty())
        updateMixerFromColour(m_active.first());
    resize(480, 560);
}

void PalettePickerDialog::updateMixerFromColour(int cubeIndex)
{
    const Rgb rgb = cubeRgb(m_kind, cubeIndex);
    m_r->setValue(rgb.r);
    m_g->setValue(rgb.g);
    m_b->setValue(rgb.b);
}

void PalettePickerDialog::addMixed()
{
    const Rgb rgb{quint8(m_r->value()), quint8(m_g->value()), quint8(m_b->value())};
    const int index = nearestCubeIndex(m_kind, rgb);
    if (m_active.contains(index))
        return;
    if (m_active.size() >= kMaxActive)
        return;
    m_active.append(index);
    refreshCount();
}

void PalettePickerDialog::refreshCount()
{
    m_count->setText(tr("%1 / %2 active").arg(m_active.size()).arg(kMaxActive));
    if (m_cube)
        m_cube->setActive(m_active);
}

} // namespace pist
