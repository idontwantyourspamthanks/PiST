// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QString>

namespace pist::hooks {

/// What a stop-hook script may do to the stopped machine.
///
/// The runner (`hooks/StopHookRunner`) implements this over `IDebugBackend`'s typed
/// intents; tests implement it as a fake. That split is what keeps both the
/// engine and the runner's policy testable with no emulator and no window
/// (docs/stop-hooks-plan.md §12).
///
/// Every method answers **synchronously**: the debugger is sitting at its `> `
/// prompt while a hook runs, so the round-trip happens inside the call. A
/// failure sets `error` to a sentence naming the call and the reason; the engine
/// raises it as the script's exception, and the runner reports it and leaves the
/// machine stopped.
class HookApi
{
public:
    virtual ~HookApi() = default;

    /// The longest read a script may ask for: a stray number must not ask the
    /// debugger for a gigabyte of dump text.
    static constexpr int kMaxReadBytes = 4096;

    /// Read `length` bytes at `address`, as a byte range (not a value): the
    /// order is the machine's memory order. Refuses an empty or oversized range.
    virtual bool readMemory(quint32 address, int length, QByteArray *bytes, QString *error) = 0;

    /// Write one byte. Refuses when the machine is not stopped — a debugger
    /// command written while running would be deferred by Hatari to the *next*
    /// stop, which is not the stop the script is running in.
    virtual bool writeMemory(quint32 address, quint8 value, QString *error) = 0;

    /// Read a register: `d0`-`d7`, `a0`-`a7`, `pc`, `sr`, `usp`, `isp`
    /// (case-insensitive).
    virtual bool readRegister(const QString &name, quint32 *value, QString *error) = 0;

    /// Write a register. Only the names Hatari accepts are writable — `d0`-`d7`,
    /// `a0`-`a7`, `pc`, `sr`; `usp` and `isp` are readable but not settable by
    /// name (see `ui/RegistersView.cpp` on Hatari's unimplemented SP/SSP names).
    /// Refuses anything else, naming what it got.
    virtual bool writeRegister(const QString &name, quint32 value, QString *error) = 0;

    /// One line on the console, prefixed by the runner.
    virtual void log(const QString &message) = 0;

    /// Why the machine stopped, as far as the IDE can honestly tell:
    /// `"breakpoint"` when the program counter matches an armed breakpoint,
    /// `"step"` after `s`/`n`, and `"unknown"` otherwise. The transport reports a
    /// prompt, not a reason, so a pause, an exception and a watchpoint all land
    /// in `"unknown"` rather than being guessed at — a watchpoint fires on a
    /// *data change* and says nothing about the address it watches.
    virtual QString stopReason() const = 0;

    /// The breakpoint that fired — `"file:line"` — when stopReason() is
    /// `"breakpoint"`; empty otherwise. Two breakpoints resolved to one address
    /// are one stop, and the first is named.
    virtual QString stopAt() const = 0;

    /// Resolve a label (or equate) to the address it has in this run, through the
    /// symbol table and the program map's live bases. Refuses a name that
    /// resolves to nothing, naming it: 0 is a valid address, so there is no "not
    /// found" value to return.
    virtual bool resolveSymbol(const QString &name, quint32 *address, QString *error) = 0;

    /// The script asks to continue. Recorded, never acted on here: the runner
    /// issues the resume after the script has returned — once everything it
    /// queued has completed — and only when the project allows it
    /// (docs/stop-hooks-plan.md D3, R5).
    virtual void requestContinue() = 0;
};

} // namespace pist::hooks
