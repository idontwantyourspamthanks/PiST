// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HatariTextParse.h"

#include "emu/MachineState.h"

#include <QRegularExpression>

namespace pist {
namespace hataritext {

namespace {

/// `$0125a2 60fe                     bra.b     $125a2`
///
/// The `$` prefix marks the Capstone engine; the UAE default prints the bare
/// address. The register dump's inline instruction line has no byte column and
/// is deliberately not matched.
const QRegularExpression &disasmRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\$?([0-9A-Fa-f]{6,8})\s+((?:[0-9A-Fa-f]{2,4}\s+)*)(\S.*)?$)"),
        QRegularExpression::MultilineOption);
    return re;
}

} // namespace

void parseDisassembly(const QString &response, MachineState *state)
{
    state->disassembly.clear();

    QString pendingLabel;
    const QStringList lines = response.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed == QLatin1String("(PC)"))
            continue;

        // A bare `name:` line labels the instruction that follows.
        if (trimmed.endsWith(QLatin1Char(':')) && !trimmed.contains(QLatin1Char(' '))) {
            pendingLabel = trimmed.left(trimmed.length() - 1);
            continue;
        }

        const auto m = disasmRe().match(trimmed);
        if (!m.hasMatch())
            continue;

        DisasmLine dl;
        dl.address = m.captured(1).toUInt(nullptr, 16);
        dl.bytes = m.captured(2).trimmed();
        dl.instruction = m.captured(3).trimmed();
        dl.label = pendingLabel;
        pendingLabel.clear();
        dl.isCurrentPc = (dl.address == state->pc);
        state->disassembly.append(dl);
    }
}

} // namespace hataritext
} // namespace pist
