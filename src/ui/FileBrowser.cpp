// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FileBrowser.h"

#include "build/FloppyImage.h"

#include <QDir>
#include <QDrag>
#include <QDropEvent>
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
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace pist {

namespace {

const char kFloppyFilter[] = "Disk images (*.st *.msa *.img *.dim *.ipf);;All files (*)";

// Carries a drag out of a floppy pane: the source image path, then one
// image-relative entry path per line.
const QString kFloppyMime = QStringLiteral("application/x-pist-floppy-entries");

QMimeData *floppyDragMime(const QString &imagePath, const QStringList &entries)
{
    if (imagePath.isEmpty() || entries.isEmpty())
        return nullptr;
    auto *mime = new QMimeData;
    mime->setData(kFloppyMime,
                  (imagePath + QLatin1Char('\n') + entries.join(QLatin1Char('\n'))).toUtf8());
    return mime;
}

bool floppyDragPayload(const QMimeData *mime, QString *imagePath, QStringList *entries)
{
    if (!mime->hasFormat(kFloppyMime))
        return false;
    const QString text = QString::fromUtf8(mime->data(kFloppyMime));
    const int firstNewline = text.indexOf(QLatin1Char('\n'));
    if (firstNewline < 0)
        return false;
    *imagePath = text.left(firstNewline);
    *entries = text.mid(firstNewline + 1).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return !imagePath->isEmpty() && !entries->isEmpty();
}

QString normalizedEntryPath(const QString &path)
{
    QString clean = QDir::fromNativeSeparators(path.trimmed());
    while (clean.startsWith(QLatin1Char('/')))
        clean.remove(0, 1);
    while (clean.endsWith(QLatin1Char('/')))
        clean.chop(1);
    return clean;
}

/// A free path in `dir` for `name`, so a paste never silently overwrites.
QString uniqueHostDestination(const QString &dir, const QString &name)
{
    const QFileInfo info(QDir(dir).absoluteFilePath(name));
    if (!info.exists())
        return info.absoluteFilePath();
    const QString suffix = info.suffix().isEmpty()
        ? QString()
        : QLatin1Char('.') + info.suffix();
    for (int n = 2;; ++n) {
        const QString candidate = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
            + QStringLiteral(" (%1)").arg(n) + suffix;
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

bool copyHostRecursively(const QString &source, const QString &target)
{
    const QFileInfo info(source);
    if (info.isDir()) {
        if (!QDir().mkpath(target))
            return false;
        const QDir dir(source);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink())
                continue;
            if (!copyHostRecursively(entry.absoluteFilePath(),
                                     QDir(target).absoluteFilePath(entry.fileName())))
                return false;
        }
        return true;
    }
    QFile::remove(target);
    return QFile::copy(source, target);
}

/// A QTreeView that reports drag activity back to FileBrowser. The hard-drive
/// pane needs drops handled here (not by QFileSystemModel) so moves can tell
/// open documents about their new path, and the floppy panes need both custom
/// drag mime and drops that write into a disk image.
class BrowserView : public QTreeView
{
public:
    using QTreeView::QTreeView;

    std::function<bool(const QPoint &, const QMimeData *)> acceptsDrop;
    std::function<void(const QPoint &, const QMimeData *, Qt::DropAction,
                       Qt::KeyboardModifiers)>
        dropped;
    /// When set, produces the drag's mime data instead of the model's.
    std::function<QMimeData *()> dragMime;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (acceptsDrop && acceptsDrop(event->position().toPoint(), event->mimeData()))
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (acceptsDrop && acceptsDrop(event->position().toPoint(), event->mimeData()))
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (dropped) {
            dropped(event->position().toPoint(), event->mimeData(), event->proposedAction(),
                    event->modifiers());
            event->acceptProposedAction();
        }
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        if (!dragMime) {
            QTreeView::startDrag(supportedActions);
            return;
        }
        QMimeData *mime = dragMime();
        if (!mime)
            return;
        QDrag drag(this);
        drag.setMimeData(mime);
        drag.exec(supportedActions, defaultDropAction());
    }
};

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

    auto *browserView = new BrowserView(hd);
    m_view = browserView;
    m_view->setObjectName(QStringLiteral("hardDriveView"));
    m_view->setModel(m_model);
    m_view->setHeaderHidden(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setDragEnabled(true);
    m_view->setAcceptDrops(true);
    m_view->setDragDropMode(QAbstractItemView::DragDrop);
    // A drag inside the hard-drive pane moves, like any file manager on one
    // volume; Ctrl drags copy.
    m_view->setDefaultDropAction(Qt::MoveAction);
    m_view->setDropIndicatorShown(true);
    browserView->acceptsDrop = [this](const QPoint &, const QMimeData *mime) {
        return !m_model->rootPath().isEmpty() && (mime->hasFormat(kFloppyMime) || mime->hasUrls());
    };
    browserView->dropped = [this](const QPoint &pos, const QMimeData *mime,
                                  Qt::DropAction action, Qt::KeyboardModifiers modifiers) {
        dropOnHardDrive(pos, mime, action, modifiers);
    };

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
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_view);
    connect(copyShortcut, &QShortcut::activated, this,
            [this] { copyHardDrivePaths(selectedHardDrivePaths(), false); });
    auto *cutShortcut = new QShortcut(QKeySequence::Cut, m_view);
    connect(cutShortcut, &QShortcut::activated, this,
            [this] { copyHardDrivePaths(selectedHardDrivePaths(), true); });
    auto *pasteShortcut = new QShortcut(QKeySequence::Paste, m_view);
    connect(pasteShortcut, &QShortcut::activated, this,
            [this] { pasteIntoDirectory(contextDirectory()); });
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
        auto *paneView = new BrowserView(group);
        pane.view = paneView;
        pane.view->setObjectName(drive == 0 ? QStringLiteral("diskAView")
                                            : QStringLiteral("diskBView"));
        pane.view->setModel(pane.model);
        pane.view->setHeaderHidden(true);
        pane.view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        pane.view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        pane.view->setAnimated(false);
        pane.view->setDragEnabled(true);
        pane.view->setAcceptDrops(true);
        pane.view->setDragDropMode(QAbstractItemView::DragDrop);
        // Dragging off a disk copies until Shift asks for a move.
        pane.view->setDefaultDropAction(Qt::CopyAction);
        pane.view->setDropIndicatorShown(true);
        pane.view->setProperty("drive", drive);
        const int driveIndex = drive;
        paneView->acceptsDrop = [this, driveIndex](const QPoint &, const QMimeData *mime) {
            return !m_floppyPath[driveIndex].isEmpty()
                && (mime->hasFormat(kFloppyMime) || mime->hasUrls());
        };
        paneView->dropped = [this, driveIndex](const QPoint &pos, const QMimeData *mime,
                                               Qt::DropAction action,
                                               Qt::KeyboardModifiers modifiers) {
            dropOnFloppy(driveIndex, pos, mime, action, modifiers);
        };
        paneView->dragMime = [this, driveIndex]() -> QMimeData * {
            return floppyDragMime(m_floppyPath[driveIndex], selectedFloppyEntries(driveIndex));
        };
        pane.view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(pane.view, &QTreeView::customContextMenuRequested, this,
                &FileBrowser::onFloppyContextMenu);
        connect(pane.view, &QTreeView::activated, this,
                [this, driveIndex](const QModelIndex &index) {
                    const QString entry = index.data(Qt::UserRole).toString();
                    if (!entry.isEmpty())
                        emit floppyEntryActivated(driveIndex, entry);
                });
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
    const QModelIndex rootIndex = m_model->index(root);
    m_view->setRootIndex(rootIndex);
    // Make the shown directory the current entry. Otherwise the context menu
    // and the clipboard keep acting on the previous folder's selection, which
    // is no longer visible — a "New File…" would land there, not here.
    m_view->setCurrentIndex(rootIndex);
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

QStringList FileBrowser::selectedFloppyEntries(int drive) const
{
    if (drive < 0 || drive > 1)
        return {};
    QStringList paths;
    const QModelIndexList indexes = m_floppy[drive].view->selectionModel()->selectedRows();
    for (const QModelIndex &index : indexes) {
        const QString path = index.data(Qt::UserRole).toString();
        if (!path.isEmpty())
            paths.append(path);
    }
    return paths;
}

void FileBrowser::copyHardDrivePaths(const QStringList &paths, bool cut)
{
    QStringList existing;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.exists())
            existing.append(info.absoluteFilePath());
    }
    if (existing.isEmpty()) {
        m_clipboard = Clipboard{};
        return;
    }
    m_clipboard.mode = cut ? Clipboard::Cut : Clipboard::Copy;
    m_clipboard.fromFloppy = false;
    m_clipboard.drive = -1;
    m_clipboard.paths = existing;
}

