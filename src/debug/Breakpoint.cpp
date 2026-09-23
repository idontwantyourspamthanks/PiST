// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "debug/Breakpoint.h"

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

} // namespace pist
