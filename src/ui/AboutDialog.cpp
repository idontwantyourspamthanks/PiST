// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/AboutDialog.h"

#include "ui/Appearance.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

namespace pist {

namespace {

const QColor kGemFace(0xe0, 0xe0, 0xe0);
const QColor kGemInk(Qt::black);

QFont gemFont(int pixelSize, bool bold = false)
{
    QFont f(QStringLiteral("sans-serif"));
    f.setPixelSize(pixelSize);
    f.setBold(bold);
    f.setStyleStrategy(QFont::NoAntialias);
    return f;
}

QLabel *line(QWidget *parent, const QString &text, const QString &objectName, int pixelSize,
             bool bold = false)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    label->setFont(gemFont(pixelSize, bold));
    label->setStyleSheet(QStringLiteral("color: #000000; background: transparent;"));
    return label;
}

class DottedRule : public QWidget
{
public:
    explicit DottedRule(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("aboutRule"));
        setFixedHeight(12);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        const int y = height() / 2;
        for (int x = 2; x < width() - 2; x += 6)
            p.drawRect(x, y, 2, 1);
    }
};

} // namespace

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(tr("About PiST"));
    setModal(true);
    setFixedSize(420, 300);
    setWindowFlags((windowFlags() & ~Qt::WindowContextHelpButtonHint) | Qt::FramelessWindowHint);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, kGemFace);
    pal.setColor(QPalette::WindowText, kGemInk);
    pal.setColor(QPalette::Button, kGemFace);
    pal.setColor(QPalette::ButtonText, kGemInk);
    setPalette(pal);
    setAutoFillBackground(false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(6);

    root->addWidget(line(this, tr("PIST, Program in ST"), QStringLiteral("aboutTitle"), 15, true));
    root->addWidget(line(this, QStringLiteral(PIST_VERSION), QStringLiteral("aboutVersion"), 13));
    root->addWidget(new DottedRule(this));

    auto *logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("aboutLogo"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setPixmap(appearance::atariLogoPixmap(88, true));
    root->addWidget(logo, 0, Qt::AlignHCenter);

    root->addSpacing(4);
    root->addWidget(line(this, tr("Copyright 2026"), QStringLiteral("aboutCopyright"), 13));
    root->addWidget(
        line(this, QStringLiteral("Koala Software/Dad.PRG"), QStringLiteral("aboutCredit"), 13));
    root->addStretch(1);

    auto *ok = new QPushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("aboutOkButton"));
    ok->setDefault(true);
    ok->setFixedSize(72, 24);
    ok->setCursor(Qt::PointingHandCursor);
    ok->setFont(gemFont(13, true));
    ok->setStyleSheet(QStringLiteral(
        "QPushButton { background: #e8e8e8; color: #000000; border: 2px solid #000000; }"
        "QPushButton:default { border: 3px solid #000000; }"
        "QPushButton:pressed { background: #c0c0c0; }"));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);

    auto *okRow = new QHBoxLayout;
    okRow->addStretch(1);
    okRow->addWidget(ok);
    okRow->addStretch(1);
    root->addLayout(okRow);
}

void AboutDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), kGemFace);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kGemInk, 2));
    p.drawRect(rect().adjusted(1, 1, -2, -2));
    p.setPen(QPen(kGemInk, 1));
    p.drawRect(rect().adjusted(5, 5, -6, -6));
}

} // namespace pist