void FileBrowser::copyFloppyEntries(int drive, const QStringList &entryPaths, bool cut)
{
    QStringList clean;
    if (drive >= 0 && drive <= 1 && !m_floppyPath[drive].isEmpty()) {
        for (const QString &path : entryPaths) {
            const QString entry = normalizedEntryPath(path);
            if (!entry.isEmpty())
                clean.append(entry);
        }
    }
    if (clean.isEmpty()) {
        m_clipboard = Clipboard{};
        return;
    }
    m_clipboard.mode = cut ? Clipboard::Cut : Clipboard::Copy;
    m_clipboard.fromFloppy = true;
    m_clipboard.drive = drive;
    m_clipboard.paths = clean;
}

bool FileBrowser::pasteIntoDirectory(const QString &dir)
{
    if (m_clipboard.mode == Clipboard::None || !QFileInfo(dir).isDir())
        return false;
    bool ok = false;
    if (m_clipboard.fromFloppy)
        ok = extractFloppyEntries(m_clipboard.drive, m_clipboard.paths, dir,
                                  m_clipboard.mode == Clipboard::Cut);
    else
        ok = transferHostPaths(m_clipboard.paths, dir, m_clipboard.mode == Clipboard::Cut);
    if (ok && m_clipboard.mode == Clipboard::Cut)
        m_clipboard = Clipboard{};
    return ok;
}

