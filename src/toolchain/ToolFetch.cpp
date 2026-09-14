// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "toolchain/ToolFetch.h"

#include "emu/Paths.h"
#include "toolchain/Toolchain.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace pist {
namespace toolchain {

namespace {

/// Run a process to completion, capturing its combined output. Returns false
/// when the process could not be started or exited non-zero; `output` receives
/// stdout+stderr either way, so a failure message can quote the tool.
bool runProcess(const QString &program, const QStringList &args,
                const QString &workingDir, QString *output,
                const QString &stdoutFile = QString())
{
    QProcess p;
    if (!workingDir.isEmpty())
        p.setWorkingDirectory(workingDir);
    if (!stdoutFile.isEmpty())
        p.setStandardOutputFile(stdoutFile);
    p.start(program, args);
    if (!p.waitForStarted(10000)) {
        if (output)
            *output = QStringLiteral("could not start %1").arg(program);
        return false;
    }
    if (!p.waitForFinished(600000)) {
        p.kill();
        p.waitForFinished(5000);
        if (output)
            *output = QStringLiteral("%1 did not finish").arg(program);
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(p.readAllStandardOutput())
                + QString::fromUtf8(p.readAllStandardError());
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

QString findTool(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

/// The names a zip-extraction tool reports for the archive's members, or an
/// empty list when no usable tool exists.
QStringList listZipMembers(const QString &zipPath, const QString &tool, QString *output)
{
    // `tool` is a full path, so match on its file name.
    const QString name = QFileInfo(tool).fileName();
    bool ok = false;
    if (name == QStringLiteral("unzip"))
        ok = runProcess(tool, {QStringLiteral("-Z1"), zipPath}, QString(), output);
    else if (name.startsWith(QStringLiteral("tar")))
        ok = runProcess(tool, {QStringLiteral("-tf"), zipPath}, QString(), output);
    if (!ok || output->isEmpty())
        return {};
    return output->split(QLatin1Char('\n'), Qt::SkipEmptyParts);
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
    if (findTool(QStringLiteral("make")).isEmpty())
        return false;
    for (const char *cc : {"cc", "gcc", "clang"})
        if (!findTool(QString::fromLatin1(cc)).isEmpty())
            return true;
    return false;
}

QString unpackTarGz(const QString &archivePath, const QString &destDir, QString *error)
{
    QString output;
    const QString tar = findTool(QStringLiteral("tar"));
    if (tar.isEmpty()) {
        *error = QStringLiteral("no `tar` on PATH to unpack the source archive");
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

    if (!runProcess(tar, {QStringLiteral("-xzf"), archivePath, QStringLiteral("-C"), destDir},
                    QString(), &output)) {
        *error = QStringLiteral("unpacking failed: %1").arg(output.trimmed());
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
    *error = QStringLiteral("the archive unpacked no directory");
    return {};
}

QString buildVasm(const QString &sourceDir, QString *log, QString *error)
{
    const QString make = findTool(QStringLiteral("make"));
    if (make.isEmpty()) {
        *error = QStringLiteral("no `make` on PATH");
        return {};
    }

    QString output;
    const bool ok = runProcess(
        make, {QStringLiteral("-C"), sourceDir, QStringLiteral("CPU=m68k"),
               QStringLiteral("SYNTAX=mot")},
        QString(), &output);
    if (log)
        *log += output;

    // The binary name is fixed by vasm's Makefile; the .exe suffix is decided
    // by the platform, and findExecutable applies that rule for us.
    const QString binary =
        QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"), {sourceDir});
    if (binary.isEmpty()) {
        *error = ok ? QStringLiteral("make succeeded but produced no vasmm68k_mot")
                    : QStringLiteral("make failed — see the log");
        return {};
    }
    if (!ok) {
        // make failed but a stale binary exists: do not install it.
        *error = QStringLiteral("make failed — see the log");
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
    // QFile::copy refuses to overwrite, so a previous install is removed first.
    QFile::remove(dest);
    if (!QFile::copy(binaryPath, dest)) {
        *error = QStringLiteral("could not copy %1 to %2").arg(binaryPath, dest);
        return {};
    }
    // Harmless on Windows, where executability comes from the suffix; decisive
    // on POSIX, where a copied build product already has the bit but a
    // defensive set costs nothing.
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                | QFileDevice::ExeOther);
    return dest;
}

QString installVasmFromTarball(const QString &tarballPath, const QString &expectedSha256,
                               QString *log, QString *error)
{
    const QString actual = sha256OfFile(tarballPath);
    if (actual.isEmpty()) {
        *error = QStringLiteral("could not read %1").arg(tarballPath);
        return {};
    }
    if (actual != expectedSha256) {
        *error = QStringLiteral("checksum mismatch: expected %1, got %2 — refusing to build an "
                                "archive that is not the pinned upstream release")
                     .arg(expectedSha256, actual);
        return {};
    }

    QTemporaryDir work;
    if (!work.isValid()) {
        *error = QStringLiteral("could not create a temporary build directory");
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
    QStringList tried;

    // python3 does list+extract in one step, matching exactly how CI extracts
    // the same archive, so it goes first where it exists.
    const QString python = findTool(QStringLiteral("python3"));
    if (!python.isEmpty()) {
        tried << QStringLiteral("python3");
        QString output;
        const QString script = QStringLiteral(
            "import sys, zipfile\n"
            "z = zipfile.ZipFile(sys.argv[1])\n"
            "name = next(n for n in z.namelist() if n.endswith(sys.argv[2]))\n"
            "open(sys.argv[3], 'wb').write(z.read(name))\n");
        if (runProcess(python, {QStringLiteral("-c"), script, zipPath, nameSuffix, destPath},
                       QString(), &output)
            && QFileInfo::exists(destPath))
            return true;
    }

    for (const char *candidate : {"unzip", "tar"}) {
        const QString tool = findTool(QString::fromLatin1(candidate));
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

        QString output;
        const QStringList args = QFileInfo(tool).fileName() == QStringLiteral("unzip")
            ? QStringList{QStringLiteral("-p"), zipPath, member}
            : QStringList{QStringLiteral("-xOf"), zipPath, member};
        if (runProcess(tool, args, QString(), &output, destPath)
            && QFileInfo::exists(destPath))
            return true;
    }

    *error = QStringLiteral("could not extract %1 from the archive (tried: %2)")
                 .arg(nameSuffix, tried.join(QStringLiteral(", ")));
    return false;
}

QString installEmuTosFromZip(const QString &zipPath, const QString &expectedSha256,
                             QString *error)
{
    const QString actual = sha256OfFile(zipPath);
    if (actual.isEmpty()) {
        *error = QStringLiteral("could not read %1").arg(zipPath);
        return {};
    }
    if (actual != expectedSha256) {
        *error = QStringLiteral("checksum mismatch: expected %1, got %2 — refusing to install an "
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
