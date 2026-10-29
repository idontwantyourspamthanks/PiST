// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/AnalysisView.h"

#include "analysis/AssemblyExport.h"
#include "analysis/ProgramTrace.h"
#include "image/StFormats.h"
#include "support/FileWrite.h"
#include "toolchain/Toolchain.h"
#include "ui/Appearance.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include <limits>

namespace pist {

AnalysisView::AnalysisView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("analysisView"));

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("analysisSummary"));
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_listing = new QTableWidget(this);
    m_listing->setObjectName(QStringLiteral("analysisListing"));
    m_listing->setColumnCount(7);
    m_listing->setHorizontalHeaderLabels({tr("Address"), tr("Label"), tr("Bytes"), tr("Instruction"),
                                          tr("Note"), tr("Xrefs"), tr("Comment")});
    m_listing->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listing->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_listing->setSelectionMode(QAbstractItemView::SingleSelection);
    // A narrow gutter, not row numbers: "pc" is the stopped instruction and
    // "ran" is one coverage has seen finish. Both used to live in the note
    // column, which sits past the stretching instruction text, so a step
    // updated the registers while the listing looked unchanged.
    m_listing->verticalHeader()->setVisible(true);
    m_listing->verticalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_listing->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_listing->verticalHeader()->setFixedWidth(44);
    m_listing->setShowGrid(false);
    m_listing->setAlternatingRowColors(true);
    m_listing->horizontalHeader()->setStretchLastSection(false);
    m_listing->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_listing->setContextMenuPolicy(Qt::CustomContextMenu);
    appearance::markMono(m_listing);

    m_rename = new QAction(tr("Rename…"), this);
    m_rename->setObjectName(QStringLiteral("analysisRenameAction"));
    m_comment = new QAction(tr("Comment…"), this);
    m_comment->setObjectName(QStringLiteral("analysisCommentAction"));
    m_markCode = new QAction(tr("Mark as code…"), this);
    m_markCode->setObjectName(QStringLiteral("analysisMarkCodeAction"));
    m_markData = new QAction(tr("Mark as data…"), this);
    m_markData->setObjectName(QStringLiteral("analysisMarkDataAction"));
    m_clearMark = new QAction(tr("Clear mark"), this);
    m_clearMark->setObjectName(QStringLiteral("analysisClearMarkAction"));
    m_export = new QAction(tr("Export assembly…"), this);
    m_export->setObjectName(QStringLiteral("analysisExportAction"));
    m_openDegas = new QAction(tr("Open as Degas picture"), this);
    m_openDegas->setObjectName(QStringLiteral("analysisOpenDegasAction"));
    m_openNeo = new QAction(tr("Open as NeoChrome picture"), this);
    m_openNeo->setObjectName(QStringLiteral("analysisOpenNeoAction"));
    m_openIff = new QAction(tr("Open as IFF picture"), this);
    m_openIff->setObjectName(QStringLiteral("analysisOpenIffAction"));
    m_openSprite = new QAction(tr("Open as sprite…"), this);
    m_openSprite->setObjectName(QStringLiteral("analysisOpenSpriteAction"));
    updateActions();

    connect(m_listing, &QWidget::customContextMenuRequested, this, &AnalysisView::showMenu);
    connect(m_listing, &QTableWidget::itemSelectionChanged, this, &AnalysisView::updateActions);
    connect(m_rename, &QAction::triggered, this, &AnalysisView::renameSelected);
    connect(m_comment, &QAction::triggered, this, &AnalysisView::commentSelected);
    connect(m_markCode, &QAction::triggered, this, &AnalysisView::markSelectedCode);
    connect(m_markData, &QAction::triggered, this, &AnalysisView::markSelectedData);
    connect(m_clearMark, &QAction::triggered, this, &AnalysisView::clearSelectedMark);
    connect(m_export, &QAction::triggered, this, &AnalysisView::exportSelected);
    connect(m_openDegas, &QAction::triggered, this, &AnalysisView::openSelectedDegas);
    connect(m_openNeo, &QAction::triggered, this, &AnalysisView::openSelectedNeo);
    connect(m_openIff, &QAction::triggered, this, &AnalysisView::openSelectedIff);
    connect(m_openSprite, &QAction::triggered, this, &AnalysisView::openSelectedSprite);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_summary);
    layout->addWidget(m_listing, 1);
}

void AnalysisView::setIdentity(const QString &path, const QString &name)
{
    m_path = path;
    if (!name.isEmpty())
        m_name = name;
}

QString AnalysisView::filePath() const
{
    return m_path;
}

QString AnalysisView::displayName() const
{
    return m_name.isEmpty() ? tr("Analysis") : m_name;
}

