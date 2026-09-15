// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QFileSystemModel;
class QLineEdit;
class QTreeView;

namespace pist {

/// Browsable view of the project's directory.
///
/// Scoped to the directory of the open source file rather than the whole
/// filesystem: the useful set of files is the project's own, and an unbounded
/// tree makes it harder to find them. A ".." style escape is deliberately not
/// offered — the location can be set directly in the path field.
class FileBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit FileBrowser(QWidget *parent = nullptr);

public slots:
    /// Show the directory containing `sourcePath`, selecting the file itself.
    void showFor(const QString &sourcePath);

    /// Show an explicit directory.
    void showDirectory(const QString &path);

signals:
    /// A file was activated (double-clicked or Enter). The receiver decides
    /// what to do with the path.
    void fileActivated(const QString &path);

    /// The user asked to create a new `.pim` image in `directory`.
    void newImageRequested(const QString &directory);

    /// A file or directory was renamed through the browser (the model also
    /// emits its own fileRenamed; this one covers renames we initiate).
    void pathRenamed(const QString &oldPath, const QString &newPath);

    /// A file or directory is about to be deleted through the browser, so an
    /// open document on it can be dealt with before it vanishes.
    void pathDeleted(const QString &path);

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

private slots:
    void onPathEntered();
    void onActivated(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);
    void onModelPathRenamed(const QString &path, const QString &oldName, const QString &newName);

private:
    /// The directory context menu actions apply to: the selected directory,
    /// the selected file's parent, or the root when nothing is selected.
    QString contextDirectory() const;

    /// Shared by the Delete shortcut and the context menu: confirm, then
    /// deletePath. Returns true when the path is gone.
    bool confirmAndDelete(const QString &path);

    QFileSystemModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QLineEdit *m_pathEdit = nullptr;
};

} // namespace pist
