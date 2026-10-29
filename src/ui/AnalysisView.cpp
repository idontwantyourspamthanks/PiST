// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/AnalysisView.h"

#include "analysis/ProgramTrace.h"
#include "ui/Appearance.h"

#include <QAction>
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
    m_listing->verticalHeader()->hide();
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
    updateActions();

    connect(m_listing, &QWidget::customContextMenuRequested, this, &AnalysisView::showMenu);
    connect(m_listing, &QTableWidget::itemSelectionChanged, this, &AnalysisView::updateActions);
    connect(m_rename, &QAction::triggered, this, &AnalysisView::renameSelected);
    connect(m_comment, &QAction::triggered, this, &AnalysisView::commentSelected);
    connect(m_markCode, &QAction::triggered, this, &AnalysisView::markSelectedCode);
    connect(m_markData, &QAction::triggered, this, &AnalysisView::markSelectedData);
    connect(m_clearMark, &QAction::triggered, this, &AnalysisView::clearSelectedMark);

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
            }
            m_listing->setItem(row, column, item);
        }
    }
    m_listing->resizeColumnsToContents();
    m_listing->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_ready = listing.error.isEmpty();
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
}

} // namespace pist
