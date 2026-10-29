// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "project/ProjectSettings.h"

#include "support/FileWrite.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace pist {

namespace {

/// The version this build writes, and the newest it can read. Bumping it is what
/// lets a later PiST change the schema without an older build dropping the fields
/// it does not understand and then saving over them.
constexpr int kProjectFileVersion = 2;

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

QStringList defaultDebugAssemblerArgs()
{
    // -devpac for Devpac compatibility (and no optimisation); -opt-x+ to put the
    // DRI/GST symbol table that -devpac suppresses back, with full-length names.
    return {QStringLiteral("-quiet"), QStringLiteral("-devpac"), QStringLiteral("-opt-x+")};
}

QStringList defaultReleaseAssemblerArgs()
{
    // -devpac for the same Devpac build as the debug list; -nosym to strip the
    // symbol table explicitly. -devpac already sets vasm's no_symbols, so the
    // -nosym is redundant — it is kept because it states what the mode is for,
    // and survives the -devpac being changed. See docs/PLAN.md §4.1.
    return {QStringLiteral("-quiet"), QStringLiteral("-devpac"), QStringLiteral("-nosym")};
}

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
    build[QStringLiteral("debugArgs")] = toArray(s.debugAssemblerArgs);
    build[QStringLiteral("releaseArgs")] = toArray(s.releaseAssemblerArgs);
    root[QStringLiteral("build")] = build;

    // Sprite exports. An older PiST ignores an array it does not know, the
    // same way it ignores the hooks object, so the project version stays put:
    // a missing list is "no exports", and writing one does not make an older
    // build refuse the file. A path inside the project is stored relative, so
    // the file still finds the sprite after the project moves.
    const QString projectDir = QFileInfo(path).absolutePath();
    QJsonArray assets;
    for (const AssetExport &asset : s.assets) {
        QJsonObject obj;
        QString source = asset.source;
        if (!source.isEmpty() && QFileInfo(source).isAbsolute()) {
            const QString relative = QDir(projectDir).relativeFilePath(source);
            if (!relative.startsWith(QLatin1String("../")) && relative != QLatin1String(".."))
                source = relative;
        }
        obj[QStringLiteral("source")] = source;
        obj[QStringLiteral("phase")] = asset.phase;
        obj[QStringLiteral("output")] = asset.output;
        obj[QStringLiteral("palette")] = asset.palette;
        obj[QStringLiteral("sprite")] = asset.sprite;
        obj[QStringLiteral("masked")] = asset.masked;
        obj[QStringLiteral("shifted")] = asset.shifted;
        obj[QStringLiteral("shiftedMasked")] = asset.shiftedMasked;
        obj[QStringLiteral("preShifts")] = asset.preShifts;
        obj[QStringLiteral("transparent")] = asset.transparent;
        assets.append(obj);
    }
    root[QStringLiteral("assets")] = assets;

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

    // The stop hook (docs/stop-hooks-plan.md). Written even when it is off, so
    // the file records what the project decided rather than leaving a reader to
    // guess whether the feature was ever configured. An older PiST ignores an
    // object it does not know, so this stays version 2.
    QJsonObject hooks;
    // A path inside the project is stored relative to the project file, so a
    // project that travels (the demo, a repository) carries a script reference
    // that still resolves on the next machine. Loading rebases it the same way
    // every other relative path is rebased (`load()`), which is what makes the
    // round-trip a no-op. An absolute path outside the project stays absolute:
    // there is no relative spelling for it that would mean the same thing.
    QString hookPath = s.stopHookPath;
    if (!hookPath.isEmpty() && QFileInfo(hookPath).isAbsolute()) {
        const QString relative = QDir(projectDir).relativeFilePath(hookPath);
        if (!relative.startsWith(QLatin1String("../")) && relative != QLatin1String(".."))
            hookPath = relative;
    }
    hooks[QStringLiteral("scriptPath")] = hookPath;
    hooks[QStringLiteral("enabled")] = s.stopHookEnabled;
    hooks[QStringLiteral("autoContinue")] = s.stopHookAutoContinue;
    root[QStringLiteral("hooks")] = hooks;

