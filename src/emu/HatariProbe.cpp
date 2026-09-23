// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HatariProbe.h"

#include "build/ProcessUtil.h"

#include <QFile>
#include <QRegularExpression>

namespace pist {

namespace {

/// One chunked pass over the binary collecting everything content can answer:
/// the HRDB banner (the fork adds no CLI option, so it cannot be probed by
/// option name), the option-table strings, and the "Hatari vX.Y.Z" banner
/// (version.h's PROG_NAME). Chunked with an overlap window so a large binary
/// is never wholly resident and a split needle is not missed.
void scanBinaryContent(const QString &hatariPath, HatariCapabilities *caps)
{
    QFile f(hatariPath);
    if (!f.open(QIODevice::ReadOnly))
        return;

    static const QRegularExpression versionRe(
        QStringLiteral(R"(Hatari v(\d+)\.(\d+)(?:\.(\d+))?)"));
    QByteArray tail;
    for (;;) {
        const QByteArray read = f.read(1 << 20);
        if (read.isEmpty())
            break;  // EOF
        const QByteArray chunk = tail + read;
        if (!caps->hasHrdb
            && chunk.contains(QByteArrayLiteral("Remote Debug Listening on port")))
            caps->hasHrdb = true;
        if (!caps->hasControlSocket && chunk.contains(QByteArrayLiteral("--control-socket")))
            caps->hasControlSocket = true;
        if (!caps->hasSymbolAutoloadOption && chunk.contains(QByteArrayLiteral("--symload")))
            caps->hasSymbolAutoloadOption = true;
        if (!caps->hasDebugExcept && chunk.contains(QByteArrayLiteral("--debug-except")))
            caps->hasDebugExcept = true;
        if (caps->version.isEmpty()) {
            const auto m = versionRe.match(QString::fromLatin1(chunk));
            if (m.hasMatch()) {
                caps->versionMajor = m.captured(1).toInt();
                caps->versionMinor = m.captured(2).toInt();
                caps->versionPatch = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
                caps->version = QStringLiteral("%1.%2.%3")
                                    .arg(caps->versionMajor)
                                    .arg(caps->versionMinor)
                                    .arg(caps->versionPatch);
            }
        }
        tail = chunk.right(64);
    }
}

} // namespace

QString HatariCapabilities::summary() const
{
    if (!valid)
        return QObject::tr("Hatari not found");

    QStringList notes;
    notes << QStringLiteral("Hatari %1").arg(version);
    notes << (hasControlSocket ? QObject::tr("control socket: yes")
                               : QObject::tr("control socket: NO"));
    if (hasSymbolAutoloadOption)
        notes << QObject::tr("--symload: yes");
    if (hasHrdb)
        notes << QObject::tr("remote debug: yes");
    return notes.join(QStringLiteral(" · "));
}

HatariCapabilities probeHatari(const QString &hatariPath)
{
    HatariCapabilities caps;
    if (hatariPath.isEmpty())
        return caps;

#ifdef Q_OS_WIN
    // Windows info options are useless over a pipe: Opt_ShowVersion (which
    // both --version and -h run) forces a NEW console and prints there
    // (options.c -> Win_ForceCon -> AllocConsole + freopen("CON")), after
    // which Main_ErrorExit waits for Enter — so a process probe pops a
    // console window, blocks until the timeout, and returns nothing. The
    // option table and the version banner are in the binary itself, and the
    // content probe answers everything a Windows session needs to know.
    scanBinaryContent(hatariPath, &caps);
    caps.valid = !caps.version.isEmpty();
    return caps;
#else
    {
        // A probe answers immediately or not at all: the two timeouts are what
        // bound "a distro's hatari may answer -h slowly", not a real workload.
        RunOptions options;
        options.startTimeoutMs = 3000;
        options.finishTimeoutMs = 5000;
        const SyncRun version =
            runSync(hatariPath, {QStringLiteral("--version")}, options);
        if (!version.started || !version.finished)
            return caps;
        const QString out = version.output;
        caps.valid = true;

        static const QRegularExpression re(QStringLiteral(R"(v(\d+)\.(\d+)(?:\.(\d+))?)"));
        auto m = re.match(out);
        if (m.hasMatch()) {
            caps.versionMajor = m.captured(1).toInt();
            caps.versionMinor = m.captured(2).toInt();
            caps.versionPatch = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
            caps.version = QStringLiteral("%1.%2.%3")
                               .arg(caps.versionMajor)
                               .arg(caps.versionMinor)
                               .arg(caps.versionPatch);
        }
    }

    {
        // The option table is a longer answer than --version, so it gets a
        // longer budget; nothing else differs.
        RunOptions options;
        options.startTimeoutMs = 3000;
        options.finishTimeoutMs = 8000;
        const SyncRun run = runSync(hatariPath, {QStringLiteral("-h")}, options);
        if (!run.started || !run.finished)
            return caps;
        const QString help = run.output;

        // Probe by option name rather than by version number: distro builds and
        // forks do not track upstream's versioning exactly.
        caps.hasControlSocket = help.contains(QLatin1String("--control-socket"));
        caps.hasSymbolAutoloadOption = help.contains(QLatin1String("--symload"));
        caps.hasDebugExcept = help.contains(QLatin1String("--debug-except"));
    }

    // The HRDB fork is version-identical to upstream and adds no CLI option,
    // so it cannot be probed by version or option name. Its listener's banner
    // string is in the binary and upstream's never is, so scan the content.
    if (caps.valid)
        scanBinaryContent(hatariPath, &caps);

    return caps;
#endif
}

} // namespace pist
