// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HatariTextParse.h"

#include "emu/MachineState.h"

#include <QRegularExpression>

namespace pist {
namespace hataritext {

namespace {

/// `$00010030 b07c 0064                          cmp.w     #$64,d0`
/// `$00010014 54c8 000a                          dbcc      d0,$10020`
///
/// The `$` prefix is optional: Hatari's Capstone disassembler emits it, the UAE
/// default prints the bare address, and this matches both. The byte column is
/// optional in the pattern, but `parseDisassembly` runs only on the `d`
/// command's output, where every line carries one — the register dump's byte-less
/// inline instruction is parsed separately (pcLineRe in EmulatorHost), so it never
/// reaches here.
///
/// The byte column is a run of hex words separated by a *single* space, then
/// padded out to the fixed text column. Those two facts are the split: a group
/// ends where the alignment gap begins and the run may never step across that
/// gap, so a hex-only mnemonic standing in the text column (`dbf`, `dbcc`,
/// `abcd`) stays out of the byte column. A run that accepted any amount of
/// whitespace between words absorbed the mnemonic along with the padding and
/// left `dl.instruction` holding nothing but the operands, which the pane
/// rendered truncated and the step-over heuristics could read no mnemonic from
/// (MAJ-57).
///
/// A ten-byte instruction overruns the column and Hatari cuts its last word
/// short with a `+` (`23fc 1234 5678 0001 23+`), so that cut token is part of
/// the byte column too — otherwise its remains lead the instruction text.
const QRegularExpression &disasmRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\$?([0-9A-Fa-f]{6,8})\s+((?:[0-9A-Fa-f]{2,4} )*(?:[0-9A-Fa-f]{1,4}\+)?)\s*(\S.*)?$)"),
        QRegularExpression::MultilineOption);
    return re;
}

// Only the lines `info video` actually prints. Resolution and the palette are
// not among them, so a summary does not invent them: the header states what
// the transcript states.
const QRegularExpression &videoBaseRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Video base\s*:\s*(?:0x|\$)?([0-9A-Fa-f]+))"));
    return re;
}

const QRegularExpression &refreshRateRe()
{
    static const QRegularExpression re(QStringLiteral(R"(Refresh rate\s*:\s*(\d+)\s*Hz)"));
    return re;
}

const QRegularExpression &overscanRe()
{
    static const QRegularExpression re(QStringLiteral(R"(V-overscan\s*:\s*(\S+))"));
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

HardwareSummary parseHardwareInfo(const QString &response)
{
    HardwareSummary summary;
    summary.transcript = response;

    const auto base = videoBaseRe().match(response);
    if (base.hasMatch()) {
        summary.screenBase = base.captured(1).toUInt(nullptr, 16);
        summary.hasScreenBase = true;
    }
    const auto rate = refreshRateRe().match(response);
    if (rate.hasMatch())
        summary.refreshHz = rate.captured(1).toInt();
    const auto overscan = overscanRe().match(response);
    if (overscan.hasMatch())
        summary.overscan = overscan.captured(1);
    return summary;
}

DmaSoundInfo parseDmaSoundInfo(const QString &response)
{
    DmaSoundInfo info;
    if (response.contains(QStringLiteral("ST doesn't include DMA!")))
        return info;

    const auto take = [&](const char *pattern) -> int {
        const auto match = QRegularExpression(QLatin1String(pattern)).match(response);
        if (!match.hasMatch())
            return -1;
        bool ok = false;
        const int value = match.captured(1).toInt(&ok, 16);
        return ok ? value : -1;
    };
    const int startHi = take(R"(\$FF8903\.b : Frame Start High : ([0-9A-Fa-f]{2}))");
    const int startMid = take(R"(\$FF8905\.b : Frame Start middle : ([0-9A-Fa-f]{2}))");
    const int startLo = take(R"(\$FF8907\.b : Frame Start low : ([0-9A-Fa-f]{2}))");
    const int endHi = take(R"(\$FF890F\.b : Frame End High : ([0-9A-Fa-f]{2}))");
    const int endMid = take(R"(\$FF8911\.b : Frame End middle : ([0-9A-Fa-f]{2}))");
    const int endLo = take(R"(\$FF8913\.b : Frame End low : ([0-9A-Fa-f]{2}))");
    const int mode = take(R"(\$FF8921\.b : Sound Mode Control : ([0-9A-Fa-f]{2}))");
    if (startHi < 0 || startMid < 0 || startLo < 0 || endHi < 0 || endMid < 0 || endLo < 0
        || mode < 0)
        return info;

    info.start = (quint32(startHi) << 16) | (quint32(startMid) << 8) | quint32(startLo);
    info.start &= ~1u;
    info.end = (quint32(endHi) << 16) | (quint32(endMid) << 8) | quint32(endLo);
    info.end &= ~1u;
    static const int kRates[] = {6258, 12517, 25033, 50066};
    info.rateHz = kRates[mode & 3];
    info.stereo = (mode & 0x80) == 0;
    info.present = true;
    return info;
}

} // namespace hataritext
} // namespace pist
