// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "debug/Breakpoint.h"

namespace pist {

ArmPlan planBreakpoints(const QList<Breakpoint> &breakpoints,
                        const ProgramLineMap &lineMap)
{
    ArmPlan plan;

    for (const Breakpoint &bp : breakpoints) {
        if (!bp.isResolvable()) {
            if (!bp.enabled)
                continue; // disabled is not the same as unresolvable
            plan.unresolved.append(bp.label());
            continue;
        }

        quint32 address = 0;
        if (!lineMap.codeAddressFor(bp.file, bp.line, &address)) {
            // No executable code here: normal for a comment, blank line, or a
            // directive that emits nothing — and, crucially, for a `dc.b`/`ds`
            // data line, which the general addressFor would resolve to a data
            // address that a breakpoint can never fire at (finding B10).
            plan.unresolved.append(bp.label());
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