    // The write rule lives in support/FileWrite.h, and the guarantee cannot be
    // delegated to QSaveFile alone: on Qt 6.8.1 — the Qt every release archive
    // bundles — commit() renames a truncated temporary file over the destination
    // and returns true when the flush inside it fails (PLAN §9). A save that
    // cannot reach the disk must leave the project the user had, not a zero-byte
    // file that the next load() reports as invalid.
    return files::write(path, QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

QStringList trustedHookProjects()
{
    return QSettings().value(QStringLiteral("hooks/trustedProjects")).toStringList();
}

namespace {

/// The hash of a script's bytes, or empty when it cannot be read. A hook whose
/// file has gone is not a trust decision that can be remembered.
QString scriptHash(const QString &scriptPath)
{
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

/// One trust record: the project, the script it names, and that script's bytes.
/// JSON rather than a separator so a path containing the separator cannot make
/// two different records compare equal.
QString trustRecord(const QString &projectPath, const QString &scriptPath, const QString &hash)
{
    QJsonObject record;
    record.insert(QStringLiteral("project"), projectPath);
    record.insert(QStringLiteral("script"), scriptPath);
    record.insert(QStringLiteral("sha256"), hash);
    return QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact));
}

} // namespace

void trustHookProject(const QString &projectPath, const QString &scriptPath)
{
    const QString canonicalProject = QFileInfo(projectPath).canonicalFilePath();
    const QString canonicalScript = QFileInfo(scriptPath).canonicalFilePath();
    if (canonicalProject.isEmpty() || canonicalScript.isEmpty())
        return;
    const QString hash = scriptHash(canonicalScript);
    if (hash.isEmpty())
        return;

    const QString record = trustRecord(canonicalProject, canonicalScript, hash);
    QStringList trusted = trustedHookProjects();
    if (trusted.contains(record))
        return;
    trusted.append(record);
    QSettings().setValue(QStringLiteral("hooks/trustedProjects"), trusted);
}

bool hookProjectIsTrusted(const ProjectSettings &settings, const QString &projectPath,
                          const QString &shippedDataDir)
{
    if (!settings.stopHookEnabled || settings.stopHookPath.isEmpty())
        return true;

    const QString canonical = QFileInfo(projectPath).canonicalFilePath();
    if (canonical.isEmpty())
        return true;   // no file yet: nothing has been imported from anywhere

    // A script that shipped with PiST travels with the binary's own data
    // directory, so the demo projects are trusted by provenance.
    if (!shippedDataDir.isEmpty()) {
        const QString shipped = QFileInfo(shippedDataDir).canonicalFilePath();
        if (!shipped.isEmpty() && canonical.startsWith(shipped + QLatin1Char('/')))
            return true;
    }

    const QString script = QFileInfo(settings.stopHookPath).canonicalFilePath();
    if (script.isEmpty())
        return false;
    const QString hash = scriptHash(script);
    if (hash.isEmpty())
        return false;
    return trustedHookProjects().contains(trustRecord(canonical, script, hash));
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
    if (root.contains(QStringLiteral("hooks"))
        && !root.value(QStringLiteral("hooks")).isObject()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'hooks' must be an object"));
        return false;
    }
    if (root.contains(QStringLiteral("assets")) && !root.value(QStringLiteral("assets")).isArray()) {
        if (error)
            *error = invalidEntry(path, QStringLiteral("'assets' must be a list"));
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
                           path, &s->defines, error))
        return false;

    // The assembler argument lists: a key that is present replaces the default,
    // an absent one leaves it. The defaults are deliberately non-empty, so
    // readStringList's own "absent yields empty" would silently discard them.
    if (build.contains(QStringLiteral("debugArgs"))
        && !readStringList(build.value(QStringLiteral("debugArgs")),
                           QStringLiteral("build.debugArgs"), path, &s->debugAssemblerArgs, error))
        return false;
    if (build.contains(QStringLiteral("releaseArgs"))
        && !readStringList(build.value(QStringLiteral("releaseArgs")),
                           QStringLiteral("build.releaseArgs"), path, &s->releaseAssemblerArgs,
                           error))
        return false;

