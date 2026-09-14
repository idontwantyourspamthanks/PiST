// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

struct MachineState;

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
/// prints `$00012596 7001  moveq #$01,d0`, so the `$` is optional. A bare
/// `name:` line labels the instruction that follows it.
void parseDisassembly(const QString &response, MachineState *state);

} // namespace hataritext

} // namespace pist
