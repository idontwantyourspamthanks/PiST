// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FileBrowser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTreeView>
#include <QShortcut>
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
    // Read-write, so renames through the inline editor land on disk. Deletion
    // and creation go through the slots below either way.
    m_model->setReadOnly(false);
    connect(m_model, &QFileSystemModel::fileRenamed, this,
            &FileBrowser::onModelPathRenamed);

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setHeaderHidden(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, m_view);
    connect(deleteShortcut, &QShortcut::activated, this, [this] {
        const QModelIndex index = m_view->currentIndex();
        if (index.isValid())
            confirmAndDelete(m_model->filePath(index));
    });
    auto *renameShortcut = new QShortcut(QKeySequence(QStringLiteral("F2")), m_view);
    connect(renameShortcut, &QShortcut::activated, this, [this] {
        const QModelIndex index = m_view->currentIndex();
        if (index.isValid())
            m_view->edit(index);
    });
    m_view->setAnimated(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QTreeView::customContextMenuRequested, this,
            &FileBrowser::onContextMenu);

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

QString FileBrowser::contextDirectory() const
{
    const QModelIndex index = m_view->currentIndex();
    if (index.isValid()) {
        const QString path = m_model->filePath(index);
        if (QFileInfo(path).isDir())
            return path;
        return QFileInfo(path).absolutePath();
    }
    return m_model->rootPath();
}

QString FileBrowser::createFile(const QString &dir, const QString &name)
{
    // A plain name only: anything with separators would escape (or fail
    // inside) the chosen directory.
    if (name.isEmpty() || name != QFileInfo(name).fileName())
        return {};
    const QString path = QDir(dir).absoluteFilePath(name);
    if (QFileInfo::exists(path))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.close();

    const QModelIndex index = m_model->index(path);
    if (index.isValid()) {
        m_view->setCurrentIndex(index);
        m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
    }
    return path;
}

QString FileBrowser::createFolder(const QString &dir, const QString &name)
{
    if (name.isEmpty() || name != QFileInfo(name).fileName())
        return {};
    const QString path = QDir(dir).absoluteFilePath(name);
    if (QFileInfo::exists(path))
        return {};
    if (!QDir(dir).mkdir(name))
        return {};

    const QModelIndex index = m_model->index(path);
    if (index.isValid()) {
        m_view->setCurrentIndex(index);
        m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
    }
    return path;
}

bool FileBrowser::deletePath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;

    // Report before deleting, so the receiver can act on the path while the
    // model still considers it valid.
    emit pathDeleted(info.absoluteFilePath());

    if (info.isDir())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

bool FileBrowser::renamePath(const QString &path, const QString &newName)
{
    if (newName.isEmpty() || newName != QFileInfo(newName).fileName())
        return false;
    const QModelIndex index = m_model->index(path);
    if (!index.isValid())
        return false;
    return m_model->setData(index, newName);
}

void FileBrowser::onContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    const QString selectedPath = index.isValid() ? m_model->filePath(index) : QString();

    QMenu menu(this);
    QAction *newFile = menu.addAction(tr("New File…"));
    QAction *newImage = menu.addAction(tr("New Image…"));
    QAction *newFolder = menu.addAction(tr("New Folder…"));
    menu.addSeparator();
    QAction *rename = menu.addAction(tr("Rename…"));
    QAction *remove = menu.addAction(tr("Delete…"));
    rename->setEnabled(index.isValid());
    remove->setEnabled(index.isValid());

    QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (chosen == newFile) {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("New File"), tr("File name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok)
            return;
        const QString path = createFile(contextDirectory(), name.trimmed());
        if (path.isEmpty())
            QMessageBox::warning(this, tr("New File"),
                                 tr("Could not create the file. The name may be invalid or "
                                    "already exist."));
    } else if (chosen == newImage) {
        emit newImageRequested(contextDirectory());
    } else if (chosen == newFolder) {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("New Folder"), tr("Folder name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok)
            return;
        const QString path = createFolder(contextDirectory(), name.trimmed());
        if (path.isEmpty())
            QMessageBox::warning(this, tr("New Folder"),
                                 tr("Could not create the folder. The name may be invalid or "
                                    "already exist."));
    } else if (chosen == rename) {
        // The model is read-write, so the inline editor commits the rename.
        m_view->edit(index);
    } else if (chosen == remove) {
        confirmAndDelete(selectedPath);
    }
}

bool FileBrowser::confirmAndDelete(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;

    const QString text = info.isDir()
        ? tr("Delete the folder %1 and everything in it?").arg(info.fileName())
        : tr("Delete %1?").arg(info.fileName());
    if (QMessageBox::question(this, tr("Delete"), text,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return false;

    if (!deletePath(path)) {
        QMessageBox::warning(this, tr("Delete"), tr("Could not delete %1.").arg(path));
        return false;
    }
    return true;
}

void FileBrowser::onModelPathRenamed(const QString &path, const QString &oldName,
                                     const QString &newName)
{
    // The model reports the directory and the two bare names; consumers want
    // full paths.
    emit pathRenamed(QDir(path).absoluteFilePath(oldName),
                     QDir(path).absoluteFilePath(newName));
}

} // namespace pist