    // A file written before the mode split stored one trailing "extraArgs" list,
    // appended after the managed arguments; there was no release mode, so the
    // list described what is now the debug build. Keyed on the *absent* new key
    // rather than on the version, so a hand-edited or third-party file carrying
    // only the old key still keeps its arguments instead of losing them: either
    // way they are folded into the debug list, never dropped.
    if (!build.contains(QStringLiteral("debugArgs"))) {
        QStringList legacy;
        if (!readStringList(build.value(QStringLiteral("extraArgs")),
                            QStringLiteral("build.extraArgs"), path, &legacy, error))
            return false;
        s->debugAssemblerArgs += legacy;
    }

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

    // The stop hook. An absent object is not an error: it is what every project
    // written before the feature looks like, and the defaults are simply "off".
    const QJsonObject hooks = root.value(QStringLiteral("hooks")).toObject();
    if (!readString(hooks.value(QStringLiteral("scriptPath")),
                    QStringLiteral("hooks.scriptPath"), path, QString(), &s->stopHookPath, error)
        || !readBool(hooks.value(QStringLiteral("enabled")), QStringLiteral("hooks.enabled"), path,
                     false, &s->stopHookEnabled, error)
        || !readBool(hooks.value(QStringLiteral("autoContinue")),
                     QStringLiteral("hooks.autoContinue"), path, false, &s->stopHookAutoContinue,
                     error))
        return false;
    if (!s->stopHookPath.isEmpty() && !QDir::isAbsolutePath(s->stopHookPath))
        s->stopHookPath = QDir::cleanPath(projectDir + QLatin1Char('/') + s->stopHookPath);

