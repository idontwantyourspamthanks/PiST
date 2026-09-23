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
            continue; // Pexec's varargs: the fixedArgBlock layout covers them

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

/// One push of the canonical binding: 4 bytes for a long (or a pointer, which
/// the binding pushes with `pea`), 2 for a word.
int pushBytes(const Param &p)
{
    return p.isLong ? 4 : 2;
}

} // namespace

int osCallStackBytes(const OsCallInfo &info)
{
    // The function number's word, the caller's arguments, then the reserved
    // words the call's binding adds. Pexec's fixed block is three longs.
    int bytes = 2;
    const QList<Param> params = parseParams(info.prototype);
    for (const Param &p : params)
        bytes += pushBytes(p);
    bytes += 2 * info.reservedWords;
    if (info.fixedArgBlock)
        bytes += 3 * 4; // env, cmdline, name
    return bytes;
}

QString osCallBinding(const OsCallInfo &info)
{
    QStringList lines;

    // Arguments in reverse declaration order (the last parameter is pushed
    // first), each as the canonical binding writes it: pea for pointers,
    // move.l for long values, move.w for words. Pexec's fixed argument block is
    // deeper than every caller-supplied one, so it is pushed first; the
    // reserved words sit above the arguments, next to the function number.
    const QList<Param> params = parseParams(info.prototype);
    if (info.fixedArgBlock) {
        lines << QStringLiteral("\tpea\tenv");
        lines << QStringLiteral("\tpea\tcmdline");
        lines << QStringLiteral("\tpea\tname");
    }
    for (int i = params.size() - 1; i >= 0; --i) {
        if (i == info.reservedArg) {
            // An argument the call fixes rather than the caller: Dbmsg's
            // reserved word, which must be 5. Keyed on the argument's position,
            // so renaming the parameter changes nothing.
            lines << QStringLiteral("\tmove.w\t#%1,-(sp)").arg(info.reservedArgValue);
            continue;
        }
        const Param &p = params.at(i);
        if (p.isPointer) {
            lines << QStringLiteral("\tpea\t%1").arg(p.name);
        } else if (p.isLong) {
            lines << QStringLiteral("\tmove.l\t#%1,-(sp)").arg(p.name);
        } else {
            lines << QStringLiteral("\tmove.w\t#%1,-(sp)").arg(p.name);
        }
    }
    // Mshrink and Frename push a reserved zero word between their arguments
    // and the function number (the C binding adds it silently).
    for (int i = 0; i < info.reservedWords; ++i)
        lines << QStringLiteral("\tmove.w\t#0,-(sp)");

    lines << QStringLiteral("\tmove.w\t#%1,-(sp)\t; %2 %3")
                 .arg(info.opcode)
                 .arg(osCallLayerName(info.trap), info.name);
    lines << QStringLiteral("\ttrap\t#%1").arg(info.trap);

    // The cleanup is derived from the layout, never from a hand-written size:
    // the pushes above and the number the caller pops cannot disagree.
    const int bytes = osCallStackBytes(info);
    if (bytes <= 8)
        lines << QStringLiteral("\taddq.l\t#%1,sp").arg(bytes);
    else
        lines << QStringLiteral("\tlea\t%1(sp),sp").arg(bytes);

    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace pist
