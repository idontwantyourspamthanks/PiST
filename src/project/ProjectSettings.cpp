// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "project/ProjectSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>

namespace pist {

namespace {

/// The version this build writes, and the newest it can read. Bumping it is what
/// lets a later PiST change the schema without an older build dropping the fields
/// it does not understand and then saving over them.
constexpr int kProjectFileVersion = 1;

QJsonArray toArray(const QStringList &list)
{
    QJsonArray array;
    for (const QString &item : list)
        array.append(item);
    return array;
}

/// The one shape of "this file is wrong" message, so a malformed entry reads the
/// same way as a document that does not parse at all.
QString invalidEntry(const QString &path, const QString &what)
{
    return QStringLiteral("'%1' is not a valid project file: %2").arg(path, what);
}

/// Read one list of strings. An absent (or null) value yields an empty list;
/// anything else that is not an array of strings is a malformed entry that fails
/// the load, naming `name` and — for a bad element — its index.
///
/// The old reader coerced both cases into an empty list, so a wrong-typed entry
/// became an empty `-I`/`-D` argument on the assembler command line with nothing
/// to explain where it came from.
bool readStringList(const QJsonValue &value, const QString &name, const QString &path,
                    QStringList *out, QString *error)
{
    if (value.isUndefined() || value.isNull()) {
        out->clear();
        return true;
    }
    if (!value.isArray()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'%1' must be a list of strings").arg(name));
        return false;
    }
    QStringList list;
    const QJsonArray array = value.toArray();
    list.reserve(array.size());
    for (int index = 0; index < array.size(); ++index) {
        const QJsonValue item = array.at(index);
        if (!item.isString()) {
            if (error)
                *error = invalidEntry(
                    path, QStringLiteral("'%1[%2]' must be a string").arg(name).arg(index));
            return false;
        }
        list.append(item.toString());
    }
    *out = list;
    return true;
}

/// Read one string. An absent (or null) value yields `fallback`; a value of any
/// other type is a malformed entry rather than the empty string the old reader
/// produced for it. `name` is the qualified key, as written in the file, so the
/// message can point at the entry that has to be fixed.
bool readString(const QJsonValue &value, const QString &name, const QString &path,
                const QString &fallback, QString *out, QString *error)
{
    if (value.isUndefined() || value.isNull()) {
        *out = fallback;
        return true;
    }
    if (!value.isString()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'%1' must be a string").arg(name));
        return false;
    }
    *out = value.toString();
    return true;
}

/// Read one integer. An absent (or null) value yields `fallback`; a non-number is
/// a malformed entry. A number that is merely out of range is not an error here —
/// the caller clamps it, which is what keeps a hostile file from reaching the
/// emulator command line.
bool readInt(const QJsonValue &value, const QString &name, const QString &path,
             int fallback, int *out, QString *error)
{
    if (value.isUndefined() || value.isNull()) {
        *out = fallback;
        return true;
    }
    if (!value.isDouble()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'%1' must be a number").arg(name));
        return false;
    }
    *out = value.toInt(fallback);
    return true;
}

/// Read one boolean. An absent (or null) value yields `fallback`; a non-boolean
/// is a malformed entry.
bool readBool(const QJsonValue &value, const QString &name, const QString &path,
              bool fallback, bool *out, QString *error)
{
    if (value.isUndefined() || value.isNull()) {
        *out = fallback;
        return true;
    }
    if (!value.isBool()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'%1' must be a boolean").arg(name));
        return false;
    }
    *out = value.toBool();
    return true;
}

/// Resolve stored path entries against the project file's own directory.
///
/// A relative `-I` or additional source means "beside the project", which is what
/// makes the file portable; reading the strings verbatim meant they were then
/// resolved against PiST's working directory, so the same project built
/// differently depending on where the IDE was started from. Absolute entries and
/// empty strings are left exactly as stored — an empty string is still a string
/// the user wrote, and rebasing it would silently invent a path (the project
/// directory itself).
QStringList resolveRelativeToProject(const QStringList &entries, const QString &projectDir)
{
    QStringList resolved;
    resolved.reserve(entries.size());
    for (const QString &entry : entries) {
        if (entry.isEmpty() || QDir::isAbsolutePath(entry))
            resolved.append(entry);
        else
            resolved.append(QDir::cleanPath(projectDir + QLatin1Char('/') + entry));
    }
    return resolved;
}

} // namespace

namespace settings {

OutputPaths outputPathsFor(const QString &sourcePath)
{
    OutputPaths paths;
    if (sourcePath.isEmpty())
        return paths;

    const QFileInfo info(sourcePath);
    const QString base = info.absolutePath() + QLatin1Char('/') + info.completeBaseName();
    paths.program = base + QStringLiteral(".prg");
    paths.listing = base + QStringLiteral(".lst");
    paths.project = base + QLatin1String(kProjectSuffix);
    return paths;
}

QString projectFileFor(const QString &sourcePath)
{
    if (sourcePath.isEmpty())
        return {};
    const QFileInfo info(sourcePath);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
         + QLatin1String(kProjectSuffix);
}

bool save(const ProjectSettings &s, const QString &path, QString *error)
{
    QJsonObject root;
    root[QStringLiteral("version")] = kProjectFileVersion;

    QJsonObject build;
    build[QStringLiteral("assemblerPath")] = s.assemblerPath;
    build[QStringLiteral("linkerPath")] = s.linkerPath;
    build[QStringLiteral("additionalSources")] = toArray(s.additionalSources);
    build[QStringLiteral("cpu")] = s.cpu;
    build[QStringLiteral("includePaths")] = toArray(s.includePaths);
    build[QStringLiteral("defines")] = toArray(s.defines);
    build[QStringLiteral("extraArgs")] = toArray(s.extraBuildArgs);
    root[QStringLiteral("build")] = build;

    QJsonObject emu;
    emu[QStringLiteral("hatariPath")] = s.hatariPath;
    emu[QStringLiteral("machine")] = machineCliName(s.machine);
    emu[QStringLiteral("monitor")] = s.monitor;
    emu[QStringLiteral("memSizeMiB")] = s.memSizeMiB;
    emu[QStringLiteral("tosPath")] = s.tosPath;
    emu[QStringLiteral("hardDiskImage")] = s.hardDiskImage;
    emu[QStringLiteral("floppyImages")] = toArray(s.floppyImages);
    emu[QStringLiteral("fastForward")] = s.fastForward;
    emu[QStringLiteral("extraArgs")] = toArray(s.extraEmulatorArgs);
    emu[QStringLiteral("debugBackend")] = s.debugBackend;
    root[QStringLiteral("emulator")] = emu;

    // Written through a temporary file that is renamed into place on commit().
    // Opening the destination directly with Truncate destroyed the only copy of
    // the project the moment anything went wrong after the open — a full disk, a
    // crash, a power cut — and the next load() then reported the remains as an
    // invalid project file.
    //
    // commit() alone does not give that guarantee on the Qt the release archives
    // ship. The payload is buffered, so write() reports success and the bytes
    // only reach the disk on the flush commit() does internally — and Qt 6.8's
    // commit() does not notice that flush failing: it renames the truncated
    // temporary file over the destination and returns true, which is precisely
    // the loss this code exists to prevent. (Qt 6.10 checks the device error
    // there; 6.8.1, bundled in every archive, does not.) So flush explicitly and
    // treat any device error as the failure it is: cancelWriting() makes the
    // commit discard the temporary file instead of renaming it, which leaves the
    // project already on disk untouched.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("cannot write '%1': %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    bool onDisk = file.write(json) == json.size();
    if (onDisk && !file.flush())
        onDisk = false;
    if (!onDisk || file.error() != QFileDevice::NoError) {
        const QString reason = file.errorString();
        file.cancelWriting();
        // Only discards the temporary file now; the failure is already in hand.
        file.commit();
        if (error)
            *error = QStringLiteral("cannot write '%1': %2").arg(path, reason);
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("cannot write '%1': %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool load(ProjectSettings *s, const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read '%1': %2").arg(path, file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("'%1' is not a valid project file: %2")
                         .arg(path, parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();

    // Refuse a file written by a newer PiST before reading a single field: this
    // build does not know what the extra keys mean, so loading one would drop
    // them the next time the project is saved. The version is checked as a
    // number first, so a hand-edited non-number fails here rather than silently
    // reading as 1.
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    if (!versionValue.isUndefined() && !versionValue.isNull() && !versionValue.isDouble()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'version' must be a number"));
        return false;
    }
    const double version = versionValue.toDouble(kProjectFileVersion);
    if (version > kProjectFileVersion) {
        if (error)
            *error = QStringLiteral("'%1' was written by a newer PiST: project version %2, "
                                    "this build supports version %3")
                         .arg(path, QString::number(version),
                              QString::number(kProjectFileVersion));
        return false;
    }

    // A present container of the wrong type is malformed too; left to
    // toObject() it would read as empty and silently drop everything inside.
    if (root.contains(QStringLiteral("build"))
        && !root.value(QStringLiteral("build")).isObject()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'build' must be an object"));
        return false;
    }
    if (root.contains(QStringLiteral("emulator"))
        && !root.value(QStringLiteral("emulator")).isObject()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'emulator' must be an object"));
        return false;
    }

    // Relative paths in the file are read against the file's own directory, not
    // the process's, so a project resolves the same wherever PiST was started.
    const QString projectDir = QFileInfo(path).absolutePath();

    const QJsonObject build = root.value(QStringLiteral("build")).toObject();
    QStringList additionalSources;
    QStringList includePaths;
    if (!readString(build.value(QStringLiteral("assemblerPath")),
                    QStringLiteral("build.assemblerPath"), path, QString(),
                    &s->assemblerPath, error)
        || !readString(build.value(QStringLiteral("linkerPath")),
                       QStringLiteral("build.linkerPath"), path, QString(), &s->linkerPath, error)
        || !readStringList(build.value(QStringLiteral("additionalSources")),
                           QStringLiteral("build.additionalSources"), path, &additionalSources,
                           error)
        || !readString(build.value(QStringLiteral("cpu")), QStringLiteral("build.cpu"), path,
                       QStringLiteral("68000"), &s->cpu, error)
        || !readStringList(build.value(QStringLiteral("includePaths")),
                           QStringLiteral("build.includePaths"), path, &includePaths, error)
        || !readStringList(build.value(QStringLiteral("defines")), QStringLiteral("build.defines"),
                           path, &s->defines, error)
        || !readStringList(build.value(QStringLiteral("extraArgs")),
                           QStringLiteral("build.extraArgs"), path, &s->extraBuildArgs, error))
        return false;

    s->additionalSources = resolveRelativeToProject(additionalSources, projectDir);
    s->includePaths = resolveRelativeToProject(includePaths, projectDir);

    const QJsonObject emu = root.value(QStringLiteral("emulator")).toObject();
    if (!readString(emu.value(QStringLiteral("hatariPath")), QStringLiteral("emulator.hatariPath"),
                    path, QString(), &s->hatariPath, error))
        return false;

    QString machineName;
    if (!readString(emu.value(QStringLiteral("machine")), QStringLiteral("emulator.machine"), path,
                    QString(), &machineName, error))
        return false;
    Machine machine = Machine::St;
    if (machineFromCliName(machineName, &machine))
        s->machine = machine;

    // A hand-edited or third-party project file must not be able to pass an
    // invalid monitor or an impossible RAM size to the emulator command line,
    // where it would abort startup with no visible explanation.
    QString monitor;
    if (!readString(emu.value(QStringLiteral("monitor")), QStringLiteral("emulator.monitor"), path,
                    QStringLiteral("mono"), &monitor, error))
        return false;
    static const QStringList validMonitors = {QStringLiteral("mono"), QStringLiteral("rgb"),
                                              QStringLiteral("vga"), QStringLiteral("tv")};
    if (validMonitors.contains(monitor))
        s->monitor = monitor;

    int ram = 1;
    if (!readInt(emu.value(QStringLiteral("memSizeMiB")), QStringLiteral("emulator.memSizeMiB"),
                 path, 1, &ram, error))
        return false;
    s->memSizeMiB = qBound(0, ram, 14);
    if (!readString(emu.value(QStringLiteral("tosPath")), QStringLiteral("emulator.tosPath"), path,
                    QString(), &s->tosPath, error))
        return false;
    if (!readString(emu.value(QStringLiteral("hardDiskImage")),
                    QStringLiteral("emulator.hardDiskImage"), path, QString(), &s->hardDiskImage,
                    error))
        return false;
    if (!readStringList(emu.value(QStringLiteral("floppyImages")),
                        QStringLiteral("emulator.floppyImages"), path, &s->floppyImages, error))
        return false;
    if (!readBool(emu.value(QStringLiteral("fastForward")), QStringLiteral("emulator.fastForward"),
                  path, true, &s->fastForward, error))
        return false;
    if (!readStringList(emu.value(QStringLiteral("extraArgs")),
                        QStringLiteral("emulator.extraArgs"), path, &s->extraEmulatorArgs, error))
        return false;
    if (!readString(emu.value(QStringLiteral("debugBackend")),
                    QStringLiteral("emulator.debugBackend"), path, QStringLiteral("auto"),
                    &s->debugBackend, error))
        return false;

    return true;
}

const QString &lastSourceKey()
{
    static const QString key = QStringLiteral("last/source");
    return key;
}

const QString &recentSourcesKey()
{
    static const QString key = QStringLiteral("last/recentSources");
    return key;
}

void rememberLastSource(const QString &sourcePath)
{
    if (sourcePath.isEmpty())
        return;
    QSettings store;
    store.setValue(lastSourceKey(), sourcePath);
    // The recent list feeds the Open Recent submenu: MRU first, each path
    // once, capped so the menu stays a menu.
    QStringList recent = store.value(recentSourcesKey()).toStringList();
    recent.removeAll(sourcePath);
    recent.prepend(sourcePath);
    while (recent.size() > 10)
        recent.removeLast();
    store.setValue(recentSourcesKey(), recent);
}

QString lastSourcePath()
{
    return QSettings().value(lastSourceKey()).toString();
}

QStringList recentSources()
{
    return QSettings().value(recentSourcesKey()).toStringList();
}

} // namespace settings

} // namespace pist