bool FileBrowser::pasteIntoFloppy(int drive, const QString &dirInImage)
{
    if (m_clipboard.mode == Clipboard::None || drive < 0 || drive > 1
        || m_floppyPath[drive].isEmpty())
        return false;
    const QString dir = normalizedEntryPath(dirInImage);
    bool ok = false;
    if (m_clipboard.fromFloppy)
        ok = copyFloppyToFloppy(m_clipboard.drive, m_clipboard.paths, drive, dir,
                                m_clipboard.mode == Clipboard::Cut);
    else
        ok = addHostPathsToFloppy(drive, dir, m_clipboard.paths,
                                  m_clipboard.mode == Clipboard::Cut);
    if (ok && m_clipboard.mode == Clipboard::Cut)
        m_clipboard = Clipboard{};
    return ok;
}

QString FileBrowser::hardDriveTargetDirectory(const QPoint &pos) const
{
    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid())
        return m_model->rootPath();
    const QString path = m_model->filePath(index);
    if (QFileInfo(path).isDir())
        return path;
    return QFileInfo(path).absolutePath();
}

QString FileBrowser::floppyTargetDirectory(int drive, const QPoint &pos) const
{
    if (drive < 0 || drive > 1)
        return {};
    const QModelIndex index = m_floppy[drive].view->indexAt(pos);
    if (!index.isValid())
        return {};
    const QString path = index.data(Qt::UserRole).toString();
    if (path.isEmpty())
        return {};
    if (index.data(Qt::UserRole + 1).toBool())
        return path;
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

bool FileBrowser::transferHostPaths(const QStringList &paths, const QString &targetDir,
                                    bool move)
{
    if (paths.isEmpty() || !QFileInfo(targetDir).isDir())
        return false;

    QStringList failures;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        const QString source = info.absoluteFilePath();
        QString target = QDir(targetDir).absoluteFilePath(info.fileName());
        if (target == source)
            continue;
        if (info.isDir() && target.startsWith(source + QLatin1Char('/'))) {
            QMessageBox::warning(this, tr("Move"),
                                 tr("Cannot put %1 inside itself.").arg(info.fileName()));
            return false;
        }
        if (QFileInfo::exists(target))
            target = uniqueHostDestination(targetDir, info.fileName());

        bool done = false;
        if (move) {
            if (QFile::rename(source, target)) {
                emit pathRenamed(source, target);
                done = true;
            } else if (copyHostRecursively(source, target)) {
                // Renaming across filesystems fails; copy and delete instead.
                deletePath(source);
                done = true;
            }
        } else {
            done = copyHostRecursively(source, target);
        }
        if (!done)
            failures.append(info.fileName());
    }
    if (!failures.isEmpty()) {
        QMessageBox::warning(this, move ? tr("Move") : tr("Copy"),
                             move ? tr("Could not move %1 to %2.")
                                        .arg(failures.join(tr(", ")), targetDir)
                                  : tr("Could not copy %1 to %2.")
                                        .arg(failures.join(tr(", ")), targetDir));
        return false;
    }
    return true;
}

