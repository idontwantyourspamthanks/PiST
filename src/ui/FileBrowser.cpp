// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FileBrowser.h"

#include "build/FloppyImage.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <functional>

namespace pist {

namespace {

const char kFloppyFilter[] = "Disk images (*.st *.msa *.img *.dim *.ipf);;All files (*)";

QWidget *makeGroup(const QString &title, QWidget *parent, QHBoxLayout **headerOut)
{
    auto *group = new QWidget(parent);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(2, 2, 2, 0);
    header->setSpacing(4);
    auto *label = new QLabel(title, group);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    header->addWidget(label);
    layout->addLayout(header);
    *headerOut = header;
    return group;
}

void compactButton(QPushButton *button)
{
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
}

} // namespace

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->setObjectName(QStringLiteral("diskSplitter"));
    split->setChildrenCollapsible(false);

    QHBoxLayout *hdHeader = nullptr;
    QWidget *hd = makeGroup(tr("Hard Drive"), this, &hdHeader);
    m_export = new QPushButton(tr("Export…"), hd);
    m_export->setObjectName(QStringLiteral("hardDriveExport"));
    m_export->setToolTip(tr("Write the selected hard-drive files to a new .st or .msa floppy image."));
    compactButton(m_export);
    hdHeader->addStretch();
    hdHeader->addWidget(m_export);
    connect(m_export, &QPushButton::clicked, this, &FileBrowser::onExportFloppy);

    m_pathEdit = new QLineEdit(hd);
    m_pathEdit->setObjectName(QStringLiteral("hardDrivePath"));
    m_pathEdit->setPlaceholderText(tr("Project directory"));
    m_pathEdit->setToolTip(tr("Press Enter to browse this directory."));
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &FileBrowser::onPathEntered);
    hd->layout()->addWidget(m_pathEdit);

    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    // Read-write, so renames through the inline editor land on disk. Deletion
    // and creation go through the slots below either way.
    m_model->setReadOnly(false);
    connect(m_model, &QFileSystemModel::fileRenamed, this,
            &FileBrowser::onModelPathRenamed);

    m_view = new QTreeView(hd);
    m_view->setObjectName(QStringLiteral("hardDriveView"));
    m_view->setModel(m_model);
    m_view->setHeaderHidden(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);

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
    qobject_cast<QVBoxLayout *>(hd->layout())->addWidget(m_view, 1);
    split->addWidget(hd);

    for (int drive = 0; drive < 2; ++drive) {
        FloppyPane &pane = m_floppy[drive];
        const QString title = drive == 0 ? tr("Disk A") : tr("Disk B");
        QHBoxLayout *header = nullptr;
        QWidget *group = makeGroup(title, this, &header);
        pane.diskName = new QLabel(tr("No disk"), group);
        pane.diskName->setObjectName(drive == 0 ? QStringLiteral("diskAName")
                                                : QStringLiteral("diskBName"));
        pane.diskName->setEnabled(false);
        header->addWidget(pane.diskName, 1);

        pane.change = new QPushButton(tr("Change…"), group);
        pane.change->setObjectName(drive == 0 ? QStringLiteral("diskAChange")
                                              : QStringLiteral("diskBChange"));
        pane.change->setToolTip(tr("Insert a floppy image into this drive."));
        pane.change->setProperty("drive", drive);
        compactButton(pane.change);
        connect(pane.change, &QPushButton::clicked, this, &FileBrowser::onChangeFloppy);
        header->addWidget(pane.change);

        pane.eject = new QPushButton(tr("Eject"), group);
        pane.eject->setObjectName(drive == 0 ? QStringLiteral("diskAEject")
                                             : QStringLiteral("diskBEject"));
        pane.eject->setToolTip(tr("Remove the floppy image from this drive."));
        pane.eject->setProperty("drive", drive);
        pane.eject->setEnabled(false);
        compactButton(pane.eject);
        connect(pane.eject, &QPushButton::clicked, this, &FileBrowser::onEjectFloppy);
        header->addWidget(pane.eject);

        pane.model = new QStandardItemModel(group);
        pane.view = new QTreeView(group);
        pane.view->setObjectName(drive == 0 ? QStringLiteral("diskAView")
                                            : QStringLiteral("diskBView"));
        pane.view->setModel(pane.model);
        pane.view->setHeaderHidden(true);
        pane.view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        pane.view->setSelectionMode(QAbstractItemView::SingleSelection);
        pane.view->setAnimated(false);
        qobject_cast<QVBoxLayout *>(group->layout())->addWidget(pane.view, 1);
        split->addWidget(group);
        refreshFloppy(drive);
    }

    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 1);
    layout->addWidget(split);
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

QString FileBrowser::directory() const
{
    return m_model ? m_model->rootPath() : QString();
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

void FileBrowser::setFloppyImages(const QStringList &images)
{
    m_floppyPath[0] = images.value(0);
    m_floppyPath[1] = images.value(1);
    refreshFloppy(0);
    refreshFloppy(1);
}

QStringList FileBrowser::floppyImages() const
{
    return {m_floppyPath[0], m_floppyPath[1]};
}

QStringList FileBrowser::selectedHardDrivePaths() const
{
    QStringList paths;
    const QModelIndexList indexes = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &index : indexes) {
        const QString path = m_model->filePath(index);
        if (!path.isEmpty())
            paths.append(path);
    }
    paths.removeDuplicates();
    return paths;
}