void AnalysisView::setBytes(const QByteArray &bytes)
{
    m_bytes = bytes;
    reload();
}

bool AnalysisView::renameAddress(quint32 address, const QString &name, QString *error)
{
    if (address >= m_endAddress) {
        if (error)
            *error = tr("That address is outside the program.");
        return false;
    }
    AnalysisNotes next = m_notes;
    if (!setAnalysisName(&next, address, name, error))
        return false;
    return apply(next, error);
}

bool AnalysisView::setAddressComment(quint32 address, const QString &text, QString *error)
{
    if (address >= m_endAddress) {
        if (error)
            *error = tr("That address is outside the program.");
        return false;
    }
    AnalysisNotes next = m_notes;
    if (!setAnalysisComment(&next, address, text, error))
        return false;
    return apply(next, error);
}

bool AnalysisView::markBytes(quint32 address, quint32 length, AnalysisOverride::Kind kind, QString *error)
{
    if (m_imageSize == 0 || address >= m_imageSize || length > m_imageSize - address) {
        if (error)
            *error = tr("That range is outside the program.");
        return false;
    }
    AnalysisNotes next = m_notes;
    if (!setAnalysisOverride(&next, address, length, kind, error))
        return false;
    return apply(next, error);
}

bool AnalysisView::clearMark(quint32 address, quint32 length, QString *error)
{
    if (m_imageSize == 0 || address >= m_imageSize || length > m_imageSize - address) {
        if (error)
            *error = tr("That range is outside the program.");
        return false;
    }
    AnalysisNotes next = m_notes;
    if (!clearAnalysisOverride(&next, address, length, error))
        return false;
    return apply(next, error);
}

void AnalysisView::showMenu(const QPoint &pos)
{
    QMenu menu(m_listing);
    menu.addAction(m_rename);
    menu.addAction(m_comment);
    menu.addSeparator();
    menu.addAction(m_markCode);
    menu.addAction(m_markData);
    menu.addAction(m_clearMark);
    menu.addSeparator();
    menu.addAction(m_openDegas);
    menu.addAction(m_openNeo);
    menu.addAction(m_openIff);
    menu.addAction(m_openSprite);
    menu.addSeparator();
    menu.addAction(m_export);
    menu.exec(m_listing->viewport()->mapToGlobal(pos));
}

void AnalysisView::renameSelected()
{
    quint32 address = 0;
    if (!selectedSpan(&address, nullptr))
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename"),
                                               tr("Name at $%1:").arg(address, 0, 16),
                                               QLineEdit::Normal, m_notes.names.value(address), &ok);
    if (!ok)
        return;
    QString error;
    if (!renameAddress(address, name, &error))
        QMessageBox::warning(this, tr("Rename"), error);
}

void AnalysisView::commentSelected()
{
    quint32 address = 0;
    if (!selectedSpan(&address, nullptr))
        return;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Comment"),
                                               tr("Comment at $%1:").arg(address, 0, 16),
                                               QLineEdit::Normal, m_notes.comments.value(address), &ok);
    if (!ok)
        return;
    QString error;
    if (!setAddressComment(address, text, &error))
        QMessageBox::warning(this, tr("Comment"), error);
}

void AnalysisView::markSelectedCode()
{
    quint32 address = 0;
    quint32 length = 0;
    if (!selectedSpan(&address, &length) || length == 0)
        return;
    const quint32 room = m_imageSize - address;
    const int maxLength = int(std::min(room, quint32(std::numeric_limits<int>::max())));
    bool ok = false;
    const int chosen = QInputDialog::getInt(this, tr("Mark as code"), tr("Number of bytes:"),
                                            int(std::min(length, quint32(maxLength))), 1, maxLength, 1, &ok);
    if (!ok)
        return;
    QString error;
    if (!markBytes(address, quint32(chosen), AnalysisOverride::Kind::Code, &error))
        QMessageBox::warning(this, tr("Mark as code"), error);
}

void AnalysisView::markSelectedData()
{
    quint32 address = 0;
    quint32 length = 0;
    if (!selectedSpan(&address, &length) || length == 0)
        return;
    const quint32 room = m_imageSize - address;
    const int maxLength = int(std::min(room, quint32(std::numeric_limits<int>::max())));
    bool ok = false;
    const int chosen = QInputDialog::getInt(this, tr("Mark as data"), tr("Number of bytes:"),
                                            int(std::min(length, quint32(maxLength))), 1, maxLength, 1, &ok);
    if (!ok)
        return;
    QString error;
    if (!markBytes(address, quint32(chosen), AnalysisOverride::Kind::Data, &error))
        QMessageBox::warning(this, tr("Mark as data"), error);
}