bool FileBrowser::confirmFloppyRewrite(const QString &imagePath)
{
    QByteArray raw;
    QString error;
    if (!floppy::loadRaw(imagePath, &raw, &error))
        return true;   // unreadable: let the write itself report it
    if (floppy::looksLikeCanonical720k(raw.left(512), raw.size()))
        return true;

    const QString name = QFileInfo(imagePath).fileName();
    const QString oversize = raw.size() > 720 * 1024
        ? tr("Everything past 720 KiB is discarded, and ")
        : QString();
    return QMessageBox::warning(this, tr("Rewrite this disk?"),
                                tr("Writing to %1 rebuilds the whole image with PiST's "
                                   "720 KiB FAT12 layout: %2its boot sector is replaced. A disk "
                                   "that is not already that shape loses whatever made it "
                                   "different.\n\nContinue?")
                                    .arg(name, oversize),
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
           == QMessageBox::Yes;
}

bool FileBrowser::addHostPathsToFloppy(int drive, const QString &dirInImage,
                                       const QStringList &hostPaths, bool removeSources)
{
    if (drive < 0 || drive > 1 || hostPaths.isEmpty() || m_floppyPath[drive].isEmpty())
        return false;
    const QString image = m_floppyPath[drive];

    QVector<floppy::Item> additions;
    QString error;
    for (const QString &path : hostPaths) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        // Collect relative to the file's parent, so the entry keeps its own
        // name at the top of the copied set.
        QVector<floppy::Item> collected;
        if (!floppy::collectHostItems(info.absolutePath(), {info.absoluteFilePath()},
                                      &collected, &error)) {
            QMessageBox::warning(this, tr("Copy"),
                                 tr("Could not read %1:\n%2").arg(info.fileName(), error));
            return false;
        }
        for (floppy::Item &item : collected) {
            item.destPath = dirInImage.isEmpty()
                ? item.destPath
                : dirInImage + QLatin1Char('/') + item.destPath;
        }
        additions += collected;
    }
    if (additions.isEmpty())
        return false;

    if (!confirmFloppyRewrite(image))
        return false;
    if (!floppy::updateImage(image, additions, {}, &error)) {
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not add the files to %1:\n%2")
                                 .arg(QFileInfo(image).fileName(), error));
        return false;
    }
    refreshFloppy(drive);
    if (removeSources) {
        for (const QString &path : hostPaths) {
            if (QFileInfo::exists(path))
                deletePath(path);
        }
    }
    return true;
}

