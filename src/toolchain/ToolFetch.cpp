// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "toolchain/ToolFetch.h"

#include "build/ProcessUtil.h"
#include "emu/Paths.h"
#include "toolchain/Toolchain.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace pist {
namespace toolchain {

namespace {

/// A tool's own long-running budget: building vasm from a fetched source
/// archive runs make over the whole assembler, which is minutes, not seconds.
RunOptions toolBuildOptions()
{
    RunOptions options;
    options.finishTimeoutMs = 600000;
    return options;
}

/// The names a zip-extraction tool reports for the archive's members, or an
/// empty list when no usable tool exists.
QStringList listZipMembers(const QString &zipPath, const QString &tool, QString *output)
{
    // `tool` is a full path, so match on its file name.
    const QString name = QFileInfo(tool).fileName();
    const bool unzip = name == QStringLiteral("unzip");
    if (!unzip && !name.startsWith(QStringLiteral("tar")))
        return {};
    const SyncRun run =
        runSync(tool, {unzip ? QStringLiteral("-Z1") : QStringLiteral("-tf"), zipPath});
    if (output)
        *output = run.output;
    if (!run.ok() || output->isEmpty())
        return {};
    return output->split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

/// Move a fully finished file over `dest`, replacing whatever is there.
///
/// Everything that produces the file does so under a sibling temporary name, so
/// that a failed attempt — a truncated archive, a copy that ran out of disk —
/// leaves the destination exactly as it was found rather than destroying a
/// working install or leaving an empty file where one is expected. QFile::rename
/// refuses to overwrite, so the old file is dropped last, once its replacement
/// already exists.
bool publishStaged(const QString &staging, const QString &dest)
{
    if (QFile::rename(staging, dest))
        return true;
    QFile::remove(dest);
    if (QFile::rename(staging, dest))
        return true;
    QFile::remove(staging);
    return false;
}

} // namespace

QString sha256OfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool canBuildVasm()
{
    if (QStandardPaths::findExecutable(QStringLiteral("make")).isEmpty())
        return false;
    for (const char *cc : {"cc", "gcc", "clang"})
        if (!QStandardPaths::findExecutable(QString::fromLatin1(cc)).isEmpty())
            return true;
    return false;
}

QString unpackTarGz(const QString &archivePath, const QString &destDir, QString *error)
{
    const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tar.isEmpty()) {
        *error = QObject::tr("no `tar` on PATH to unpack the source archive");
        return {};
    }

    QString mkError;
    if (!paths::ensureDirectory(destDir, &mkError)) {
        *error = mkError;
        return {};
    }

    // The archive's top-level directory is identified by what the extraction
    // adds to an empty staging area, so no tar listing format is parsed.
    const QDir dir(destDir);
    const QStringList before = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);

    const SyncRun run = runSync(
        tar, {QStringLiteral("-xzf"), archivePath, QStringLiteral("-C"), destDir});
    if (!run.ok()) {
        *error = QObject::tr("unpacking failed: %1").arg(run.failureText());
        return {};
    }

    const QStringList after = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QString &entry : after) {
        if (before.contains(entry))
            continue;
        const QString full = dir.absoluteFilePath(entry);
        if (QFileInfo(full).isDir())
            return full;
    }
    *error = QObject::tr("the archive unpacked no directory");
    return {};
}

QString buildVasm(const QString &sourceDir, QString *log, QString *error)
{
    const QString make = QStandardPaths::findExecutable(QStringLiteral("make"));
    if (make.isEmpty()) {
        *error = QObject::tr("no `make` on PATH");
        return {};
    }

    const SyncRun run =
        runSync(make, {QStringLiteral("-C"), sourceDir, QStringLiteral("CPU=m68k"),
                       QStringLiteral("SYNTAX=mot")},
                toolBuildOptions());
    if (log)
        *log += run.output;

    // The binary name is fixed by vasm's Makefile; the .exe suffix is decided
    // by the platform, and findExecutable applies that rule for us.
    const QString binary =
        QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"), {sourceDir});
    if (binary.isEmpty()) {
        *error = run.ok() ? QObject::tr("make succeeded but produced no vasmm68k_mot")
                          : QObject::tr("make failed — see the log");
        return {};
    }
    if (!run.ok()) {
        // make failed but a stale binary exists: do not install it.
        *error = QObject::tr("make failed — see the log");
        return {};
    }
    return binary;
}

QString installToolBinary(const QString &binaryPath, QString *error)
{
    const QString destDir = suggestedInstallDir();
    QString mkError;
    if (!paths::ensureDirectory(destDir, &mkError)) {
        *error = mkError;
        return {};
    }

    const QString dest = destDir + QLatin1Char('/') + QFileInfo(binaryPath).fileName();
    // Copy beside the target and rename into place, so a failure here (full
    // disk, an unreadable or vanished source) cannot delete the toolchain the
    // user already has: `dest` is not touched until its replacement exists.
    const QString staging = dest + QStringLiteral(".part");
    QFile::remove(staging);
    if (!QFile::copy(binaryPath, staging)) {
        QFile::remove(staging);
        *error = QObject::tr("could not copy %1 to %2").arg(binaryPath, dest);
        return {};
    }
    // Harmless on Windows, where executability comes from the suffix; decisive
    // on POSIX, where a copied build product already has the bit but a
    // defensive set costs nothing. Set before the rename so the file is never
    // visible at `dest` without it.
    QFile::setPermissions(staging, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                     | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                     | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                     | QFileDevice::ExeOther);
    if (!publishStaged(staging, dest)) {
        *error = QObject::tr("could not install %1 as %2").arg(binaryPath, dest);
        return {};
    }
    return dest;
}