void AnalysisView::clearSelectedMark()
{
    quint32 address = 0;
    quint32 length = 0;
    if (!selectedSpan(&address, &length) || length == 0)
        return;
    QString error;
    if (!clearMark(address, length, &error))
        QMessageBox::warning(this, tr("Clear mark"), error);
}

bool AnalysisView::selectedSpan(quint32 *address, quint32 *length) const
{
    const int row = m_listing->currentRow();
    if (row < 0)
        return false;
    const QTableWidgetItem *item = m_listing->item(row, 0);
    if (!item)
        return false;
    if (address)
        *address = item->data(Qt::UserRole).toUInt();
    if (length)
        *length = item->data(Qt::UserRole + 1).toUInt();
    return true;
}

bool AnalysisView::apply(const AnalysisNotes &next, QString *error)
{
    if (!m_ready) {
        if (error)
            *error = tr("This file has no analysis listing.");
        return false;
    }
    if (m_path.isEmpty()) {
        if (error)
            *error = tr("This analysis is not stored in a file.");
        return false;
    }
    if (!saveAnalysisNotes(analysisNotesPath(m_path), next, error))
        return false;
    reload();
    if (!m_ready) {
        if (error)
            *error = m_summary->text();
        return false;
    }
    return true;
}

void AnalysisView::reload()
{
    m_ready = false;
    m_imageSize = 0;
    m_endAddress = 0;
    m_textSize = 0;
    m_dataSize = 0;
    m_bssSize = 0;
    if (!m_path.isEmpty()) {
        QString error;
        AnalysisNotes notes;
        if (!loadAnalysisNotes(analysisNotesPath(m_path), &notes, &error)) {
            m_notes = {};
            m_summary->setText(error);
            m_listing->setRowCount(0);
            updateActions();
            return;
        }
        m_notes = notes;
    }
    const ProgramListing listing = listProgram(m_bytes, m_notes);
    m_imageSize = listing.imageSize;
    m_endAddress = listing.endAddress;
    m_textSize = listing.textSize;
    m_dataSize = listing.dataSize;
    m_bssSize = listing.bssSize;
    m_summary->setText(listing.error.isEmpty() ? listing.summary : listing.error);
    m_listing->setRowCount(listing.rows.size());
    for (int row = 0; row < listing.rows.size(); ++row) {
        const ListingRow &source = listing.rows.at(row);
        const QStringList cells = {
            QStringLiteral("$%1").arg(source.address, 0, 16),
            source.label,
            source.bytes,
            source.text,
            source.note,
            source.xrefs,
            source.comment,
        };
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (column == 0) {
                item->setData(Qt::UserRole, source.address);
                item->setData(Qt::UserRole + 1, source.length);
            } else if (column == 4) {
                // The listing's own note, kept so a coverage prefix can be
                // added and taken off without rebuilding the row.
                item->setData(Qt::UserRole, source.note);
            }
            m_listing->setItem(row, column, item);
        }
    }
    m_listing->resizeColumnsToContents();
    m_listing->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_ready = listing.error.isEmpty();
    paintMarks();
    updateActions();
}

void AnalysisView::updateActions()
{
    quint32 address = 0;
    quint32 length = 0;
    const bool row = selectedSpan(&address, &length);
    m_rename->setEnabled(row && m_ready);
    m_comment->setEnabled(row && m_ready);
    const bool bytes = row && m_ready && length > 0 && address < m_imageSize;
    m_markCode->setEnabled(bytes && (address % 2u) == 0);
    m_markData->setEnabled(bytes);
    m_clearMark->setEnabled(bytes);
    m_export->setEnabled(m_ready);
    const bool inImage = row && m_ready && address < m_imageSize;
    const quint32 room = inImage ? m_imageSize - address : 0;
    m_openDegas->setEnabled(room >= quint32(kDegasPictureBytes));
    m_openNeo->setEnabled(room >= quint32(kNeoPictureBytes));
    m_openSprite->setEnabled(inImage);
    bool iff = false;
    if (inImage && m_bytes.size() >= 28 + int(address) + 4)
        iff = m_bytes.mid(28 + int(address), 4) == QByteArrayLiteral("FORM");
    m_openIff->setEnabled(iff);
}

void AnalysisView::setCoverage(const QVector<CoverageSample> &samples, quint32 textBase,
                               quint32 dataBase, quint32 bssBase)
{
    const QVector<quint32> image = coverageImageAddresses(samples, textBase, dataBase, bssBase,
                                                          m_textSize, m_dataSize, m_bssSize);
    m_observed.clear();
    for (quint32 address : image)
        m_observed.insert(address);
    paintMarks();
}

