// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/FloppyTransfer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

namespace pist {
namespace floppy {

namespace {

/// One member of a copied subtree: the chosen entry itself, then everything
/// below it when it is a folder.
struct SubtreeMember {
    const Entry *entry = nullptr;
    /// The member's path inside the subtree: empty for the root itself,
    /// `PROG.PRG` for `AUTO/PROG.PRG` when `AUTO` was chosen. This is what a
    /// destination in another tree is rebuilt from, so a chosen entry keeps its
    /// own name wherever it lands.
    QString relativePath;
};

/// The subtree of `entries` rooted at `root` (an image-relative path). The
/// comparison is case-insensitive, like the writer's own subtree rule for
/// removals — an entry names itself and everything below it. Empty when the
/// disk has no such entry.
QVector<SubtreeMember> subtreeOf(const QVector<Entry> &entries, const QString &root)
{
    QVector<SubtreeMember> members;
    for (const Entry &entry : entries) {
        if (entry.path.compare(root, Qt::CaseInsensitive) == 0) {
            members.append({&entry, QString()});
        } else if (entry.path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) {
            members.append({&entry, entry.path.mid(root.size() + 1)});
        }
    }
    return members;
}

/// Where a member of a copied set lands inside an image: its own path when the
/// destination is the image root — it already names its place there — and
/// `dirInImage` joined in front of it otherwise, so the entry keeps its name
/// and a copied folder keeps its shape.
QString imageDestination(const QString &dirInImage, const QString &entryPath)
{
    if (dirInImage.isEmpty())
        return entryPath;
    return dirInImage + QLatin1Char('/') + entryPath;
}

} // namespace

Transfer::Transfer(Host *host)
    : m_host(host)
{
}

Transfer::Result Transfer::refusal(Outcome outcome)
{
    Result result;
    result.outcome = outcome;
    return result;
}

bool Transfer::confirmTargets(const QVector<Target> &targets, Outcome *outcome)
{
    for (const Target &target : targets) {
        if (!m_host->confirmRewrite(target.image)) {
            *outcome = Declined;
            return false;
        }
    }
    for (const Target &target : targets) {
        if (m_host->mountedImage(target.drive) != target.image) {
            *outcome = Resynced;
            return false;
        }
    }
    return true;
}

QString Transfer::uniqueHostDestination(const QString &dir, const QString &name)
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

Transfer::Result Transfer::hostToImage(int drive, const QString &dirInImage,
                                       const QStringList &hostPaths)
{
    Result result;
    if (drive < 0 || drive > 1 || hostPaths.isEmpty())
        return result;
    const QString image = m_host->mountedImage(drive);
    if (image.isEmpty())
        return result;
    const QString dir = normalizeEntryPath(dirInImage);

    QVector<Item> additions;
    for (const QString &path : hostPaths) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        // Collect relative to the file's parent, so the entry keeps its own
        // name at the top of the copied set.
        QVector<Item> collected;
        if (!collectHostItems(info.absolutePath(), {info.absoluteFilePath()}, &collected,
                              &result.error)) {
            result.outcome = Unreadable;
            result.subject = info.absoluteFilePath();
            return result;
        }
        for (Item &item : collected)
            item.destPath = imageDestination(dir, item.destPath);
        additions += collected;
    }
    if (additions.isEmpty())
        return result;

