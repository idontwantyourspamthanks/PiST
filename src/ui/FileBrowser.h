// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

#include <QStringList>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QMimeData;
class QPushButton;
class QStandardItemModel;
class QTreeView;

namespace pist {

/// Browsable view of the project's host directory (the GEMDOS hard drive)
/// plus the floppy images mounted as A: and B:.
///
/// The hard-drive pane is scoped to the directory of the open source file
/// rather than the whole filesystem: the useful set of files is the
/// project's own, and an unbounded tree makes it harder to find them. A ".."
/// style escape is deliberately not offered — the location can be set
/// directly in the path field.
///
/// Disk A/B list the FAT12 contents of the inserted `.st` / `.msa` image.
/// Changing or ejecting a disk updates the project settings (the same paths
/// the emulator settings page stores); live insertion is the MainWindow's job.
class FileBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit FileBrowser(QWidget *parent = nullptr);

public slots:
    /// Reveal `sourcePath` in the tree, expanding and selecting it — without
    /// moving the project root. The root changes only through showDirectory
    /// (the path field or the Browse… button); the first showFor on an empty
    /// browser seeds it with the file's directory.
    void showFor(const QString &sourcePath);

    /// Choose an explicit project directory: the only way the root moves.
    void showDirectory(const QString &path);

    /// The directory currently shown, or empty before the first show.
    QString directory() const;

    /// Inserted floppy images, index 0 => drive A. Empty string = ejected.
    void setFloppyImages(const QStringList &images);
    QStringList floppyImages() const;

    /// Re-read the mounted images into their panes, after their contents
    /// changed on disk behind the browser's back (e.g. a document edited out
    /// of a disk was saved back into it).
    void refreshFloppyImages()
    {
        refreshFloppy(0);
        refreshFloppy(1);
    }

    /// Host paths selected in the hard-drive tree (files and folders).
    QStringList selectedHardDrivePaths() const;

    /// Entries selected in floppy pane `drive`, as image-relative paths like
    /// `listImage` reports them (`AUTO/PROG.PRG`).
    QStringList selectedFloppyEntries(int drive) const;

    /// Write the current hard-drive selection to a `.st` or `.msa` image.
    bool exportHardDriveSelection(const QString &imagePath, QString *error);

signals:
    /// A file was activated (double-clicked or Enter). The receiver decides
    /// what to do with the path.
    void fileActivated(const QString &path);

    /// An entry inside a mounted floppy image was activated (double-clicked
    /// or Enter). `drive` is 0 (A:) or 1 (B:); `entryPath` is image-relative,
    /// as `listImage` reports it (`AUTO/PROG.PRG`).
    void floppyEntryActivated(int drive, const QString &entryPath);

    /// The user asked to create a new `.pim` image in `directory`.
    void newImageRequested(const QString &directory);

    /// A file or directory was renamed through the browser (the model also
    /// emits its own fileRenamed; this one covers renames we initiate).
    void pathRenamed(const QString &oldPath, const QString &newPath);

    /// A file or directory is about to be deleted through the browser, so an
    /// open document on it can be dealt with before it vanishes.
    void pathDeleted(const QString &path);

    /// Drive 0 is A:, drive 1 is B:. An empty path means the drive was ejected.
    void floppyImageChanged(int drive, const QString &path);

public slots:
    /// Create an empty file in `dir`. Returns the new path, or empty on
    /// failure (already exists, or the name is not a plain file name).
    QString createFile(const QString &dir, const QString &name);

    /// Create a folder in `dir`. Returns the new path, or empty on failure.
    QString createFolder(const QString &dir, const QString &name);

    /// Delete `path`: a file, or a directory. A non-empty directory is
    /// removed recursively. Returns true on success.
    bool deletePath(const QString &path);

    /// Rename `path` to `newName` (a plain name, in the same directory).
    /// Returns true on success. This is the same path the inline editor takes.
    bool renamePath(const QString &path, const QString &newName);

    /// Copy (or cut, to move) hard-drive paths into the browser's clipboard
    /// for pasting into a hard-drive directory or onto a floppy pane.
    void copyHardDrivePaths(const QStringList &paths, bool cut);

    /// Copy (or cut) entries from floppy `drive` — image-relative paths as
    /// `selectedFloppyEntries` reports them — into the browser's clipboard.
    void copyFloppyEntries(int drive, const QStringList &entryPaths, bool cut);

