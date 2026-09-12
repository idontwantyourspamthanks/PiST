// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FileBrowser.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QLineEdit>
#include <QTreeView>
#include <QVBoxLayout>

namespace pist {

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("Project directory"));
    m_pathEdit->setToolTip(tr("Press Enter to browse this directory."));
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &FileBrowser::onPathEntered);
    layout->addWidget(m_pathEdit);

    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    m_model->setReadOnly(true);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setHeaderHidden(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setAnimated(false);

    // Only the name column is useful in a narrow dock; showing type, size and
    // date would push the names out of view.
    for (int column = 1; column < m_model->columnCount(); ++column)
        m_view->hideColumn(column);

    connect(m_view, &QTreeView::activated, this, &FileBrowser::onActivated);
    layout->addWidget(m_view);
}

void FileBrowser::showFor(const QString &sourcePath)
{
    if (sourcePath.isEmpty())
        return;

    const QFileInfo info(sourcePath);
    if (!info.exists())
        return;

    showDirectory(info.absolutePath());

    // Select the file being edited, so the browser and the editor agree.
    const QModelIndex index = m_model->index(info.absoluteFilePath());
    if (index.isValid()) {
        m_view->setCurrentIndex(index);
        m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
    }
}

void FileBrowser::showDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir())
        return;

    const QString root = info.absoluteFilePath();
    m_model->setRootPath(root);
    m_view->setRootIndex(m_model->index(root));
    m_pathEdit->setText(root);
}

void FileBrowser::onPathEntered()
{
    const QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty())
        return;

    if (QFileInfo(path).isDir())
        showDirectory(path);
    else
        m_pathEdit->setText(m_model->rootPath());
}

void FileBrowser::onActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    const QString path = m_model->filePath(index);
    if (QFileInfo(path).isDir()) {
        m_view->setExpanded(index, !m_view->isExpanded(index));
        return;
    }

    emit fileActivated(path);
}

} // namespace pist