    Outcome outcome = Invalid;
    if (!confirmTargets({{drive, image}}, &outcome))
        return refusal(outcome);
    if (!updateImage(image, additions, {}, &result.error)) {
        result.outcome = AddFailed;
        result.subject = image;
        return result;
    }
    result.outcome = Completed;
    result.refresh = {drive};
    return result;
}

Transfer::Result Transfer::imageToHost(int drive, const QStringList &entryPaths,
                                       const QString &targetDir, bool removeSource)
{
    Result result;
    if (drive < 0 || drive > 1 || entryPaths.isEmpty() || !QFileInfo(targetDir).isDir())
        return result;
    const QString image = m_host->mountedImage(drive);
    if (image.isEmpty())
        return result;

    // A move rewrites the image; a plain copy-out does not.
    if (removeSource) {
        Outcome outcome = Invalid;
        if (!confirmTargets({{drive, image}}, &outcome))
            return refusal(outcome);
    }

    QByteArray raw;
    if (!loadRaw(image, &raw, &result.error)) {
        result.outcome = Unreadable;
        result.subject = image;
        return result;
    }
    QString listError;
    const QVector<Entry> entries = listRaw(raw, &listError);
    if (!listError.isEmpty()) {
        result.outcome = Unreadable;
        result.subject = image;
        result.error = listError;
        return result;
    }

    // One reason string for the whole set, as the failed-copy message reports
    // the last thing that went wrong rather than a per-entry list.
    QString error;
    QStringList removals;
    for (const QString &chosen : entryPaths) {
        const QString entry = normalizeEntryPath(chosen);
        // The chosen entry lands in `targetDir` under its own name and the rest
        // of its subtree follows inside it — resolved before anything is
        // written, so two chosen entries never claim the same fresh name.
        const QString base = entry.section(QLatin1Char('/'), -1);
        const QString target = uniqueHostDestination(targetDir, base);
        const QVector<SubtreeMember> members = subtreeOf(entries, entry);
        bool ok = true;
        if (members.isEmpty()) {
            ok = false;
            result.cause = EntryMissing;
            result.entry = base;
        }
        for (const SubtreeMember &member : members) {
            const Entry &entryData = *member.entry;
            if (removeSource)
                removals.append(entryData.path);
            const QString destination = member.relativePath.isEmpty()
                ? target
                : QDir(target).absoluteFilePath(member.relativePath);
            // Defence in depth against crafted images: an extraction must land
            // inside the folder the user chose. The listing sanitises entry
            // names, so this can only fire on something that slipped through
            // anyway — refuse it loudly rather than write outside.
            if (!QDir::cleanPath(destination)
                     .startsWith(QDir::cleanPath(targetDir) + QLatin1Char('/'))) {
                ok = false;
                result.cause = EntryEscapes;
                result.entry = entryData.path;
                break;
            }
            bool written = false;
            if (entryData.isDirectory) {
                written = QDir().mkpath(destination);
                // Named here, at the step that failed: `error` otherwise still
                // held whatever the last read said (nothing, or a previous
                // entry's damage), and the caller's "Could not copy … from …"
                // message then ended in a colon with no reason after it
                // (MIN-85).
                if (!written)
                    error = QObject::tr("cannot create %1").arg(destination);
            } else {
                QByteArray data;
                if (readFileRaw(raw, entryData.path, &data, &error)) {
                    QFile out(destination);
                    if (!out.open(QIODevice::WriteOnly)) {
                        error = QObject::tr("cannot write %1: %2")
                                    .arg(destination, out.errorString());
                    } else {
                        const qint64 put = out.write(data);
                        if (put == data.size()) {
                            written = true;
                        } else {
                            error = out.errorString().isEmpty()
                                ? QObject::tr("short write to %1").arg(destination)
                                : QObject::tr("cannot write %1: %2")
                                      .arg(destination, out.errorString());
                        }
                    }
                } else {
                    // A damaged entry: `error` names the damage, and the caller
                    // shows it as it came.
                    result.cause = NoCause;
                    result.entry.clear();
                }
            }
            if (!written) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            result.names.append(base);
            result.error = error;
        }
    }

    if (!result.names.isEmpty()) {
        result.outcome = CopyFailed;
        result.subject = image;
        return result;
    }

    if (removeSource && !removals.isEmpty()) {
        if (!updateImage(image, {}, removals, &result.error)) {
            result.outcome = SourceRemovalFailed;
            result.subject = image;
            result.refresh = {drive};
            return result;
        }
        result.refresh = {drive};
    }
    result.outcome = Completed;
    return result;
}

Transfer::Result Transfer::imageToImage(int sourceDrive, const QStringList &entryPaths,
                                        int targetDrive, const QString &dirInImage,
                                        bool removeSource)
{
    Result result;
    if (sourceDrive < 0 || sourceDrive > 1 || targetDrive < 0 || targetDrive > 1
        || entryPaths.isEmpty())
        return result;
    const QString sourceImage = m_host->mountedImage(sourceDrive);
    const QString targetImage = m_host->mountedImage(targetDrive);
    if (sourceImage.isEmpty() || targetImage.isEmpty())
        return result;
    const QString dir = normalizeEntryPath(dirInImage);
    const bool sameImage = QFileInfo(sourceImage).absoluteFilePath()
        == QFileInfo(targetImage).absoluteFilePath();

    // Every write below rebuilds an image — the target always, and the source
    // too when this is a move between two different disks — so both disks are
    // asked about before either drive is re-read.
    QVector<Target> targets{{targetDrive, targetImage}};
    if (!sameImage && removeSource)
        targets.append({sourceDrive, sourceImage});
    Outcome outcome = Invalid;
    if (!confirmTargets(targets, &outcome))
        return refusal(outcome);

    for (const QString &chosen : entryPaths) {
        const QString entry = normalizeEntryPath(chosen);
        if (sameImage && (dir == entry || dir.startsWith(entry + QLatin1Char('/')))) {
            result.outcome = DestinationInsideSource;
            result.subject = entry;
            return result;
        }
    }

    QByteArray raw;
    if (!loadRaw(sourceImage, &raw, &result.error)) {
        result.outcome = Unreadable;
        result.subject = sourceImage;
        return result;
    }
    const QVector<Entry> entries = listRaw(raw, &result.error);
    if (!result.error.isEmpty()) {
        result.outcome = Unreadable;
        result.subject = sourceImage;
        return result;
    }

    QVector<Item> additions;
    QStringList removals;
    for (const QString &chosen : entryPaths) {
        const QString entry = normalizeEntryPath(chosen);
        for (const SubtreeMember &member : subtreeOf(entries, entry)) {
            const Entry &entryData = *member.entry;
            if (removeSource)
                removals.append(entryData.path);
            Item item;
            item.destPath = imageDestination(dir, entryData.path);
            item.isDirectory = entryData.isDirectory;
            if (!entryData.isDirectory
                && !readFileRaw(raw, entryData.path, &item.data, &result.error)) {
                result.outcome = Unreadable;
                result.subject = sourceImage;
                return result;
            }
            additions.append(item);
        }
    }
    if (additions.isEmpty()) {
        result.outcome = NothingToCopy;
        result.subject = sourceImage;
        return result;
    }

    if (sameImage) {
        if (!updateImage(targetImage, additions, removals, &result.error)) {
            result.outcome = WriteFailed;
            result.subject = targetImage;
            return result;
        }
    } else {
        if (!updateImage(targetImage, additions, {}, &result.error)) {
            result.outcome = WriteFailed;
            result.subject = targetImage;
            return result;
        }
        if (removeSource && !removals.isEmpty()) {
            if (!updateImage(sourceImage, {}, removals, &result.error)) {
                result.outcome = SourceRemovalFailed;
                result.subject = sourceImage;
                result.refresh = {sourceDrive, targetDrive};
                return result;
            }
        }
    }
    result.outcome = Completed;
    result.refresh = {targetDrive};
    if (removeSource && !sameImage)
        result.refresh.append(sourceDrive);
    return result;
}

} // namespace floppy
} // namespace pist