    /// Paste the clipboard into hard-drive directory `dir`. Returns true on
    /// success; failures are reported with a message box.
    bool pasteIntoDirectory(const QString &dir);

    /// Paste the clipboard into floppy `drive`, inside the image directory
    /// `dirInImage` (empty for the image root). Returns true on success.
    bool pasteIntoFloppy(int drive, const QString &dirInImage);

    /// Ask before an operation that rewrites `imagePath`: every write goes
    /// through floppy::updateImage, which rebuilds the image with PiST's
    /// canonical 720 KiB layout. A disk that is not already that shape loses
    /// its boot sector and, if it is larger, everything past 720 KiB — which
    /// is documented in the writer but invisible to the user unless something
    /// says so. Returns false when the user declines. Public because
    /// MainWindow's write-back-on-save path rewrites an image too, and the
    /// policy belongs in one place.
    bool confirmFloppyRewrite(const QString &imagePath);

private slots:
    void onPathEntered();
    void onActivated(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);
    void onFloppyContextMenu(const QPoint &pos);
    void onModelPathRenamed(const QString &path, const QString &oldName, const QString &newName);
    void onExportFloppy();
    void onChangeFloppy();
    void onEjectFloppy();

private:
    /// The clipboard behind the Copy/Cut/Paste actions. It is browser-wide,
    /// not the system clipboard, because a paste can cross panes and needs to
    /// know whether its entries are host paths or floppy image paths.
    struct Clipboard {
        enum Mode { None, Copy, Cut };
        Mode mode = None;
        bool fromFloppy = false;   // paths are image-relative entry paths
        int drive = -1;            // source drive when fromFloppy
        QStringList paths;
    };

    /// The directory context menu actions apply to: the selected directory,
    /// the selected file's parent, or the root when nothing is selected.
    QString contextDirectory() const;

    /// The directory a drop or paste onto the hard-drive pane at `pos`
    /// targets: the directory under the cursor, else its parent.
    QString hardDriveTargetDirectory(const QPoint &pos) const;

    /// The same for a floppy pane: an image directory path, empty for the
    /// image root.
    QString floppyTargetDirectory(int drive, const QPoint &pos) const;

    /// Shared by the Delete shortcut and the context menu: confirm, then
    /// deletePath. Returns true when the path is gone.
    bool confirmAndDelete(const QString &path);

    /// Host → host: copy or move `paths` into `targetDir`.
    bool transferHostPaths(const QStringList &paths, const QString &targetDir, bool move);

    /// Host → floppy: add `hostPaths` inside image directory `dirInImage`;
    /// when `removeSources`, delete the host files afterwards (a move).
    bool addHostPathsToFloppy(int drive, const QString &dirInImage,
                              const QStringList &hostPaths, bool removeSources);

    /// Floppy → host: extract `entryPaths` into `targetDir`; when
    /// `removeSource`, also drop them from the image (a move).
    bool extractFloppyEntries(int drive, const QStringList &entryPaths,
                              const QString &targetDir, bool removeSource);

    /// Floppy → floppy, possibly the same image (copying within a disk).
    bool copyFloppyToFloppy(int sourceDrive, const QStringList &entryPaths,
                            int targetDrive, const QString &dirInImage, bool removeSource);

    void dropOnHardDrive(const QPoint &pos, const QMimeData *mime, Qt::DropAction action,
                         Qt::KeyboardModifiers modifiers);
    void dropOnFloppy(int drive, const QPoint &pos, const QMimeData *mime,
                      Qt::DropAction action, Qt::KeyboardModifiers modifiers);

    void refreshFloppy(int drive);
    int driveOfSender() const;

    /// Expand the tree down to `path` and select it. The model populates in a
    /// thread, so an index asked for too early is invalid: then the path is
    /// parked in m_pendingReveal and retried on directoryLoaded.
    void reveal(const QString &path);

    struct FloppyPane {
        QPushButton *change = nullptr;
        QPushButton *eject = nullptr;
        QTreeView *view = nullptr;
        QStandardItemModel *model = nullptr;
        QLabel *diskName = nullptr;
    };

    QFileSystemModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_projectTitle = nullptr;
    FloppyPane m_floppy[2];
    /// Whether the project root was ever chosen (showDirectory). The model's
    /// default rootPath is ".", so "no root yet" cannot be read back from it.
    bool m_rootChosen = false;
    QString m_pendingReveal;
    QString m_pendingCurrent;
    QString m_floppyPath[2];
    Clipboard m_clipboard;
};

} // namespace pist
