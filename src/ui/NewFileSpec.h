// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "ui/FileTemplates.h"

#include <QString>

namespace pist {

/// The New File transaction, minus the widgets (docs/untitled-plan.md §4).
///
/// File ▸ New File is a create-file transaction: a name, a directory and a
/// template, and the file exists on disk before anything is opened. The dialog
/// is a shell over this class, so the parts that can be wrong — the derived
/// path, the validation, the bytes — are testable without `exec()`-ing a modal.
///
/// The rules live here rather than in the dialog because they are what a caller
/// promises: every rejection has a sentence attached, ready to show.
class NewFileSpec
{
public:
    /// What to tell the user about the current (directory, name). `ok` gates
    /// the dialog's Accept; `message` is the reason it is off — or empty when
    /// everything is in order; `note` is advice that never blocks.
    struct Verdict
    {
        bool ok = false;
        QString message;
        QString note;
    };

    /// The verdict for a target that already exists — `create()` reports the
    /// same thing, so a file appearing between the dialog and the write is
    /// refused in the same words.
    static Verdict existingTargetVerdict(const QString &path);

    NewFileSpec() = default;
    NewFileSpec(const QString &directory, const QString &name, SourceTemplate kind);

    QString directory() const { return m_directory; }
    QString name() const { return m_name; }
    SourceTemplate kind() const { return m_kind; }

    /// The name as a file name: the typed name with `kSourceSuffix` appended
    /// when it carries no suffix of its own ("hello" → "hello.s"; "prog.asm"
    /// and "x.y.z" left alone, which is how a free-form extension survives).
    QString fileName() const;

    /// The absolute path the file will be created at — directory joined with
    /// fileName(). Empty when the directory is empty, so a caller can never
    /// write into the process's working directory by accident.
    QString targetPath() const;

    /// Whether this can be created right now, and why not when it cannot.
    Verdict verify() const;

    /// Create the file, with the template's bytes, through `files::write()`
    /// (the staged-write rule: a failure leaves whatever was at `targetPath()`
    /// exactly as it was). Refuses an existing target — the dialog has already
    /// said so, and a file that appeared in between must not be clobbered by
    /// the template. False and `error` set on any failure.
    bool create(QString *error) const;

private:
    QString m_directory;
    QString m_name;
    SourceTemplate m_kind = SourceTemplate::Blank;
};

} // namespace pist
