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
#include <QStorageInfo>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace pist {

namespace {

// Carries a drag out of a floppy pane: the source image path, then one
// image-relative entry path per line.
const QString kFloppyMime = QStringLiteral("application/x-pist-floppy-entries");

/// Whether `source` and `targetDir` sit on different mounted volumes, i.e.
/// whether QFile::rename cannot be a plain rename(2). Measured on Qt 6.10.2:
/// a *directory* rename across volumes fails (so the caller's fallback ran
/// anyway), while a *file* rename performs Qt's own copy+delete internally —
/// a path whose symlink and permission semantics would be Qt's, not the
/// fallback's (MIN-76). Comparing roots rather than device ids makes an
/// unrecognised volume report "different" and take the safe path.
bool crossesFilesystem(const QString &source, const QString &targetDir)
{
    const QStorageInfo from(QFileInfo(source).absolutePath());
    const QStorageInfo to(QFileInfo(targetDir).absolutePath());
    return from.rootPath() != to.rootPath();
}

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

/// Copy `source` (file, directory, or symbolic link) to `target`.
///
/// A symbolic link is materialised as the regular file it resolves to; a link
/// to a directory and a dangling link have no file content to reproduce, so
/// they fail the copy instead of being skipped. Skipping is what made the
/// cross-filesystem move a data-loss path: the copy reported success without
/// the link, and the caller then deleted the source tree — link included.
bool copyHostRecursively(const QString &source, const QString &target)
{
    const QFileInfo info(source);
    if (info.isSymLink()) {
        const QFileInfo resolved(info.canonicalFilePath());
        if (!resolved.isFile())
            return false;
        QFile::remove(target);
        return QFile::copy(resolved.absoluteFilePath(), target);
    }
    if (info.isDir()) {
        if (!QDir().mkpath(target))
            return false;
        const QDir dir(source);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &entry : entries) {
            if (!copyHostRecursively(entry.absoluteFilePath(),
                                     QDir(target).absoluteFilePath(entry.fileName())))
                return false;
        }
        return true;
    }
    if (!info.isFile())
        return false;
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

QString floppyImageFilter()
{
    // The literal lives inside tr() so lupdate extracts it — the old
    // `tr(kFloppyFilter)` passed a runtime const char[] and was invisible.
    return FileBrowser::tr("Disk images (*.st *.msa *.img *.dim *.ipf);;All files (*)");
}

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
    QWidget *hd = makeGroup(tr("Project"), this, &hdHeader);
    m_projectTitle = qobject_cast<QLabel *>(hdHeader->itemAt(0)->widget());
    if (m_projectTitle)
        m_projectTitle->setObjectName(QStringLiteral("projectTitle"));
    hdHeader->addStretch();

    m_pathEdit = new QLineEdit(hd);
    m_pathEdit->setObjectName(QStringLiteral("hardDrivePath"));
    m_pathEdit->setPlaceholderText(tr("Project directory"));
    m_pathEdit->setToolTip(tr("Press Enter to browse this directory."));
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &FileBrowser::onPathEntered);
    auto *browse = new QPushButton(tr("Browse…"), hd);
    browse->setObjectName(QStringLiteral("hardDriveBrowse"));
    browse->setToolTip(tr("Choose the project directory with a folder picker."));
    compactButton(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(
            this, tr("Choose Project Directory"),
            // rootPath() defaults to ".", so "no root yet" is m_rootChosen.
            m_rootChosen ? m_model->rootPath() : QDir::homePath());
        if (!chosen.isEmpty())
            showDirectory(chosen);
    });
    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(browse);
    hd->layout()->addItem(pathRow);

    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    // Read-write, so renames through the inline editor land on disk. Deletion
    // and creation go through the slots below either way.
    m_model->setReadOnly(false);
    connect(m_model, &QFileSystemModel::fileRenamed, this,
            &FileBrowser::onModelPathRenamed);
    // The model populates in a thread: a root or reveal target asked for
    // before its directory lands is an invalid index, so both are repaired
    // here, when the load finishes.
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this] {
        const QModelIndex rootIndex = m_model->index(m_model->rootPath());
        if (rootIndex.isValid() && m_view->rootIndex() != rootIndex)
            m_view->setRootIndex(rootIndex);
        if (!m_pendingCurrent.isEmpty()) {
            const QModelIndex current = m_model->index(m_pendingCurrent);
            if (current.isValid()) {
                m_pendingCurrent.clear();
                m_view->setCurrentIndex(current);
            }
        }
        if (!m_pendingReveal.isEmpty())
            reveal(m_pendingReveal);
    });

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

    // The root is the user's chosen project directory: it moves only when they
    // choose another (the path field or Browse…), never because a file was
    // opened or focused. The first open seeds it, so a fresh window still
    // lands somewhere useful. (The model's default rootPath is ".", so the
    // choice is tracked explicitly rather than read back from it.)
    const QString absolute = info.absoluteFilePath();
    if (!m_rootChosen)
        showDirectory(info.absolutePath());

    // Outside the shown project: leave the browser where the user put it.
    if (QDir(m_model->rootPath()).relativeFilePath(absolute).startsWith(QStringLiteral("..")))
        return;

    reveal(absolute);
}