bool FileBrowser::exportHardDriveSelection(const QString &imagePath, QString *error)
{
    const QStringList selected = selectedHardDrivePaths();
    if (selected.isEmpty()) {
        if (error)
            *error = tr("Select files or folders on the hard drive to export.");
        return false;
    }
    QString root = directory();
    if (root.isEmpty())
        root = QFileInfo(selected.first()).absolutePath();
    QVector<floppy::Item> items;
    if (!floppy::collectHostItems(root, selected, &items, error))
        return false;
    return floppy::writeImage(imagePath, items, error);
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
    QAction *exportFloppy = menu.addAction(tr("Export to Floppy Image…"));
    exportFloppy->setEnabled(!selectedHardDrivePaths().isEmpty() || index.isValid());
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
    } else if (chosen == exportFloppy) {
        onExportFloppy();
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

int FileBrowser::driveOfSender() const
{
    const QObject *s = sender();
    if (!s)
        return -1;
    bool ok = false;
    const int drive = s->property("drive").toInt(&ok);
    if (!ok || drive < 0 || drive > 1)
        return -1;
    return drive;
}

void FileBrowser::onChangeFloppy()
{
    const int drive = driveOfSender();
    if (drive < 0)
        return;
    const QString start = !m_floppyPath[drive].isEmpty()
        ? QFileInfo(m_floppyPath[drive]).absolutePath()
        : directory();
    const QString title = drive == 0 ? tr("Select a floppy image for drive A:")
                                     : tr("Select a floppy image for drive B:");
    const QString path = QFileDialog::getOpenFileName(this, title, start,
                                                      tr(kFloppyFilter));
    if (path.isEmpty())
        return;
    m_floppyPath[drive] = path;
    refreshFloppy(drive);
    emit floppyImageChanged(drive, path);
}

void FileBrowser::onEjectFloppy()
{
    const int drive = driveOfSender();
    if (drive < 0)
        return;
    m_floppyPath[drive].clear();
    refreshFloppy(drive);
    emit floppyImageChanged(drive, QString());
}

void FileBrowser::onExportFloppy()
{
    if (selectedHardDrivePaths().isEmpty()) {
        QMessageBox::information(this, tr("Export floppy"),
                                 tr("Select files or folders on the hard drive to export."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export floppy image"), directory(),
        tr("ST disk image (*.st);;MSA disk image (*.msa)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!exportHardDriveSelection(path, &error)) {
        QMessageBox::warning(this, tr("Export floppy"),
                             tr("Could not write %1:\n%2").arg(path, error));
        return;
    }
    QMessageBox::information(this, tr("Export floppy"),
                             tr("Wrote %1.").arg(QFileInfo(path).fileName()));
}

void FileBrowser::refreshFloppy(int drive)
{
    if (drive < 0 || drive > 1)
        return;
    FloppyPane &pane = m_floppy[drive];
    const QString path = m_floppyPath[drive];
    pane.model->clear();
    pane.eject->setEnabled(!path.isEmpty());

    if (path.isEmpty()) {
        pane.diskName->setText(tr("No disk"));
        pane.diskName->setToolTip(QString());
        pane.diskName->setEnabled(false);
        auto *item = new QStandardItem(tr("No disk inserted"));
        item->setEnabled(false);
        item->setSelectable(false);
        pane.model->appendRow(item);
        return;
    }

    const QFileInfo info(path);
    pane.diskName->setText(info.fileName());
    pane.diskName->setToolTip(info.absoluteFilePath());
    pane.diskName->setEnabled(true);

    if (!info.exists()) {
        auto *item = new QStandardItem(tr("Image not found"));
        item->setEnabled(false);
        item->setSelectable(false);
        pane.model->appendRow(item);
        return;
    }

    QString error;
    const QVector<floppy::Entry> entries = floppy::listImage(path, &error);
    if (!error.isEmpty()) {
        auto *item = new QStandardItem(error);
        item->setEnabled(false);
        item->setSelectable(false);
        pane.model->appendRow(item);
        return;
    }

    QFileIconProvider icons;
    QHash<QString, QStandardItem *> dirs;
    std::function<QStandardItem *(const QString &)> folderFor;
    folderFor = [&](const QString &dirPath) -> QStandardItem * {
        if (dirPath.isEmpty())
            return nullptr;
        if (QStandardItem *have = dirs.value(dirPath, nullptr))
            return have;
        const int slash = dirPath.lastIndexOf(QLatin1Char('/'));
        QStandardItem *parent = slash < 0 ? nullptr : folderFor(dirPath.left(slash));
        auto *item = new QStandardItem(slash < 0 ? dirPath : dirPath.mid(slash + 1));
        item->setIcon(icons.icon(QFileIconProvider::Folder));
        item->setEditable(false);
        if (parent)
            parent->appendRow(item);
        else
            pane.model->appendRow(item);
        dirs.insert(dirPath, item);
        return item;
    };

    for (const floppy::Entry &entry : entries) {
        const int slash = entry.path.lastIndexOf(QLatin1Char('/'));
        QStandardItem *parent = slash < 0 ? nullptr : folderFor(entry.path.left(slash));
        if (entry.isDirectory) {
            folderFor(entry.path);
            continue;
        }
        auto *item = new QStandardItem(slash < 0 ? entry.path : entry.path.mid(slash + 1));
        item->setIcon(icons.icon(QFileIconProvider::File));
        item->setEditable(false);
        if (parent)
            parent->appendRow(item);
        else
            pane.model->appendRow(item);
    }

    if (pane.model->rowCount() == 0) {
        auto *item = new QStandardItem(tr("Empty disk"));
        item->setEnabled(false);
        item->setSelectable(false);
        pane.model->appendRow(item);
    }
}

} // namespace pist