bool FileBrowser::extractFloppyEntries(int drive, const QStringList &entryPaths,
                                       const QString &targetDir, bool removeSource)
{
    if (drive < 0 || drive > 1 || entryPaths.isEmpty() || !QFileInfo(targetDir).isDir())
        return false;
    const QString image = m_floppyPath[drive];
    if (image.isEmpty())
        return false;
    // A move rewrites the image; a plain copy-out does not.
    if (removeSource && !confirmFloppyRewrite(image))
        return false;

    QString error;
    QByteArray raw;
    if (!floppy::loadRaw(image, &raw, &error)) {
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not read %1:\n%2")
                                 .arg(QFileInfo(image).fileName(), error));
        return false;
    }
    QString listError;
    const QVector<floppy::Entry> entries = floppy::listRaw(raw, &listError);
    if (!listError.isEmpty()) {
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not read %1:\n%2")
                                 .arg(QFileInfo(image).fileName(), listError));
        return false;
    }

    QStringList failures;
    QStringList removals;
    for (const QString &chosen : entryPaths) {
        const QString entry = normalizedEntryPath(chosen);
        const QString base = entry.section(QLatin1Char('/'), -1);
        const QString target = uniqueHostDestination(targetDir, base);
        bool found = false;
        bool ok = true;
        for (const floppy::Entry &e : entries) {
            const bool top = e.path.compare(entry, Qt::CaseInsensitive) == 0;
            const bool under = !top
                && e.path.startsWith(entry + QLatin1Char('/'), Qt::CaseInsensitive);
            if (!top && !under)
                continue;
            found = true;
            if (removeSource)
                removals.append(e.path);
            const QString destination = top
                ? target
                : QDir(target).absoluteFilePath(e.path.mid(entry.size() + 1));
            // Defence in depth against crafted images: an extraction must
            // land inside the folder the user chose. The listing sanitises
            // entry names, so this can only fire on something that slipped
            // through anyway — refuse it loudly rather than write outside.
            if (!QDir::cleanPath(destination)
                     .startsWith(QDir::cleanPath(targetDir) + QLatin1Char('/'))) {
                ok = false;
                error = tr("%1 escapes the destination folder.").arg(e.path);
                break;
            }
            bool written = false;
            if (e.isDirectory) {
                written = QDir().mkpath(destination);
            } else {
                QByteArray data;
                if (floppy::readFileRaw(raw, e.path, &data, &error)) {
                    QFile out(destination);
                    written = out.open(QIODevice::WriteOnly)
                        && out.write(data) == data.size();
                }
            }
            if (!written) {
                ok = false;
                break;
            }
        }
        if (!found)
            error = tr("%1 is not on the disk.").arg(base);
        if (!found || !ok)
            failures.append(base);
    }

    if (!failures.isEmpty()) {
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not copy %1 from %2:\n%3")
                                 .arg(failures.join(tr(", ")), QFileInfo(image).fileName(),
                                      error));
        return false;
    }

    if (removeSource && !removals.isEmpty()) {
        if (!floppy::updateImage(image, {}, removals, &error)) {
            QMessageBox::warning(this, tr("Move"),
                                 tr("The files were copied, but could not be removed from "
                                    "%1:\n%2").arg(QFileInfo(image).fileName(), error));
            refreshFloppy(drive);
            return false;
        }
        refreshFloppy(drive);
    }
    return true;
}