void FileBrowser::reveal(const QString &path)
{
    const QModelIndex index = m_model->index(path);
    if (!index.isValid()) {
        // Not loaded yet; directoryLoaded retries.
        m_pendingReveal = path;
        return;
    }
    m_pendingReveal.clear();
    // A revealed file is the selection; a parked "select the new root" from
    // an earlier showDirectory must not clobber it when the load lands.
    m_pendingCurrent.clear();
    // Inside the shown project: reveal the file — expand the chain down to
    // it and select it — without moving the root.
    for (QModelIndex p = index.parent(); p.isValid(); p = p.parent())
        m_view->expand(p);
    m_view->setCurrentIndex(index);
    m_view->scrollTo(index, QAbstractItemView::PositionAtCenter);
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
    m_rootChosen = true;
    // An explicit root change cancels any reveal still waiting on the old one.
    m_pendingReveal.clear();
    const QModelIndex rootIndex = m_model->index(root);
    m_view->setRootIndex(rootIndex);
    // Make the shown directory the current entry. Otherwise the context menu
    // and the clipboard keep acting on the previous folder's selection, which
    // is no longer visible — a "New File…" would land there, not here. The
    // index is invalid until the model's thread has read the directory, so
    // park it for directoryLoaded in that case.
    if (rootIndex.isValid())
        m_view->setCurrentIndex(rootIndex);
    else
        m_pendingCurrent = root;
    m_pathEdit->setText(root);
    m_pathEdit->setToolTip(root);
    if (m_projectTitle) {
        const QString name = QFileInfo(root).fileName();
        m_projectTitle->setText(name.isEmpty() ? root : name);
        m_projectTitle->setToolTip(root);
    }
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

    // Remove first: pathDeleted says the path is gone, so reporting it for a
    // removal that failed closes or disowns an open document whose file is
    // still on disk. The receiver acts on the path, not on the model entry.
    const bool removed = info.isDir() ? QDir(path).removeRecursively()
                                      : QFile::remove(path);
    if (removed)
        emit pathDeleted(info.absoluteFilePath());
    return removed;
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
            const QString entry = floppy::normalizeEntryPath(path);
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
    m_clipboard.imagePath = m_floppyPath[drive];
    m_clipboard.paths = clean;
}

bool FileBrowser::clipboardUsable()
{
    if (m_clipboard.mode == Clipboard::None)
        return false;
    bool usable = false;
    if (m_clipboard.fromFloppy) {
        // Same drive, same image: an entry path copied off the disk that was
        // mounted then means nothing once that disk is ejected or another one
        // is inserted in its place.
        usable = m_clipboard.drive >= 0 && m_clipboard.drive <= 1
            && m_floppyPath[m_clipboard.drive] == m_clipboard.imagePath
            && !m_clipboard.imagePath.isEmpty();
    } else {
        for (const QString &path : m_clipboard.paths) {
            if (QFileInfo::exists(path)) {
                usable = true;
                break;
            }
        }
    }
    if (!usable)
        m_clipboard = Clipboard{};
    return usable;
}

