// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/OsCallBinding.h"

#include "editor/OsCallRef.h"

#include <QRegularExpression>
#include <QStringList>

namespace pist {

namespace {

/// One parameter of a parsed prototype: its placeholder name and the push
/// that puts it on the stack.
struct Param
{
    QString name;
    bool isLong;
    bool isPointer; // long, but pushed with pea (canonical binding style)
};

/// Parse the parameter list of a C prototype ("int32_t Fread(int16_t handle,
/// int32_t count, void *buf)"). Every prototype in the table is plain ints
/// and pointers — no by-value structs, no nested commas — so a flat split
/// suffices. Function-pointer parameters ("void (*mousevec)()") are longs.
QList<Param> parseParams(const QString &prototype)
{
    const int open = prototype.indexOf(QLatin1Char('('));
    const int close = prototype.lastIndexOf(QLatin1Char(')'));
    const QString inside = prototype.mid(open + 1, close - open - 1).trimmed();
    if (inside.isEmpty() || inside == QLatin1String("void"))
        return {};

    static const QRegularExpression fnPtrName(QStringLiteral("\\(\\*(\\w+)\\)"));
    static const QRegularExpression trailingName(QStringLiteral("(\\w+)\\s*$"));

    QList<Param> params;
    const QStringList parts = inside.split(QLatin1Char(','));
    for (const QString &raw : parts) {
        const QString part = raw.trimmed();
        if (part == QLatin1String("..."))
            continue; // Pexec's varargs: handled by the fixed layout below

        Param p;
        p.isPointer = part.contains(QLatin1Char('*'));
        p.isLong = p.isPointer || part.contains(QStringLiteral("32_t"))
                   || part.contains(QLatin1String("long"), Qt::CaseInsensitive);

        const QRegularExpressionMatch fnPtr = fnPtrName.match(part);
        if (fnPtr.hasMatch()) {
            p.name = fnPtr.captured(1);
        } else {
            const QRegularExpressionMatch name = trailingName.match(part);
            p.name = name.hasMatch() ? name.captured(1) : QStringLiteral("arg");
        }
        params.append(p);
    }
    return params;
}

} // namespace

QString osCallBinding(const OsCallInfo &info)
{
    QStringList lines;

    // Arguments in reverse declaration order (the last parameter is pushed
    // first), each as the canonical binding writes it: pea for pointers,
    // move.l for long values, move.w for words.
    const QList<Param> params = parseParams(info.prototype);
    const bool isPexec = (info.trap == 1 && info.opcode == 75);
    if (isPexec) {
        // The varargs call has one fixed layout: three longs after the mode.
        lines << QStringLiteral("\tpea\tenv");
        lines << QStringLiteral("\tpea\tcmdline");
        lines << QStringLiteral("\tpea\tname");
    }
    for (int i = params.size() - 1; i >= 0; --i) {
        const Param &p = params.at(i);
        if (info.trap == 14 && info.opcode == 11 && p.name == QLatin1String("rsrvd")) {
            // Dbmsg's reserved word is not a parameter to fill in: it must be 5.
            lines << QStringLiteral("\tmove.w\t#5,-(sp)");
        } else if (p.isPointer) {
            lines << QStringLiteral("\tpea\t%1").arg(p.name);
        } else if (p.isLong) {
            lines << QStringLiteral("\tmove.l\t#%1,-(sp)").arg(p.name);
        } else {
            lines << QStringLiteral("\tmove.w\t#%1,-(sp)").arg(p.name);
        }
    }
    // Mshrink and Frename push a reserved zero word between their arguments
    // and the function number (the C binding adds it silently).
    if ((info.trap == 1 && info.opcode == 74) || (info.trap == 1 && info.opcode == 86))
        lines << QStringLiteral("\tmove.w\t#0,-(sp)");

    lines << QStringLiteral("\tmove.w\t#%1,-(sp)\t; %2 %3")
                 .arg(info.opcode)
                 .arg(osCallLayerName(info.trap), info.name);
    lines << QStringLiteral("\ttrap\t#%1").arg(info.trap);
    if (info.stackBytes <= 8)
        lines << QStringLiteral("\taddq.l\t#%1,sp").arg(info.stackBytes);
    else
        lines << QStringLiteral("\tlea\t%1(sp),sp").arg(info.stackBytes);

    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace pist