void AnalysisView::setExecutionPc(quint32 pc, quint32 textBase, quint32 dataBase, quint32 bssBase)
{
    m_havePc = true;
    m_pc = pc;
    m_textBase = textBase;
    m_dataBase = dataBase;
    m_bssBase = bssBase;
    paintMarks();
}

void AnalysisView::clearExecutionPc()
{
    if (!m_havePc)
        return;
    m_havePc = false;
    paintMarks();
}

std::optional<quint32> AnalysisView::executionAddress() const
{
    if (!m_havePc)
        return std::nullopt;
    const std::optional<quint32> image = imageAddressForPc(m_pc, m_textBase, m_dataBase, m_bssBase,
                                                           m_textSize, m_dataSize, m_bssSize);
    if (!image || rowForImage(*image) < 0)
        return std::nullopt;
    return image;
}

bool AnalysisView::openPicture(PictureReading reading, quint32 address, int width, int height,
                               QString *path, QString *note, QString *error)
{
    if (!m_ready) {
        if (error)
            *error = tr("This file has no analysis listing.");
        return false;
    }
    // The listing's addresses are image-relative. The image starts after the
    // 28-byte TOS header.
    constexpr int kHeader = 28;
    int need = 0;
    switch (reading) {
    case PictureReading::Degas:
        need = kDegasPictureBytes;
        break;
    case PictureReading::Neo:
        need = kNeoPictureBytes;
        break;
    case PictureReading::Iff:
        need = 12;
        break;
    case PictureReading::Sprite:
        if (width < 16 || width > 320 || (width % 16) != 0 || height < 1 || height > 200) {
            if (error)
                *error = tr("A sprite is 16 to 320 pixels wide, in steps of 16, and 1 to 200 tall.");
            return false;
        }
        need = (width / 16) * 8 * height;
        break;
    }
    if (address >= m_imageSize || quint32(need) > m_imageSize - address
        || m_bytes.size() < kHeader + int(address) + need) {
        if (error)
            *error = tr("That range is outside the program.");
        return false;
    }
    const int available = reading == PictureReading::Iff ? int(m_imageSize - address) : need;
    const QByteArray span = m_bytes.mid(kHeader + int(address), available);
    PictureFile file;
    if (!bytesAsPicture(reading, span, width, height, &file, error))
        return false;
    const QString written = QDir::temp().filePath(
        QStringLiteral("pist-analysis-%1.%2").arg(++m_pictureSerial).arg(file.suffix));
    if (!files::write(written, file.bytes, error))
        return false;
    if (path)
        *path = written;
    if (note)
        *note = file.note;
    return true;
}

void AnalysisView::openSelectedPicture(PictureReading reading, const QString &title, int width,
                                       int height)
{
    quint32 address = 0;
    if (!selectedSpan(&address, nullptr))
        return;
    QString path;
    QString note;
    QString error;
    if (!openPicture(reading, address, width, height, &path, &note, &error)) {
        QMessageBox::warning(this, title, error);
        return;
    }
    emit pictureReady(path);
    if (!note.isEmpty())
        QMessageBox::information(this, title, note);
}

void AnalysisView::openSelectedDegas()
{
    openSelectedPicture(PictureReading::Degas, tr("Open as Degas picture"), 0, 0);
}

void AnalysisView::openSelectedNeo()
{
    openSelectedPicture(PictureReading::Neo, tr("Open as NeoChrome picture"), 0, 0);
}

void AnalysisView::openSelectedIff()
{
    openSelectedPicture(PictureReading::Iff, tr("Open as IFF picture"), 0, 0);
}

void AnalysisView::openSelectedSprite()
{
    quint32 address = 0;
    if (!selectedSpan(&address, nullptr))
        return;
    bool ok = false;
    const int width = QInputDialog::getInt(this, tr("Open as sprite"), tr("Width:"), 16, 16, 320, 16,
                                           &ok);
    if (!ok)
        return;
    const int height = QInputDialog::getInt(this, tr("Open as sprite"), tr("Height:"), 16, 1, 200, 1,
                                            &ok);
    if (!ok)
        return;
    openSelectedPicture(PictureReading::Sprite, tr("Open as sprite"), width, height);
}

bool AnalysisView::exportAssemblyTo(const QString &path, QString *error)
{
    if (!m_ready) {
        if (error)
            *error = tr("This file has no analysis listing.");
        return false;
    }
    const QString source = exportAssembly(m_bytes, m_notes, error);
    if (source.isEmpty())
        return false;
    return files::write(path, source.toUtf8(), error);
}