bool FileBrowser::pasteIntoDirectory(const QString &dir)
{
    if (!clipboardUsable() || !QFileInfo(dir).isDir())
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
    if (!clipboardUsable() || drive < 0 || drive > 1
        || m_floppyPath[drive].isEmpty())
        return false;
    // The controller normalises the destination directory itself: one rule
    // for how an entry path is spelled, wherever it came from.
    bool ok = false;
    if (m_clipboard.fromFloppy)
        ok = copyFloppyToFloppy(m_clipboard.drive, m_clipboard.paths, drive, dirInImage,
                                m_clipboard.mode == Clipboard::Cut);
    else
        ok = addHostPathsToFloppy(drive, dirInImage, m_clipboard.paths,
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
            target = floppy::Transfer::uniqueHostDestination(targetDir, info.fileName());

        bool done = false;
        if (move) {
            // Only a move within one volume is a rename(2). Across volumes the
            // copy+delete fallback below runs explicitly, so a nested symlink
            // is materialised by copyHostRecursively on every Qt and every
            // filesystem instead of by whatever QFile::rename does internally.
            const bool sameVolume = !crossesFilesystem(source, targetDir);
            if (sameVolume && QFile::rename(source, target)) {
                emit pathRenamed(source, target);
                done = true;
            } else if (copyHostRecursively(source, target)) {
                // The source goes only after a copy that reproduced every
                // entry, and the removal is not reported as pathDeleted: the
                // document must follow the file to its new home with
                // pathRenamed, not be closed as if the file had vanished.
                const bool removed = info.isDir() ? QDir(source).removeRecursively()
                                                  : QFile::remove(source);
                if (removed) {
                    emit pathRenamed(source, target);
                    done = true;
                }
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

QString FileBrowser::FloppyHost::mountedImage(int drive) const
{
    if (drive < 0 || drive > 1)
        return {};
    return m_browser->m_floppyPath[drive];
}

bool FileBrowser::FloppyHost::confirmRewrite(const QString &imagePath)
{
    return m_browser->confirmFloppyRewrite(imagePath);
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
    const floppy::Transfer::Result result = m_transfer.hostToImage(drive, dirInImage, hostPaths);
    finishTransfer(result);
    if (!result.ok())
        return false;
    // The move's host half: the additions are on the disk, so the sources go —
    // through deletePath, so that the rest of the application hears the path is
    // really gone (an open document closes rather than editing a ghost).
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
    const floppy::Transfer::Result result =
        m_transfer.imageToHost(drive, entryPaths, targetDir, removeSource);
    finishTransfer(result);
    return result.ok();
}

bool FileBrowser::copyFloppyToFloppy(int sourceDrive, const QStringList &entryPaths,
                                     int targetDrive, const QString &dirInImage,
                                     bool removeSource)
{
    const floppy::Transfer::Result result =
        m_transfer.imageToImage(sourceDrive, entryPaths, targetDrive, dirInImage, removeSource);
    finishTransfer(result);
    return result.ok();
}

void FileBrowser::finishTransfer(const floppy::Transfer::Result &result)
{
    reportTransferFailure(result);
    for (int drive : result.refresh)
        refreshFloppy(drive);
}

void FileBrowser::reportTransferFailure(const floppy::Transfer::Result &result)
{
    // The disk or file the message names, as the controller reported it.
    const QString subject = QFileInfo(result.subject).fileName();
    switch (result.outcome) {
    case floppy::Transfer::Completed:
    case floppy::Transfer::Declined:
    case floppy::Transfer::Resynced:
    case floppy::Transfer::Invalid:
        return;   // nothing failed, or nothing was asked: no message
    case floppy::Transfer::Unreadable:
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not read %1:\n%2").arg(subject, result.error));
        return;
    case floppy::Transfer::CopyFailed: {
        // The controller reports the cause of a copy-out failure rather than a
        // sentence, so the two it can name are phrased here.
        QString reason = result.error;
        if (result.cause == floppy::Transfer::EntryMissing)
            reason = tr("%1 is not on the disk.").arg(result.entry);
        else if (result.cause == floppy::Transfer::EntryEscapes)
            reason = tr("%1 escapes the destination folder.").arg(result.entry);
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not copy %1 from %2:\n%3")
                                 .arg(result.names.join(tr(", ")), subject, reason));
        return;
    }
    case floppy::Transfer::AddFailed:
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not add the files to %1:\n%2")
                                 .arg(subject, result.error));
        return;
    case floppy::Transfer::WriteFailed:
        QMessageBox::warning(this, tr("Copy"),
                             tr("Could not write %1:\n%2").arg(subject, result.error));
        return;
    case floppy::Transfer::SourceRemovalFailed:
        QMessageBox::warning(this, tr("Move"),
                             tr("The files were copied, but could not be removed from "
                                "%1:\n%2").arg(subject, result.error));
        return;
    case floppy::Transfer::DestinationInsideSource:
        // The subject is an entry path inside the image, not a host file: it is
        // the path the user is looking at, so it is shown whole.
        QMessageBox::warning(this, tr("Paste"),
                             tr("Cannot put %1 inside itself.").arg(result.subject));
        return;
    case floppy::Transfer::NothingToCopy:
        QMessageBox::warning(this, tr("Copy"), tr("Nothing to copy was found on the disk."));
        return;
    }
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
    pasteAction->setEnabled(hasDisk && clipboardUsable());

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
    pasteAction->setEnabled(clipboardUsable()
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
                                                      floppyImageFilter());
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

    const QString letter = drive == 0 ? QStringLiteral("A") : QStringLiteral("B");
    // An empty drive is one row. The listing opens when an image is inserted.
    pane.view->setVisible(!path.isEmpty());
    pane.eject->setVisible(!path.isEmpty());
    if (QWidget *group = pane.view->parentWidget()) {
        if (path.isEmpty())
            group->setMaximumHeight(group->minimumSizeHint().height());
        else
            group->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    if (path.isEmpty()) {
        pane.diskName->setText(tr("%1: no disk").arg(letter));
        pane.diskName->setToolTip(QString());
        pane.diskName->setEnabled(false);
        return;
    }

    const QFileInfo info(path);
    pane.diskName->setText(tr("%1: %2").arg(letter, info.fileName()));
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
