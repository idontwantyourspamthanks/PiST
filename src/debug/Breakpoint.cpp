// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "debug/Breakpoint.h"

#include <QDir>
#include <QFileInfo>

namespace pist {

namespace {

/// Whether Hatari's breakpoint grammar accepts `condition`.
///
/// BreakCond_TokenizeExpression (breakcond.c) drops whitespace, separates the
/// comparisons and `&`, and rejects every other character: a `|` is an
/// "invalid character", so `a || b` is refused outright. It also demands at
/// least one comparison — a bare value fails with "condition comparison
/// missing". A refused condition makes `b` fail, so the breakpoint never arms
/// while the panel still shows it as set; validating here turns that into a
/// reported unresolved breakpoint with a reason (MIN-10).
///
/// There is no term limit: BreakCond_ParseCondition recurses on every `&&`, so
/// a chain longer than three is legal. The finding's ">3 terms violates the
/// grammar" did not reproduce against Hatari 2.6.1.
bool conditionAccepted(const QString &condition, QString *reason)
{
    const QString trimmed = condition.trimmed();
    if (trimmed.isEmpty())
        return true;

    bool hasComparison = false;
    for (const QChar c : trimmed) {
        if (c.isSpace())
            continue;
        if (c == QLatin1Char('=') || c == QLatin1Char('!') || c == QLatin1Char('<')
            || c == QLatin1Char('>')) {
            hasComparison = true;
            continue;
        }
        const bool allowed = c.isLetterOrNumber() || c == QLatin1Char('_')
                             || c == QLatin1Char(':') || c == QLatin1Char('$')
                             || c == QLatin1Char('#') || c == QLatin1Char('%')
                             || c == QLatin1Char('(') || c == QLatin1Char(')')
                             || c == QLatin1Char('.') || c == QLatin1Char('&');
        if (!allowed) {
            *reason = QStringLiteral("its condition contains '%1', which Hatari's breakpoint "
                                     "grammar does not accept").arg(c);
            return false;
        }
    }
    if (!hasComparison) {
        *reason = QStringLiteral("its condition has no comparison (=, !, < or >)");
        return false;
    }
    // `&&` separates the comparisons; an empty term beside one is content
    // Hatari's parser also rejects ("trailing content for breakpoint
    // condition").
    const QStringList terms = trimmed.split(QStringLiteral("&&"));
    for (const QString &term : terms) {
        if (term.trimmed().isEmpty()) {
            *reason = QStringLiteral("its condition has an empty comparison beside '&&'");
            return false;
        }
    }
    return true;
}

} // namespace

ArmPlan planBreakpoints(const QList<Breakpoint> &breakpoints,
                        const ProgramLineMap &lineMap)
{
    ArmPlan plan;

    for (const Breakpoint &bp : breakpoints) {
        if (!bp.isResolvable()) {
            if (!bp.enabled)
                continue; // disabled is not the same as unresolvable
            plan.unresolved.append(bp.label());
            plan.unresolvedReasons.append(QStringLiteral("it is not anchored to a line"));
            continue;
        }

        // The condition is ANDed into the command; one the debugger's grammar
        // refuses must not be armed at all, or the breakpoint silently never
        // fires (MIN-10).
        QString conditionReason;
        if (!conditionAccepted(bp.condition, &conditionReason)) {
            plan.unresolved.append(bp.label());
            plan.unresolvedReasons.append(conditionReason);
            continue;
        }

        quint32 address = 0;
        if (!lineMap.codeAddressFor(bp.file, bp.line, &address)) {
            // No executable code here: normal for a comment, blank line, or a
            // directive that emits nothing — and, crucially, for a `dc.b`/`ds`
            // data line, which the general addressFor would resolve to a data
            // address that a breakpoint can never fire at (finding B10).
            plan.unresolved.append(bp.label());
            plan.unresolvedReasons.append(QStringLiteral("it emits no code or data"));
            continue;
        }

        Breakpoint resolved = bp;
        resolved.address = address;
        resolved.resolved = true;
        plan.armed.append(resolved);

        // Hatari's `b` takes a condition, not a bare address: `b loop` fails
        // with "condition comparison missing". A literal address works because
        // it is evaluable, but the explicit `pc =` form is unambiguous and is
        // what the address form is converted to internally anyway.
        QString command = QStringLiteral("b pc = $%1").arg(address, 0, 16);
        if (!bp.condition.trimmed().isEmpty())
            command += QStringLiteral(" && ") + bp.condition.trimmed();
        plan.commands.append(command);
    }

    return plan;
}

namespace {

/// Why a named watchpoint could not be resolved, in the watchpoint's own voice.
///
/// `labelAddress()` gives the cause and lets the caller phrase it, because the
/// reader differs: a script error and an arm-time log line are not the same
/// sentence. This is the log's, and it reads as the tail of
/// `[watchpoints] watch counter.l: <reason>`.
QString unresolvedReason(const QString &name, LabelAddressProblem problem)
{
    switch (problem) {
    case LabelAddressProblem::NoSourcePosition:
        return QStringLiteral("'%1' has no source position (a command-line define, or a name "
                              "a macro built)")
            .arg(name);
    case LabelAddressProblem::Equate:
        return QStringLiteral("'%1' is an equate — it has a value, not a place, and a watchpoint "
                              "needs one")
            .arg(name);
    case LabelAddressProblem::NoAddress:
        return QStringLiteral("'%1' emits nothing, and nothing after it does either").arg(name);
    case LabelAddressProblem::NotLive:
        return QStringLiteral("'%1' has no address yet — no stop has reported the program's bases")
            .arg(name);
    case LabelAddressProblem::UnknownName:
        return QStringLiteral("no symbol named '%1' (has the project been built?)").arg(name);
    case LabelAddressProblem::None:
        break;
    }
    return QStringLiteral("'%1' could not be resolved to an address").arg(name);
}

} // namespace

WatchpointPlan planWatchpoints(const QList<Watchpoint> &watchpoints,
                               const QVector<SymbolEntry> &symbols,
                               const ProgramLineMap &lineMap)
{
    WatchpointPlan plan;

    for (const Watchpoint &wp : watchpoints) {
        if (wp.symbol.isEmpty()) {
            // An address means the same thing before the program is loaded as
            // after, so there is nothing to resolve: it is already resolved.
            Watchpoint armed = wp;
            armed.resolved = true;
            plan.armed.append(armed);
            plan.commands.append(armed.command());
            continue;
        }

        LabelAddress where;
        LabelAddressProblem problem = LabelAddressProblem::None;
        if (!labelAddress(symbols, lineMap, wp.symbol, &where, &problem)) {
            plan.unresolved.append(wp.label());
            plan.unresolvedReasons.append(unresolvedReason(wp.symbol, problem));
            continue;
        }

        Watchpoint armed = wp;
        armed.address = where.address;
        armed.resolved = true;
        plan.armed.append(armed);
        plan.commands.append(armed.command());
    }

    return plan;
}

QList<Breakpoint> renameFile(const QList<Breakpoint> &breakpoints,
                             const QString &oldFile, const QString &newFile)
{
    // Rebuild in input order, rewriting only the entries keyed to the old name.
    // Everything else is copied verbatim, so a field the caller did not touch
    // cannot be lost by the migration.
    QList<Breakpoint> migrated;
    migrated.reserve(breakpoints.size());
    for (const Breakpoint &bp : breakpoints) {
        Breakpoint entry = bp;
        if (entry.file == oldFile)
            entry.file = newFile;
        migrated.append(entry);
    }
    return migrated;
}

const Breakpoint *breakpointAt(const QList<Breakpoint> &breakpoints, quint32 pc)
{
    for (const Breakpoint &bp : breakpoints) {
        // `resolved` is the arming gate's word: address 0 belongs to a breakpoint
        // no session ever armed, and code never runs there.
        if (bp.resolved && bp.address == pc)
            return &bp;
    }
    return nullptr;
}

QString resolveScriptPath(const QString &typed, const QString &sourceFile)
{
    const QString path = typed.trimmed();
    if (path.isEmpty() || QDir::isAbsolutePath(path))
        return path;

    return QDir::cleanPath(QFileInfo(sourceFile).absolutePath() + QLatin1Char('/') + path);
}

} // namespace pist
