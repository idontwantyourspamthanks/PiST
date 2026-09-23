// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {
namespace control {

/// The wire vocabulary RemoteControl serves and the MCP shim drives.
///
/// The verbs used to be spelled as literals in four places — the server's
/// dispatch, its help text, the shim's tool mapping and its block-framing
/// flags — so the four could disagree silently (finding MIN-52). This table is
/// the one place the vocabulary is written down: `help` is generated from it,
/// the shim's framing comes from it, and the server accepts a verb only if it
/// appears here. Adding a verb is adding a row (and, when it does real work, a
/// branch in RemoteControl::execute).
///
/// Header-only and Qt-Core-only, like the rest of the shared helpers: the
/// shim links no server code and must not have to.

/// The control protocol's own version, published in the discovery file and
/// checked by the shim before it dials.
///
/// Bump it when the vocabulary or the framing changes incompatibly: a pist and
/// a pist-mcp from different releases then refuse the pair immediately, naming
/// both versions, instead of negotiating a connection and failing verb by verb
/// once an agent tries something the other half does not have (finding MIN-52).
constexpr int kControlProtocolVersion = 1;

/// One remote-control verb.
struct Verb
{
    /// The word on the wire — the first token of a command line.
    const char *name;
    /// The `help` line for it: its arguments and what it does. The column
    /// alignment is part of the text the server sends.
    const char *usage;
    /// True when the reply is framed as a block — an explicit ok/error status
    /// line, the body, then a line holding only '.'. A single-line verb's reply
    /// is one line whose text carries its own outcome.
    bool block;
};

/// Every verb the server accepts, in the order `help` lists them.
inline constexpr Verb kVerbs[] = {
    {"open", "open <path>      open a source file", false},
    {"read", "read             the current document as JSON {path, text}", true},
    {"build", "build            assemble, answering when the build finishes", false},
    {"run", "run              build and start the emulator, answering when running", false},
    {"stop", "stop             stop the emulator session", false},
    {"cmd", "cmd <command>    run a debugger command, replying with its output", true},
    {"step", "step             step one instruction", false},
    {"stepover", "stepover         step over a subroutine", false},
    {"continue", "continue         resume execution", false},
    {"breakpoint", "breakpoint <n|label>   toggle a breakpoint at a source line (code\n"
                   "                 lines only) or at a symbol's definition", false},
    {"symbols", "symbols [filter] the build's symbols as a JSON array", true},
    {"readmem", "readmem <a> <n>  read <n> bytes at <a> as JSON rows (when stopped)", true},
    {"disasm", "disasm [a]       disassembly as JSON rows (when stopped)", true},
    {"setreg", "setreg <n> <v>   write register <n> to <v> (when stopped)", false},
    {"setmem", "setmem <a> <v>   write memory byte at <a> to <v> (when stopped)", false},
    {"watchpoint", "watchpoint <a>   break when the value at address <a> changes "
                   "(optional .b/.w/.l)", false},
    {"screenshot", "screenshot <f>   save the window to <f> (default /tmp/pist-screenshot.png)",
     false},
    {"console", "console          the build & debug console text", true},
    {"state", "state            registers and PC (text)", true},
    {"statejson", "statejson        registers and PC as a JSON object", true},
    {"problems", "problems         the Problems pane as a JSON array", true},
    {"tabs", "tabs             the open documents as a JSON array", true},
    {"save", "save             save the current document", false},
    {"profile", "profile <v>      start|stop collection, results as a JSON array", false},
    {"watch", "watch            receive events as the session changes state", false},
    {"unwatch", "unwatch          stop receiving events (connection stays open)", false},
    {"help", "help             this list", true},
    {"quit", "quit             close the IDE", false},
};

/// The table's row for `name`, or nullptr when the vocabulary has no such verb.
inline const Verb *findVerb(const QString &name)
{
    for (const Verb &verb : kVerbs) {
        if (name == QLatin1String(verb.name))
            return &verb;
    }
    return nullptr;
}

/// Whether a whole command line gets a block reply.
///
/// `profile` is the one verb whose framing depends on its sub-verb: its results
/// are a block while start and stop answer on a single line.
inline bool isBlockCommand(const QString &command)
{
    const QString verb = command.section(QLatin1Char(' '), 0, 0);
    if (verb == QLatin1String("profile"))
        return command.section(QLatin1Char(' '), 1, 1) == QLatin1String("results");
    const Verb *found = findVerb(verb);
    return found && found->block;
}

} // namespace control
} // namespace pist
