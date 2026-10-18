-- hello.hooks.lua — the stop-hook script demo/hello.pistproject points at.
--
-- A stop hook runs at every debugger stop with the machine held still, and is
-- where the IDE's "scripted stop hooks" feature lives (new in 0.10.0). Switch it
-- on in Run ▸ Stop hooks…; this project's .pistproject already names this file
-- and sets hooks.enabled. Watch the console dock for the "[hook] " lines, and
-- Run ▸ Stop hooks… for how the last run went.
--
-- What this script does, in order:
--   1. logs the stop's reason and the breakpoint it stopped at,
--   2. counts the stop in the *guest's* own memory (`hook_calls` in hello.s),
--   3. reads the watched word `counter` by name,
--   4. on the second stop, pokes its low byte, so the watchpoint on `counter.w`
--      fires on a change no instruction made,
--   5. asks to continue once the program's counter has finished climbing.
--
-- The API is exactly these functions, and nothing else is reachable (no io, no
-- os, no require):
--
--   readmem(addr, len)     -> a byte-exact string of len bytes (1..4096)
--   readreg(name)          -> an integer; writereg(name, value) sets d0-d7,
--                             a0-a7, pc or sr
--   sym(name)              -> the address the label has *this run*
--   writemem(addr, value)  -> one byte
--   log(msg)               -> one "[hook] " line in the console (print does it too)
--   continue() / resume()  -> asks to resume; the IDE issues it after the script
--                             returns, and only if the project allows continuing
--
-- Two values describe the stop the script is running at:
--
--   reason  "breakpoint", "step" or "unknown" — a pause, an exception and a
--           watchpoint all report "unknown", because the emulator reports a
--           prompt, not a cause
--   at      the breakpoint that fired, "file:line"; nil when there is none
--
-- Three rules worth knowing:
--
--   * Every stop builds a fresh interpreter, so a Lua global does *not* survive
--     from one stop to the next. Anything that has to count across stops lives
--     in the guest's own memory, which is why the stop count below is the word
--     `hook_calls` that hello.s declares rather than a Lua variable.
--   * Addresses move: GEMDOS relocates the program on every run, so a hard-coded
--     address is wrong next run. sym() resolves through this run's bases.
--   * An error is reported as "[hook] <file>:<line>: <message>" and leaves the
--     machine stopped; a runaway script is cut off after 500 ms.
--
-- Nothing here needs hooks.autoContinue — see the last block, which states the
-- condition it continues on. With autoContinue off (this project's setting) the
-- IDE logs the request and stays stopped, so the hooks give you a step-by-step
-- run without ever resuming on their own.

-- The 68k is big-endian, and readmem() returns memory order, so the first byte
-- of a word is its high half.
local function word_at(addr)
    local hi, lo = string.byte(readmem(addr, 2), 1, 2)
    return hi * 256 + lo
end

-- 1. Say what this stop is. `at` is nil unless a breakpoint fired, so this is a
--    test of presence rather than a comparison against "".
if at then
    log(string.format("stop: reason=%s at=%s pc=$%06x", reason, at, readreg("pc")))
else
    log(string.format("stop: reason=%s (no breakpoint) pc=$%06x", reason, readreg("pc")))
end

-- 2. Count the stop in the guest's memory. `hook_calls` starts at 0 in hello.s,
--    so the value read here is how many stops have already run: the second stop
--    of the session reads 1.
local calls = string.byte(readmem(sym("hook_calls"), 1))
writemem(sym("hook_calls"), calls + 1)

-- 3. The watched word, read once and by name: `counter` is the word hello.s
--    counts its rounds into, and sym() resolves it through this run's bases, so
--    it is right wherever GEMDOS put the program this time.
local counter_addr = sym("counter")
local counter = word_at(counter_addr)

-- 4. The second stop is the one worth doing something at: the program has run
--    its first round, so `counter` is no longer 0. Poke its low byte — one byte
--    is all writemem() writes, so the high half is left as it was — and the
--    watchpoint on counter.w fires on a change no instruction in the program
--    made. The program's next round stores over it again; a script pokes state,
--    it does not own it.
if calls == 1 then
    log(string.format("second stop: counter is %d at $%06x; poking its low byte", counter,
                      counter_addr))
    writemem(counter_addr, 0x7F)
end

-- 5. Continue only once the program has counted its last round: `counter` has
--    reached `rounds` (3 in hello.s). The value tested is the one read at the
--    top of *this* stop, before the poke above, so the script's own write cannot
--    be mistaken for the program finishing. Every earlier stop is left for
--    whoever is reading the screen, the entry stop included. hooks.autoContinue
--    does not have to be on — with it off the IDE logs this request and stays
--    stopped anyway, so the condition above is stated rather than relied on.
if counter >= 3 then
    log("counter has reached rounds; asking to continue")
    continue()
end
