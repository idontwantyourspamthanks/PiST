// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QMetaType>
#include <QString>

namespace pist {

struct MachineState;

/// What an `info <subject>` report says, in the shape a pane displays it.
///
/// The report is Hatari's text, and the fields below are the parts of it that
/// are state rather than prose — the screen address, the refresh rate, the
/// overscan mode. They are parsed here, at the transport's edge, so the pane
/// only renders what it is handed (MAJ-45); the transcript travels with them
/// because the pane shows it verbatim under the summary.
///
/// Only the `info video` report carries any of the three (the MFP, ACIA, IKBD,
/// YM and blitter reports are read as transcripts), so a report without them
/// leaves the flags at their defaults and the pane shows no summary line.
struct HardwareSummary
{
    /// The report exactly as the debugger printed it.
    QString transcript;
    /// `Video base` — the screen address — and whether the report stated one.
    quint32 screenBase = 0;
    bool hasScreenBase = false;
    /// `Refresh rate … Hz`; 0 when the report does not state one.
    int refreshHz = 0;
    /// The `V-overscan` token as printed ("none", "top+bottom", …); empty when
    /// the report does not state one.
    QString overscan;
};

/// Parsers for Hatari's *text* debugger output, shared by both backends: the
/// native transport reads it from stderr, and the HRDB backend gets the same
/// text back from the fork's `console` command (which runs the same debugui
/// commands upstream does). One parser, because the format is Hatari's, not
/// the transport's.
namespace hataritext {

/// Parse `d` output into state->disassembly, marking the line at state->pc.
///
/// The address prefix depends on which disassembler engine is active, which is
/// a *user configuration* setting (`bDisasmUAE`), not a build property: the UAE
/// engine (the default) prints `00012596 7001  moveq #$01,d0` while Capstone
/// prints `$00012596 7001  moveq #$01,d0`, so the `$` is optional. (Spacing is
/// abbreviated here: in real output the byte column is padded out to a fixed
/// text column, which is the field boundary the parser splits on.)
/// A bare `name:` line labels the instruction that follows it.
void parseDisassembly(const QString &response, MachineState *state);

/// Parse an `info <subject>` report into the summary a pane displays.
HardwareSummary parseHardwareInfo(const QString &response);

} // namespace hataritext

} // namespace pist

// The summary crosses the backend boundary on hardwareInfoReady(), so it is a
// Qt metatype; the backends that emit it register it.
Q_DECLARE_METATYPE(pist::HardwareSummary)