bool FileBrowser::copyFloppyToFloppy(int sourceDrive, const QStringList &entryPaths,
                                     int targetDrive, const QString &dirInImage,
                                     bool removeSource)
{
    if (sourceDrive < 0 || sourceDrive > 1 || targetDrive < 0 || targetDrive > 1
        || entryPaths.isEmpty())
        return false;
    const QString sourceImage = m_floppyPath[sourceDrive];
    const QString targetImage = m_floppyPath[targetDrive];
    if (sourceImage.isEmpty() || targetImage.isEmpty())
        return false;
    const QString dir = normalizedEntryPath(dirInImage);
    const bool sameImage = QFileInfo(sourceImage).absoluteFilePath()
        == QFileInfo(targetImage).absoluteFilePath();
    // Every write below rebuilds an image: the target always, and the source
    // too when this is a move between two different disks.
    if (!confirmFloppyRewrite(targetImage))
        return false;
    if (!sameImage && removeSource && !confirmFloppyRewrite(sourceImage))
        return false;

    for (const QString &chosen : entryPaths) {
        const QString entry = normalizedEntryPath(chosen);
        if (sameImage
            && (dir == entry || dir.startsWith(entry + QLatin1Char('/')))) {
            QMessageBox::warning(this, tr("Paste"),
                                 tr("Cannot put %1 inside itself.").arg(entry));
            return false;
        }
    }

    QString error;
    QByteArray raw;
    if (!floppy::loadRaw(sourceImage, &raw, &error)) {
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not read %1:\n%2")
                                 .arg(QFileInfo(sourceImage).fileName(), error));
        return false;
    }
    const QVector<floppy::Entry> entries = floppy::listRaw(raw, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not read %1:\n%2")
                                 .arg(QFileInfo(sourceImage).fileName(), error));
        return false;
    }

    QVector<floppy::Item> additions;
    QStringList removals;
    for (const QString &chosen : entryPaths) {
        const QString entry = normalizedEntryPath(chosen);
        for (const floppy::Entry &e : entries) {
            const bool top = e.path.compare(entry, Qt::CaseInsensitive) == 0;
            const bool under = !top
                && e.path.startsWith(entry + QLatin1Char('/'), Qt::CaseInsensitive);
            if (!top && !under)
                continue;
            if (removeSource)
                removals.append(e.path);
            floppy::Item item;
            item.destPath = dir.isEmpty() ? e.path : dir + QLatin1Char('/') + e.path;
            item.isDirectory = e.isDirectory;
            if (!e.isDirectory && !floppy::readFileRaw(raw, e.path, &item.data, &error)) {
                QMessageBox::warning(this, tr("Copy"),
                                     tr("Could not read %1:\n%2")
                                         .arg(QFileInfo(sourceImage).fileName(), error));
                return false;
            }
            additions.append(item);
        }
    }
    if (additions.isEmpty()) {
        QMessageBox::warning(this, tr("Copy"), tr("Nothing to copy was found on the disk."));
        return false;
    }

    if (sameImage) {
        if (!floppy::updateImage(targetImage, additions, removals, &error)) {
            QMessageBox::warning(this, tr("Copy"),
                                 tr("Could not write %1:\n%2")
                                     .arg(QFileInfo(targetImage).fileName(), error));
            return false;
        }
    } else {
        if (!floppy::updateImage(targetImage, additions, {}, &error)) {
            QMessageBox::warning(this, tr("Copy"),
                                 tr("Could not write %1:\n%2")
                                     .arg(QFileInfo(targetImage).fileName(), error));
            return false;
        }
        if (removeSource && !removals.isEmpty()) {
            if (!floppy::updateImage(sourceImage, {}, removals, &error)) {
                QMessageBox::warning(this, tr("Move"),
                                     tr("The files were copied, but could not be removed "
                                        "from %1:\n%2")
                                         .arg(QFileInfo(sourceImage).fileName(), error));
                refreshFloppy(sourceDrive);
                refreshFloppy(targetDrive);
                return false;
            }
        }
    }
    refreshFloppy(targetDrive);
    if (removeSource && !sameImage)
        refreshFloppy(sourceDrive);
    return true;
}

void FileBrowser::dropOnHardDrive(const QPoint &pos, const QMimeData *mime,
                                  Qt::DropAction action, Qt::KeyboardModifiers modifiers)
{
    const QString targetDir = hardDriveTargetDirectory(pos);
    if (targetDir.isEmpty())
        return;

    QString sourceImage;
    QStringList entries;
    if (floppyDragPayload(mime, &sourceImage, &entries)) {
        int drive = -1;
        for (int d = 0; d < 2; ++d) {
            if (QFileInfo(m_floppyPath[d]).absoluteFilePath()
                == QFileInfo(sourceImage).absoluteFilePath())
                drive = d;
        }
        if (drive < 0)
            return;
        extractFloppyEntries(drive, entries, targetDir,
                             action == Qt::MoveAction && (modifiers & Qt::ShiftModifier));
        return;
    }

    QStringList paths;
    for (const QUrl &url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty())
            paths.append(path);
    }
    // A drag inside the hard-drive pane proposes a move; drags arriving from
    // elsewhere propose a copy unless Shift asks for a move.
    if (!paths.isEmpty())
        transferHostPaths(paths, targetDir, action == Qt::MoveAction);
}

