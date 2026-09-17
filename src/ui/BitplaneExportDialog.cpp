// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/BitplaneExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace pist {

namespace {

/// A row of the printed map: either one block, or a run of same-sized blocks of
/// one kind collapsed to `first..last` — the pre-shifted copies and, past the
/// first frame, nothing else, since frames are listed by their stride instead.
struct MapRow {
    QString name;
    int offset = 0;
    int bytes = 0;
    int copies = 0;
};

QVector<MapRow> mapRows(const QVector<BitplaneBlock> &blocks)
{
    QVector<MapRow> rows;
    for (int i = 0; i < blocks.size();) {
        int end = i + 1;
        while (end < blocks.size() && blocks.at(end).kind == blocks.at(i).kind
               && blocks.at(end).frame == blocks.at(i).frame
               && blocks.at(end).bytes == blocks.at(i).bytes)
            ++end;
        MapRow row;
        row.name = blocks.at(i).name;
        row.offset = blocks.at(i).offset;
        row.bytes = blocks.at(i).bytes;
        row.copies = end - i;
        if (row.copies > 1) {
            // "sprite_shift0_f0" and seven more become "sprite_shift0..7_f0":
            // the range goes before the frame suffix the layout adds.
            const int suffix = row.name.lastIndexOf(QStringLiteral("_f"));
            const QString range = QStringLiteral("..%1").arg(row.copies - 1);
            row.name = suffix > 0 ? row.name.left(suffix) + range + row.name.mid(suffix)
                                  : row.name + range;
        }
        rows.append(row);
        i = end;
    }
    return rows;
}

} // namespace