void AnalysisView::exportSelected()
{
    if (!m_ready)
        return;
    QString suggested;
    if (!m_path.isEmpty()) {
        const QFileInfo info(m_path);
        suggested = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                    + QStringLiteral(".s");
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export assembly"), suggested,
                                                      tr("Assembly source (*.s)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exportAssemblyTo(path, &error)) {
        QMessageBox::warning(this, tr("Export assembly"), error);
        return;
    }

    QString overridePath;
    if (auto *ide = qobject_cast<MainWindow *>(window()))
        overridePath = ide->assemblerPath();
    const ToolInfo vasm = toolchain::findAssembler(overridePath);
    const QString fileName = QFileInfo(path).fileName();
    if (!vasm.found()) {
        QMessageBox::information(this, tr("Export assembly"),
                                 tr("Wrote %1.\n\nThe assembler was not found, so the bytes were not checked.")
                                     .arg(fileName));
        return;
    }
    QString mismatch;
    const QString source = exportAssembly(m_bytes, m_notes, &error);
    if (source.isEmpty() || !assembledImageMatches(vasm.path, source, m_bytes, &mismatch)) {
        QMessageBox::warning(this, tr("Export assembly"),
                             tr("Wrote %1, but assembling it does not reproduce the original program.\n\n%2")
                                 .arg(fileName, source.isEmpty() ? error : mismatch));
        return;
    }
    QMessageBox::information(this, tr("Export assembly"),
                             tr("Wrote %1. The assembled program matches the original.").arg(fileName));
}

int AnalysisView::rowForImage(quint32 address) const
{
    int spanning = -1;
    for (int row = 0; row < m_listing->rowCount(); ++row) {
        const QTableWidgetItem *item = m_listing->item(row, 0);
        if (!item)
            continue;
        const quint32 start = item->data(Qt::UserRole).toUInt();
        const quint32 length = item->data(Qt::UserRole + 1).toUInt();
        if (length == 0 && start == address)
            return row;
        if (length > 0 && address >= start && (address - start) < length)
            spanning = row;
    }
    return spanning;
}

void AnalysisView::paintMarks()
{
    const std::optional<quint32> image =
        m_havePc ? imageAddressForPc(m_pc, m_textBase, m_dataBase, m_bssBase, m_textSize, m_dataSize,
                                     m_bssSize)
                 : std::nullopt;
    const int hit = image ? rowForImage(*image) : -1;
    const QColor pc = appearance::colors().pcRow;
    for (int row = 0; row < m_listing->rowCount(); ++row) {
        QTableWidgetItem *address = m_listing->item(row, 0);
        QTableWidgetItem *note = m_listing->item(row, 4);
        const bool here = row == hit;
        bool ran = false;
        if (address && note) {
            const quint32 start = address->data(Qt::UserRole).toUInt();
            const quint32 length = address->data(Qt::UserRole + 1).toUInt();
            ran = m_observed.contains(start);
            if (!ran && length > 1) {
                const quint32 end = start + length;
                for (quint32 at = start + 1; at < end; ++at) {
                    if (m_observed.contains(at)) {
                        ran = true;
                        break;
                    }
                }
            }
            const QString source = note->data(Qt::UserRole).toString();
            if (ran)
                note->setText(source.isEmpty() ? QStringLiteral("ran")
                                               : QStringLiteral("ran, ") + source);
            else
                note->setText(source);
        }
        for (int column = 0; column < m_listing->columnCount(); ++column) {
            QTableWidgetItem *item = m_listing->item(row, column);
            if (!item)
                continue;
            if (here)
                item->setBackground(pc);
            else
                item->setData(Qt::BackgroundRole, QVariant());
            QFont font = item->font();
            font.setBold(here);
            item->setFont(font);
        }
        // The gutter stays put when the note column is scrolled away. "pc" is
        // the instruction the machine is stopped on; "ran" has finished.
        QString marker;
        if (here)
            marker = QStringLiteral("pc");
        else if (ran)
            marker = QStringLiteral("ran");
        QTableWidgetItem *gutter = m_listing->verticalHeaderItem(row);
        if (!gutter) {
            gutter = new QTableWidgetItem;
            m_listing->setVerticalHeaderItem(row, gutter);
        }
        gutter->setText(marker);
        gutter->setTextAlignment(Qt::AlignCenter);
    }
    if (hit >= 0) {
        const QModelIndex idx = m_listing->model()->index(hit, 0);
        const QRect visible = m_listing->viewport()->rect();
        if (!visible.contains(m_listing->visualRect(idx)))
            m_listing->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }
}

} // namespace pist
