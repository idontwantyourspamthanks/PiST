// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "debug/Breakpoint.h"

namespace pist {

ArmPlan planBreakpoints(const QList<Breakpoint> &breakpoints,
                        const LineMap &lineMap,
                        const LineMap::SectionBases &bases)
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
        if (!lineMap.addressFor(bp.file, bp.line, bases, &address)) {
            // The line produced no code or data, which is normal for a comment,
            // a blank line, or a directive that emits nothing.
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