    // An absent list is every project written before sprite exports: no assets.
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    s->assets.clear();
    for (int i = 0; i < assets.size(); ++i) {
        if (!assets.at(i).isObject()) {
            if (error)
                *error = invalidEntry(path, QStringLiteral("'assets[%1]' must be an object").arg(i));
            return false;
        }
        const QJsonObject obj = assets.at(i).toObject();
        const QString prefix = QStringLiteral("assets[%1]").arg(i);
        AssetExport asset;
        if (!readString(obj.value(QStringLiteral("source")), prefix + QStringLiteral(".source"), path,
                        QString(), &asset.source, error)
            || !readString(obj.value(QStringLiteral("phase")), prefix + QStringLiteral(".phase"), path,
                           QString(), &asset.phase, error)
            || !readString(obj.value(QStringLiteral("output")), prefix + QStringLiteral(".output"),
                           path, QString(), &asset.output, error)
            || !readBool(obj.value(QStringLiteral("palette")), prefix + QStringLiteral(".palette"),
                         path, true, &asset.palette, error)
            || !readBool(obj.value(QStringLiteral("sprite")), prefix + QStringLiteral(".sprite"), path,
                         true, &asset.sprite, error)
            || !readBool(obj.value(QStringLiteral("masked")), prefix + QStringLiteral(".masked"), path,
                         false, &asset.masked, error)
            || !readBool(obj.value(QStringLiteral("shifted")), prefix + QStringLiteral(".shifted"),
                         path, false, &asset.shifted, error)
            || !readBool(obj.value(QStringLiteral("shiftedMasked")),
                         prefix + QStringLiteral(".shiftedMasked"), path, false, &asset.shiftedMasked,
                         error)
            || !readInt(obj.value(QStringLiteral("preShifts")), prefix + QStringLiteral(".preShifts"),
                        path, 4, &asset.preShifts, error)
            || !readInt(obj.value(QStringLiteral("transparent")),
                        prefix + QStringLiteral(".transparent"), path, 0, &asset.transparent, error))
            return false;
        if (!asset.source.isEmpty() && !QDir::isAbsolutePath(asset.source))
            asset.source = QDir::cleanPath(projectDir + QLatin1Char('/') + asset.source);
        s->assets.append(asset);
    }

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

void renameSource(const QString &oldPath, const QString &newPath)
{
    if (oldPath.isEmpty() || newPath.isEmpty())
        return;
    const QFileInfo oldInfo(oldPath);
    QSettings store;
    if (QFileInfo(store.value(lastSourceKey()).toString()) == oldInfo)
        store.setValue(lastSourceKey(), newPath);

    QStringList recent = store.value(recentSourcesKey()).toStringList();
    bool changed = false;
    for (QString &entry : recent) {
        if (QFileInfo(entry) == oldInfo) {
            entry = newPath;
            changed = true;
        }
    }
    if (changed)
        store.setValue(recentSourcesKey(), recent);
}

namespace {

/// The counter keys. Spelled once each, right here: nothing outside this file
/// increments a counter or reads a raw key, so there is no second spelling to
/// drift (MIN-53).
const QString &sessionCountKey()
{
    static const QString key = QStringLiteral("stats/sessions");
    return key;
}

const QString &buildCountKey()
{
    static const QString key = QStringLiteral("stats/builds");
    return key;
}

const QString &runCountKey()
{
    static const QString key = QStringLiteral("stats/runs");
    return key;
}

const QString &openedFileCountKey()
{
    static const QString key = QStringLiteral("stats/filesOpened");
    return key;
}

const QString &firstLaunchKey()
{
    static const QString key = QStringLiteral("stats/firstLaunch");
    return key;
}

const QString &lastSessionBuildsKey()
{
    static const QString key = QStringLiteral("stats/lastSessionBuilds");
    return key;
}

const QString &lastBuildOkKey()
{
    static const QString key = QStringLiteral("stats/lastBuildOk");
    return key;
}

const QString &lastSessionFileKey()
{
    static const QString key = QStringLiteral("stats/lastSessionFile");
    return key;
}

/// Add one to the counter under @p key. Read-modify-write through one QSettings
/// instance, so the write that follows the read is this instance's own.
int bump(const QString &key)
{
    QSettings store;
    const int value = store.value(key, 0).toInt() + 1;
    store.setValue(key, value);
    return value;
}

} // namespace

int sessionCount()
{
    return QSettings().value(sessionCountKey(), 0).toInt();
}

int buildCount()
{
    return QSettings().value(buildCountKey(), 0).toInt();
}

int runCount()
{
    return QSettings().value(runCountKey(), 0).toInt();
}

int openedFileCount()
{
    return QSettings().value(openedFileCountKey(), 0).toInt();
}

QString firstLaunchDate()
{
    return QSettings().value(firstLaunchKey()).toString();
}

void noteSessionStarted()
{
    bump(sessionCountKey());
    // The first launch is the first launch: the date is written once and never
    // rewritten, so it keeps saying how long this installation has been here.
    QSettings store;
    if (!store.contains(firstLaunchKey()))
        store.setValue(firstLaunchKey(), QDate::currentDate().toString(Qt::ISODate));
}

void noteBuild()
{
    bump(buildCountKey());
}

void noteRun()
{
    bump(runCountKey());
}

void noteFileOpened()
{
    bump(openedFileCountKey());
}

void flushLastSession(const LastSession &session)
{
    QSettings store;
    store.setValue(lastSessionBuildsKey(), session.builds);
    store.setValue(lastBuildOkKey(), session.lastBuildOk);
    store.setValue(lastSessionFileKey(), session.lastFile);
}

LastSession lastSession()
{
    QSettings store;
    LastSession session;
    session.known = store.contains(lastSessionBuildsKey());
    session.builds = store.value(lastSessionBuildsKey(), 0).toInt();
    session.lastBuildOk = store.value(lastBuildOkKey(), false).toBool();
    session.lastFile = store.value(lastSessionFileKey()).toString();
    return session;
}

} // namespace settings

} // namespace pist
