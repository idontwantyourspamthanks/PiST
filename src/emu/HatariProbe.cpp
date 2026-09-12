// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#include "emu/HatariProbe.h"

#include <QProcess>
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
    if (hasBacktraceCommand)
        notes << QObject::tr("backtrace: yes");
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
        caps.hasCmdFifo = help.contains(QLatin1String("--cmd-fifo"));
        caps.hasSymbolAutoloadOption = help.contains(QLatin1String("--symload"));
        caps.hasConout = help.contains(QLatin1String("--conout"));
    }

    return caps;
}

} // namespace pist
