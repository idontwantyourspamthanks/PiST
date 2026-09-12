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
    /// A file was activated (double-clicked or Enter). Only assembly-ish files
    /// are emitted; the receiver decides what to do with them.
    void fileActivated(const QString &path);

private slots:
    void onPathEntered();
    void onActivated(const QModelIndex &index);

private:
    QFileSystemModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QLineEdit *m_pathEdit = nullptr;
};

} // namespace pist