void FileBrowser::dropOnFloppy(int drive, const QPoint &pos, const QMimeData *mime,
                               Qt::DropAction action, Qt::KeyboardModifiers modifiers)
{
    if (drive < 0 || drive > 1 || m_floppyPath[drive].isEmpty())
        return;
    const QString targetDir = floppyTargetDirectory(drive, pos);

    QString sourceImage;
    QStringList entries;
    if (floppyDragPayload(mime, &sourceImage, &entries)) {
        int sourceDrive = -1;
        for (int d = 0; d < 2; ++d) {
            if (QFileInfo(m_floppyPath[d]).absoluteFilePath()
                == QFileInfo(sourceImage).absoluteFilePath())
                sourceDrive = d;
        }
        if (sourceDrive < 0)
            return;
        copyFloppyToFloppy(sourceDrive, entries, drive, targetDir,
                           action == Qt::MoveAction && (modifiers & Qt::ShiftModifier));
        return;
    }

    QStringList paths;
    for (const QUrl &url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty())
            paths.append(path);
    }
    if (!paths.isEmpty()) {
        // Only an explicit shift-drag removes the host originals; a plain
        // drop onto a disk always copies.
        addHostPathsToFloppy(drive, targetDir, paths,
                             action == Qt::MoveAction && (modifiers & Qt::ShiftModifier));
    }
}

void FileBrowser::onFloppyContextMenu(const QPoint &pos)
{
    const int drive = driveOfSender();
    if (drive < 0)
        return;
    FloppyPane &pane = m_floppy[drive];
    const QStringList selected = selectedFloppyEntries(drive);
    const bool hasDisk = !m_floppyPath[drive].isEmpty();

    QMenu menu(this);
    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    QAction *cutAction = menu.addAction(tr("Cut"));
    cutAction->setShortcut(QKeySequence::Cut);
    QAction *pasteAction = menu.addAction(tr("Paste"));
    pasteAction->setShortcut(QKeySequence::Paste);
    copyAction->setEnabled(hasDisk && !selected.isEmpty());
    cutAction->setEnabled(hasDisk && !selected.isEmpty());
    pasteAction->setEnabled(hasDisk && m_clipboard.mode != Clipboard::None);

    QAction *chosen = menu.exec(pane.view->viewport()->mapToGlobal(pos));
    if (chosen == copyAction)
        copyFloppyEntries(drive, selected, false);
    else if (chosen == cutAction)
        copyFloppyEntries(drive, selected, true);
    else if (chosen == pasteAction)
        pasteIntoFloppy(drive, floppyTargetDirectory(drive, pos));
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
    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    QAction *cutAction = menu.addAction(tr("Cut"));
    cutAction->setShortcut(QKeySequence::Cut);
    QAction *pasteAction = menu.addAction(tr("Paste"));
    pasteAction->setShortcut(QKeySequence::Paste);
    menu.addSeparator();
    QAction *exportFloppy = menu.addAction(tr("Export to Floppy Image…"));
    exportFloppy->setEnabled(!selectedHardDrivePaths().isEmpty() || index.isValid());
    menu.addSeparator();
    QAction *rename = menu.addAction(tr("Rename…"));
    QAction *remove = menu.addAction(tr("Delete…"));
    rename->setEnabled(index.isValid());
    remove->setEnabled(index.isValid());
    copyAction->setEnabled(index.isValid());
    cutAction->setEnabled(index.isValid());
    pasteAction->setEnabled(m_clipboard.mode != Clipboard::None
                            && QFileInfo(contextDirectory()).isDir());

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
    } else if (chosen == copyAction) {
        copyHardDrivePaths(selectedHardDrivePaths(), false);
    } else if (chosen == cutAction) {
        copyHardDrivePaths(selectedHardDrivePaths(), true);
    } else if (chosen == pasteAction) {
        pasteIntoDirectory(contextDirectory());
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
        // Drops and the clipboard need the entry's path in the image and
        // whether it names a folder.
        item->setData(dirPath, Qt::UserRole);
        item->setData(true, Qt::UserRole + 1);
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
        item->setData(entry.path, Qt::UserRole);
        item->setData(false, Qt::UserRole + 1);
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