QString installVasmFromTarball(const QString &tarballPath, const QString &expectedSha256,
                               QString *log, QString *error)
{
    const QString actual = sha256OfFile(tarballPath);
    if (actual.isEmpty()) {
        *error = QObject::tr("could not read %1").arg(tarballPath);
        return {};
    }
    if (actual != expectedSha256) {
        *error = QObject::tr("checksum mismatch: expected %1, got %2 — refusing to build an "
                             "archive that is not the pinned upstream release")
                     .arg(expectedSha256, actual);
        return {};
    }

    QTemporaryDir work;
    if (!work.isValid()) {
        *error = QObject::tr("could not create a temporary build directory");
        return {};
    }
    const QString sourceDir = unpackTarGz(tarballPath, work.path(), error);
    if (sourceDir.isEmpty())
        return {};

    // vasm's tarball nests the source one level down (vasm/…); build there.
    const QString binary = buildVasm(sourceDir, log, error);
    if (binary.isEmpty())
        return {};
    return installToolBinary(binary, error);
}

bool extractZipMember(const QString &zipPath, const QString &nameSuffix,
                      const QString &destPath, QString *error)
{
    // Everything is extracted under a sibling temporary name and moved into
    // place only once the tool reported success. QProcess opens (and truncates)
    // a setStandardOutputFile target before the child even starts, so a failed
    // attempt would otherwise leave a half-written or empty file where a working
    // ROM belongs — which discovery would hand to Hatari as a broken ROM and
    // which hides the retry the setup dialog offers.
    const QString tempPath = destPath + QStringLiteral(".part");
    QFile::remove(tempPath);

    QStringList tried;
    bool extracted = false;

    // python3 does list+extract in one step, matching exactly how CI extracts
    // the same archive, so it goes first where it exists.
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (!python.isEmpty()) {
        tried << QStringLiteral("python3");
        const QString script = QStringLiteral(
            "import sys, zipfile\n"
            "z = zipfile.ZipFile(sys.argv[1])\n"
            "name = next(n for n in z.namelist() if n.endswith(sys.argv[2]))\n"
            "open(sys.argv[3], 'wb').write(z.read(name))\n");
        RunOptions options;
        // The member is written straight to the staging path, not captured:
        // the extracted ROM is the product, not the tool's chatter.
        options.stdoutFile = tempPath;
        const SyncRun run =
            runSync(python, {QStringLiteral("-c"), script, zipPath, nameSuffix, tempPath},
                    options);
        extracted = run.ok() && QFileInfo::exists(tempPath);
    }

    for (const char *candidate : {"unzip", "tar"}) {
        if (extracted)
            break;
        const QString tool = QStandardPaths::findExecutable(QString::fromLatin1(candidate));
        if (tool.isEmpty())
            continue;
        tried << QString::fromLatin1(candidate);

        QString listing;
        const QStringList members = listZipMembers(zipPath, tool, &listing);
        QString member;
        for (const QString &name : members)
            if (name.endsWith(nameSuffix))
                member = name;
        if (member.isEmpty())
            continue;

        const QStringList args = QFileInfo(tool).fileName() == QStringLiteral("unzip")
            ? QStringList{QStringLiteral("-p"), zipPath, member}
            : QStringList{QStringLiteral("-xOf"), zipPath, member};
        RunOptions options;
        options.stdoutFile = tempPath;
        const SyncRun run = runSync(tool, args, options);
        extracted = run.ok() && QFileInfo::exists(tempPath);
    }

    if (!extracted) {
        QFile::remove(tempPath);
        *error = QObject::tr("could not extract %1 from the archive (tried: %2)")
                     .arg(nameSuffix, tried.join(QStringLiteral(", ")));
        return false;
    }

    if (!publishStaged(tempPath, destPath)) {
        *error = QObject::tr("could not move the extracted %1 into place").arg(nameSuffix);
        return false;
    }
    return true;
}

QString installEmuTosFromZip(const QString &zipPath, const QString &expectedSha256,
                             QString *error)
{
    const QString actual = sha256OfFile(zipPath);
    if (actual.isEmpty()) {
        *error = QObject::tr("could not read %1").arg(zipPath);
        return {};
    }
    if (actual != expectedSha256) {
        *error = QObject::tr("checksum mismatch: expected %1, got %2 — refusing to install an "
                             "archive that is not the pinned upstream release")
                     .arg(expectedSha256, actual);
        return {};
    }

    const QString destDir = paths::suggestedRomDir();
    QString mkError;
    if (!paths::ensureDirectory(destDir, &mkError)) {
        *error = mkError;
        return {};
    }
    const QString dest = destDir + QLatin1Char('/')
                       + QString::fromLatin1(pins::kEmuTosImageSuffix);
    if (!extractZipMember(zipPath, QString::fromLatin1(pins::kEmuTosImageSuffix), dest, error))
        return {};
    return dest;
}

} // namespace toolchain
} // namespace pist
