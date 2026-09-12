// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "project/ProjectSettings.h"

#include "emu/Paths.h"
#include "emu/TosRom.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace pist {

namespace {

QJsonArray toArray(const QStringList &list)
{
    QJsonArray array;
    for (const QString &item : list)
        array.append(item);
    return array;
}

QStringList fromArray(const QJsonValue &value)
{
    QStringList list;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array)
        list.append(item.toString());
    return list;
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
    root[QStringLiteral("version")] = 1;

    QJsonObject build;
    build[QStringLiteral("cpu")] = s.cpu;
    build[QStringLiteral("includePaths")] = toArray(s.includePaths);
    build[QStringLiteral("defines")] = toArray(s.defines);
    build[QStringLiteral("extraArgs")] = toArray(s.extraBuildArgs);
    root[QStringLiteral("build")] = build;

    QJsonObject emu;
    emu[QStringLiteral("machine")] = machineCliName(s.machine);
    emu[QStringLiteral("monitor")] = s.monitor;
    emu[QStringLiteral("memSizeMiB")] = s.memSizeMiB;
    emu[QStringLiteral("tosPath")] = s.tosPath;
    emu[QStringLiteral("hardDiskImage")] = s.hardDiskImage;
    emu[QStringLiteral("floppyImages")] = toArray(s.floppyImages);
    emu[QStringLiteral("fastForward")] = s.fastForward;
    emu[QStringLiteral("extraArgs")] = toArray(s.extraEmulatorArgs);
    root[QStringLiteral("emulator")] = emu;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("cannot write '%1': %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    // An ignored short write would report success for a truncated file, and the
    // user would only discover it when the project failed to load later.
    if (file.write(json) != json.size()) {
        if (error)
            *error = QStringLiteral("could not write '%1': %2").arg(path, file.errorString());
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

    const QJsonObject build = root.value(QStringLiteral("build")).toObject();
    s->cpu = build.value(QStringLiteral("cpu")).toString(QStringLiteral("68000"));
    s->includePaths = fromArray(build.value(QStringLiteral("includePaths")));
    s->defines = fromArray(build.value(QStringLiteral("defines")));
    s->extraBuildArgs = fromArray(build.value(QStringLiteral("extraArgs")));

    const QJsonObject emu = root.value(QStringLiteral("emulator")).toObject();
    Machine machine = Machine::St;
    if (machineFromCliName(emu.value(QStringLiteral("machine")).toString(), &machine))
        s->machine = machine;
    // A hand-edited or third-party project file must not be able to pass an
    // invalid monitor or an impossible RAM size to the emulator command line,
    // where it would abort startup with no visible explanation.
    const QString monitor = emu.value(QStringLiteral("monitor")).toString(QStringLiteral("mono"));
    static const QStringList validMonitors = {QStringLiteral("mono"), QStringLiteral("rgb"),
                                              QStringLiteral("vga"), QStringLiteral("tv")};
    if (validMonitors.contains(monitor))
        s->monitor = monitor;

    const int ram = emu.value(QStringLiteral("memSizeMiB")).toInt(1);
    s->memSizeMiB = qBound(0, ram, 14);
    s->tosPath = emu.value(QStringLiteral("tosPath")).toString();
    s->hardDiskImage = emu.value(QStringLiteral("hardDiskImage")).toString();
    s->floppyImages = fromArray(emu.value(QStringLiteral("floppyImages")));
    s->fastForward = emu.value(QStringLiteral("fastForward")).toBool(true);
    s->extraEmulatorArgs = fromArray(emu.value(QStringLiteral("extraArgs")));

    return true;
}

void rememberLastProject(const QString &projectPath, const QString &sourcePath)
{
    QSettings store;
    if (!projectPath.isEmpty())
        store.setValue(QStringLiteral("last/project"), projectPath);
    if (!sourcePath.isEmpty())
        store.setValue(QStringLiteral("last/source"), sourcePath);
}

QString lastProjectPath()
{
    return QSettings().value(QStringLiteral("last/project")).toString();
}

QString lastSourcePath()
{
    return QSettings().value(QStringLiteral("last/source")).toString();
}

} // namespace settings

} // namespace pist
