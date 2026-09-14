// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HatariProbe.h"

#include <QProcess>
#include <QFile>
#include <QRegularExpression>

namespace pist {

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

    {
        QProcess p;
        p.start(hatariPath, {QStringLiteral("--version")});
        if (!p.waitForStarted(3000) || !p.waitForFinished(5000))
            return caps;
        const QString out = QString::fromUtf8(p.readAllStandardOutput())
                          + QString::fromUtf8(p.readAllStandardError());
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
        QProcess p;
        p.start(hatariPath, {QStringLiteral("-h")});
        if (!p.waitForStarted(3000) || !p.waitForFinished(8000))
            return caps;
        const QString help = QString::fromUtf8(p.readAllStandardOutput())
                           + QString::fromUtf8(p.readAllStandardError());

        // Probe by option name rather than by version number: distro builds and
        // forks do not track upstream's versioning exactly.
        caps.hasControlSocket = help.contains(QLatin1String("--control-socket"));
        caps.hasSymbolAutoloadOption = help.contains(QLatin1String("--symload"));
        caps.hasDebugExcept = help.contains(QLatin1String("--debug-except"));
        caps.hasParse = help.contains(QLatin1String("--parse"));
    }

    // The HRDB fork is version-identical to upstream and adds no CLI option,
    // so it cannot be probed by version or option name. Its listener's banner
    // string is in the binary and upstream's never is, so scan the content.
    // Chunked with an overlap window so a large binary is never wholly
    // resident and a split needle is not missed.
    if (caps.valid) {
        const QByteArray needle = QByteArrayLiteral("Remote Debug Listening on port");
        QFile f(hatariPath);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray tail;
            for (;;) {
                const QByteArray read = f.read(1 << 20);
                if (read.isEmpty())
                    break;  // EOF
                const QByteArray chunk = tail + read;
                if (chunk.contains(needle)) {
                    caps.hasHrdb = true;
                    break;
                }
                tail = chunk.right(needle.size());
            }
        }
    }

    return caps;
}

} // namespace pist