BitplaneExportDialog::BitplaneExportDialog(const QVector<BitplaneExportPhase> &phases,
                                           int current, QWidget *parent)
    : QDialog(parent)
    , m_phases(phases)
{
    setWindowTitle(tr("Export bitplane data"));

    auto *layout = new QVBoxLayout(this);
    auto *top = new QFormLayout;
    m_phase = new QComboBox(this);
    m_phase->setObjectName(QStringLiteral("bitplanePhase"));
    for (int i = 0; i < phases.size(); ++i) {
        m_phase->addItem(tr("%1 — %2×%3, %4 frame%5")
                             .arg(phases.at(i).name)
                             .arg(phases.at(i).width)
                             .arg(phases.at(i).height)
                             .arg(phases.at(i).frames)
                             .arg(phases.at(i).frames == 1 ? QString() : QStringLiteral("s")),
                         i);
    }
    m_phase->setCurrentIndex(qBound(0, current, qMax(0, phases.size() - 1)));
    m_phase->setEnabled(phases.size() > 1);
    connect(m_phase, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &BitplaneExportDialog::refreshMap);
    top->addRow(tr("Phase:"), m_phase);
    layout->addLayout(top);

    m_source = new QLabel(this);
    m_source->setObjectName(QStringLiteral("bitplaneSource"));
    layout->addWidget(m_source);

    auto *group = new QGroupBox(tr("Blocks to write"), this);
    auto *groupLayout = new QVBoxLayout(group);
    auto addBlock = [this, group, groupLayout](QCheckBox *&box, const QString &objectName,
                                               const QString &text, const QString &tip,
                                               bool checked) {
        box = new QCheckBox(text, group);
        box->setObjectName(objectName);
        box->setToolTip(tip);
        box->setChecked(checked);
        groupLayout->addWidget(box);
        connect(box, &QCheckBox::toggled, this, &BitplaneExportDialog::refreshMap);
    };
    addBlock(m_palette, QStringLiteral("bitplanePalette"), tr("Palette"),
             tr("16 colour words, registers 0–15, ready to copy into $ff8240"), true);
    addBlock(m_sprite, QStringLiteral("bitplaneSprite"), tr("Sprite"),
             tr("The frame's four bitplanes in screen format: plane 0–3 per "
                "16-pixel group"),
             true);
    addBlock(m_masked, QStringLiteral("bitplaneMasked"), tr("Sprite + mask"),
             tr("Each 16-pixel group gains a mask word in front of its planes. A "
                "set mask bit keeps the screen, so the sprite blits transparently"),
             true);
    addBlock(m_shifted, QStringLiteral("bitplaneShifted"), tr("Pre-shifted sprite"),
             tr("Every shift from 0 up, each copy one 16-pixel group wider so the "
                "pixels a shift moves out still fit"),
             false);
    addBlock(m_shiftedMasked, QStringLiteral("bitplaneShiftedMasked"),
             tr("Pre-shifted sprite + mask"),
             tr("The pre-shifted copies again, each with its mask words"), false);
    layout->addWidget(group);

    auto *steps = new QFormLayout;
    m_preShifts = new QComboBox(this);
    m_preShifts->setObjectName(QStringLiteral("bitplanePreShifts"));
    for (int count : {2, 4, 8})
        m_preShifts->addItem(tr("%1 pre-shifts (%2 px steps)").arg(count).arg(16 / count),
                             count);
    m_preShifts->setCurrentIndex(1); // 4 copies, 4 px steps
    connect(m_preShifts, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &BitplaneExportDialog::refreshMap);
    steps->addRow(tr("Pre-shift:"), m_preShifts);
    layout->addLayout(steps);

    m_scrollDemo = new QCheckBox(tr("Also write a scroller (.s)"), this);
    m_scrollDemo->setObjectName(QStringLiteral("bitplaneScrollDemo"));
    m_scrollDemo->setToolTip(
        tr("A ready-to-assemble GEMDOS program that sets this palette, animates the "
           "phase's frames and scrolls them across the screen, loading the .dat "
           "written beside it. It needs a colour monitor (RGB/VGA/TV) in 320×200: on "
           "any other screen it says so and waits instead of drawing. It builds with "
           "F7 like any other source"));
    connect(m_scrollDemo, &QCheckBox::toggled, this, &BitplaneExportDialog::refreshMap);
    layout->addWidget(m_scrollDemo);

    auto *mapGroup = new QGroupBox(tr("File map — the labels for your source"), this);
    auto *mapLayout = new QVBoxLayout(mapGroup);
    m_map = new QPlainTextEdit(mapGroup);
    m_map->setObjectName(QStringLiteral("bitplaneMap"));
    m_map->setReadOnly(true);
    m_map->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_map->setMinimumHeight(120);
    mapLayout->addWidget(m_map);
    layout->addWidget(mapGroup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    m_ok = buttons->button(QDialogButtonBox::Ok);

    refreshMap();
}

int BitplaneExportDialog::phase() const
{
    return qMax(0, m_phase->currentData().toInt());
}

BitplaneDataOptions BitplaneExportDialog::options() const
{
    BitplaneDataOptions options;
    options.palette = m_palette->isChecked();
    options.sprite = m_sprite->isChecked();
    options.masked = m_masked->isChecked();
    options.shifted = m_shifted->isChecked();
    options.shiftedMasked = m_shiftedMasked->isChecked();
    options.preShifts = m_preShifts->currentData().toInt();
    return options;
}

bool BitplaneExportDialog::writesScrollDemo() const
{
    return m_scrollDemo->isEnabled() && m_scrollDemo->isChecked();
}

void BitplaneExportDialog::refreshMap()
{
    const BitplaneDataOptions chosen = options();
    m_preShifts->setEnabled(chosen.shifted || chosen.shiftedMasked);

    const int index = qBound(0, m_phase->currentIndex(), qMax(0, m_phases.size() - 1));
    const BitplaneExportPhase selected =
        m_phases.isEmpty() ? BitplaneExportPhase() : m_phases.at(index);
    m_source->setText(tr("%1 \u00d7 %2 px, %3 frame(s), 4 bitplanes")
                          .arg(selected.width)
                          .arg(selected.height)
                          .arg(selected.frames));

    const QVector<BitplaneBlock> blocks =
        bitplaneLayout(selected.width, selected.height, selected.frames, chosen);
    // A scroller needs something to scroll: with no sprite block selected there
    // is nothing for it to draw, so the choice is not offered.
    bool hasSprite = false;
    for (const BitplaneBlock &block : blocks) {
        hasSprite = hasSprite || block.kind == BitplaneBlock::Kind::Sprite
                    || block.kind == BitplaneBlock::Kind::Masked
                    || block.kind == BitplaneBlock::Kind::Shifted
                    || block.kind == BitplaneBlock::Kind::ShiftedMasked;
    }
    m_scrollDemo->setEnabled(hasSprite);
    if (!hasSprite)
        m_scrollDemo->setChecked(false);

    // The map lists frame 0 — every frame has the same shape — plus how many
    // bytes each further frame is on from it. The full list is in the console
    // after an export, and in the scroller's own `equ`s.
    QVector<BitplaneBlock> firstFrame;
    for (const BitplaneBlock &block : blocks) {
        if (block.frame <= 0)
            firstFrame.append(block);
    }
    int total = 0;
    for (const BitplaneBlock &block : blocks)
        total = block.offset + block.bytes;
    // What one frame costs: the distance between the first block of frame 1 and
    // the first block of frame 0 (the palette is written once, before them all).
    int frameStride = 0;
    if (qMax(1, selected.frames) > 1) {
        const BitplaneBlock *frameZero = nullptr;
        const BitplaneBlock *frameOne = nullptr;
        for (const BitplaneBlock &block : blocks) {
            if (block.frame == 0 && !frameZero)
                frameZero = &block;
            if (block.frame == 1 && !frameOne)
                frameOne = &block;
        }
        if (frameZero && frameOne)
            frameStride = frameOne->offset - frameZero->offset;
    }

    QString text;
    for (const MapRow &row : mapRows(firstFrame)) {
        text += QStringLiteral("%1  %2  %3\n")
                    .arg(row.name, -26)
                    .arg(QStringLiteral("$%1").arg(row.offset, 4, 16, QLatin1Char('0')), 7)
                    .arg(row.copies > 1 ? QStringLiteral("%1 ea").arg(row.bytes)
                                        : QString::number(row.bytes),
                         9);
    }
    if (frameStride > 0) {
        text += QStringLiteral("each further frame is %1 bytes on (%2 frames)\n")
                    .arg(frameStride)
                    .arg(selected.frames);
    }
    text += QStringLiteral("%1  %2  %3")
                .arg(QStringLiteral("total"), -26)
                .arg(QString(), 7)
                .arg(total, 9);
    m_map->setPlainText(text);
    // Nothing selected is a blob with no bytes in it, not a file worth writing.
    m_ok->setEnabled(!blocks.isEmpty());
}

} // namespace pist
