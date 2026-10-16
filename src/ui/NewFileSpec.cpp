// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/NewFileSpec.h"

#include "support/FileWrite.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>

namespace pist {

namespace {

/// The characters a GEMDOS file name cannot hold, plus the one the host cannot.
///
/// The name is refused rather than noted because these are not stylistic: PiST
/// writes a source to a floppy image on request (and the emulator's GEMDOS has
/// to open it), and a name the host rejects outright would fail at creation
/// anyway. A name merely *longer* than 8.3 assembles fine from the host — only
/// floppy destinations care — so that one is a note, never a refusal
/// (docs/untitled-plan.md §4).
bool gemdosReservedChar(QChar c, QChar *offender)
{
    const QString reserved = QStringLiteral("*?:\"<>|");
    if (reserved.contains(c) || c.unicode() < 0x20) {
        if (offender)
            *offender = c;
        return true;
    }
    return false;
}

} // namespace

NewFileSpec::NewFileSpec(const QString &directory, const QString &name, SourceTemplate kind)
    : m_directory(directory)
    , m_name(name)
    , m_kind(kind)
{
}

NewFileSpec::Verdict NewFileSpec::existingTargetVerdict(const QString &path)
{
    Verdict verdict;
    verdict.message = QObject::tr("%1 already exists.").arg(QFileInfo(path).fileName());
    return verdict;
}

QString NewFileSpec::fileName() const
{
    QString name = m_name.trimmed();
    // A trailing dot is a name the user is still typing ("prog."): the suffix
    // replaces it rather than being appended to it.
    while (name.endsWith(QLatin1Char('.')))
        name.chop(1);
    if (name.isEmpty())
        return {};
    if (QFileInfo(name).suffix().isEmpty())
        name += QLatin1String(kSourceSuffix);
    return name;
}

QString NewFileSpec::targetPath() const
{
    const QString name = fileName();
    if (name.isEmpty())
        return {};
    const QString directory = m_directory.trimmed();
    if (directory.isEmpty())
        return {};
    return QFileInfo(QDir(directory), name).absoluteFilePath();
}

NewFileSpec::Verdict NewFileSpec::verify() const
{
    Verdict verdict;

    const QString name = fileName();
    if (name.isEmpty()) {
        verdict.message = QObject::tr("Enter a name.");
        return verdict;
    }

    const QString directory = m_directory.trimmed();
    if (directory.isEmpty()) {
        verdict.message = QObject::tr("Choose a directory.");
        return verdict;
    }
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir()) {
        verdict.message = QObject::tr("%1 is not a directory.").arg(directory);
        return verdict;
    }

    // The name has to stay a name: separators here would put the file
    // somewhere the user did not choose (and one they may not even see).
    if (name == QLatin1String(".") || name == QLatin1String("..")
        || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        verdict.message = QObject::tr("A file name, not a path.");
        return verdict;
    }
    QChar offender;
    for (const QChar c : name) {
        if (gemdosReservedChar(c, &offender)) {
            verdict.message = QObject::tr("A GEMDOS file name cannot contain %1.").arg(offender);
            return verdict;
        }
    }

    const QString path = targetPath();
    if (QFileInfo::exists(path)) {
        verdict.message = existingTargetVerdict(path).message;
        return verdict;
    }

    // Advice, not a refusal: see gemdosReservedChar.
    const QFileInfo fileInfo(name);
    if (fileInfo.completeBaseName().length() > 8 || fileInfo.suffix().length() > 3) {
        verdict.note = QObject::tr("Longer than GEMDOS' 8.3 — it assembles from here, "
                                   "but a floppy image will want a shorter name.");
    }

    verdict.ok = true;
    return verdict;
}

bool NewFileSpec::create(QString *error) const
{
    const QString path = targetPath();
    if (path.isEmpty()) {
        if (error)
            *error = QObject::tr("There is nowhere to create the file.");
        return false;
    }
    if (QFileInfo::exists(path)) {
        if (error)
            *error = existingTargetVerdict(path).message;
        return false;
    }
    return files::write(path, templateBytes(m_kind), error);
}

} // namespace pist
