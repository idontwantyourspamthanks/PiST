// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "hooks/HookApi.h"

#include <QString>

namespace pist::hooks {

/// How one script run ended. `error`, when set, is a sentence for the console;
/// `file` and `line` locate it in the script, and `line` is -1 when the message
/// has no line of its own (a file that could not be read, a timeout).
struct StopHookResult
{
    bool ran = false;
    bool ok = true;
    bool timedOut = false;
    QString file;
    int line = -1;
    QString error;
};

/// The scripting engine behind the stop hook: evaluate one Lua file with `api`
/// bound as its globals, under a wall-clock budget.
///
/// One interpeter per call, closed with it, so a script cannot leak state — or a
/// runaway loop — into the next stop.
///
/// **Lua 5.4, pinned by checksum and linked statically**, chosen over Qt's
/// QJSEngine on measurement (docs/stop-hooks-plan.md §5 D4): the emulator-script
/// audience already writes Lua, `readmem` can return a byte-exact string where
/// JavaScript's UTF-16 strings cannot carry a byte above 0x7F, `continue()` is
/// not a reserved word, and the shipped cost is ~345 KiB rather than ~8.8 MB.
///
/// The budget is enforced from Lua's own count hook — the only code that runs
/// while a script is running — against a deadline, so a `while true do end` is
/// stopped rather than hanging the IDE.
class StopHookEngine
{
public:
    /// A hook is a look-and-poke, not a program.
    static constexpr int kDefaultBudgetMs = 500;

    static StopHookResult run(const QString &scriptPath, HookApi &api,
                              int budgetMs = kDefaultBudgetMs);
};

} // namespace pist::hooks
