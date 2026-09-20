// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/InstrRef.h"

#include <QHash>
#include <QLatin1String>

namespace pist {

namespace {

void add(QList<InstructionInfo> &table, const char *mnemonic, const char *summary, const char *flags)
{
    table.append(InstructionInfo{QString::fromLatin1(mnemonic),
                                 QString::fromLatin1(summary),
                                 QString::fromLatin1(flags)});
}

/// The reference, grouped the way a manual groups it: movement, arithmetic,
/// logic, then program control. Building it once at first use keeps the list
/// out of the static-init order, which would matter for the QString members.
QList<InstructionInfo> buildTable()
{
    QList<InstructionInfo> t;
    t.reserve(160);

    // Phrases repeated across whole families, so a family reads consistently.
    const char *const setAll = "X N Z V C set";
    const char *const setNzOnly = "X unaffected; N/Z set; V/C cleared";
    const char *const cmpFlags = "N Z V C set; X unaffected";
    const char *const noFlags = "none affected";

    // --- Data movement -------------------------------------------------------
    add(t, "MOVE", "Move: copy a source operand to a destination. The same mnemonic reads and writes SR, CCR and USP.", "X unaffected; N/Z set, V/C cleared");
    add(t, "MOVEA", "Move address: copy a source to an address register. The word form sign-extends.", noFlags);
    add(t, "MOVEQ", "Move quick: load a sign-extended 8-bit immediate into a data register.", setNzOnly);
    add(t, "MOVEM", "Move multiple: load or store a list of registers as consecutive memory words or longs.", noFlags);
    add(t, "MOVEP", "Move peripheral: transfer between a data register and every other byte of memory, for 8-bit peripherals.", noFlags);
    add(t, "LEA", "Load effective address: compute an addressing mode's address without reading memory, into an address register.", noFlags);
    add(t, "PEA", "Push effective address: compute an addressing mode's address and push it on the stack.", noFlags);
    add(t, "LINK", "Link: push the frame pointer, set it to the stack pointer, then reserve the frame's bytes.", noFlags);
    add(t, "UNLK", "Unlink: restore the stack pointer from the frame pointer and pop the saved frame pointer.", noFlags);
    add(t, "EXG", "Exchange: swap the complete contents of two registers.", noFlags);

    // --- Integer arithmetic --------------------------------------------------
    add(t, "ADD", "Add: add a source operand to a destination.", setAll);
    add(t, "ADDA", "Add address: add a source operand to an address register; the word form sign-extends.", noFlags);
    add(t, "ADDI", "Add immediate: add an immediate value to a destination.", setAll);
    add(t, "ADDQ", "Add quick: add a 1-8 immediate to the destination.", setAll);
    add(t, "ADDX", "Add extended: add two operands with the X flag carried in, for multi-precision arithmetic.", setAll);
    add(t, "SUB", "Subtract: subtract a source operand from a destination.", setAll);
    add(t, "SUBA", "Subtract address: subtract a source operand from an address register; the word form sign-extends.", noFlags);
    add(t, "SUBI", "Subtract immediate: subtract an immediate value from a destination.", setAll);
    add(t, "SUBQ", "Subtract quick: subtract a 1-8 immediate from the destination.", setAll);
    add(t, "SUBX", "Subtract extended: subtract with the X flag borrowed in, for multi-precision arithmetic.", setAll);
    add(t, "CMP", "Compare: subtract the source from the destination and set the flags only.", cmpFlags);
    add(t, "CMPA", "Compare address: compare a source with an address register and set the flags only.", cmpFlags);
    add(t, "CMPI", "Compare immediate: compare an immediate with the destination and set the flags only.", cmpFlags);
    add(t, "CMPM", "Compare memory: compare two post-increment memory operands and set the flags only.", cmpFlags);
    add(t, "MULS", "Multiply signed: multiply two 16-bit values into a 32-bit signed product.", setNzOnly);
    add(t, "MULU", "Multiply unsigned: multiply two 16-bit values into a 32-bit unsigned product.", setNzOnly);
    add(t, "DIVS", "Divide signed: divide a 32-bit value by a 16-bit divisor, leaving quotient in the low word and remainder in the high word.", "X unaffected; N/Z set from the quotient, V on overflow or divide by zero, C cleared");
    add(t, "DIVU", "Divide unsigned: divide a 32-bit value by a 16-bit divisor, leaving quotient in the low word and remainder in the high word.", "X unaffected; N/Z set from the quotient, V on overflow or divide by zero, C cleared");
    add(t, "NEG", "Negate: subtract the destination from zero (two's complement).", setAll);
    add(t, "NEGX", "Negate extended: subtract the destination from zero with the X flag borrowed in.", setAll);
    add(t, "CLR", "Clear: set a destination to zero without reading its old value.", "X unaffected; N cleared, Z set, V/C cleared");
    add(t, "TST", "Test: compare a destination against zero and set the flags only.", setNzOnly);
    add(t, "EXT", "Extend: sign-extend a byte to a word; later processors extend a word to a long.", setNzOnly);
    add(t, "ABCD", "Add decimal with extend: add two packed BCD bytes with the X flag carried in.", "X and C set on decimal carry; Z cleared unless the result is zero");
    add(t, "SBCD", "Subtract decimal with extend: subtract two packed BCD bytes with the X flag borrowed in.", "X and C set on decimal borrow; Z cleared unless the result is zero");
    add(t, "NBCD", "Negate decimal: subtract a packed BCD byte from zero with the X flag borrowed in, giving its ten's complement.", "X and C set on decimal borrow; Z cleared unless the result is zero");

    // --- Logic and shifts ----------------------------------------------------
    add(t, "AND", "And: bitwise AND a source operand into a destination.", setNzOnly);
    add(t, "ANDI", "And immediate: bitwise AND an immediate value into a destination.", setNzOnly);
    add(t, "OR", "Or: bitwise OR a source operand into a destination.", setNzOnly);
    add(t, "ORI", "Or immediate: bitwise OR an immediate value into a destination.", setNzOnly);
    add(t, "EOR", "Exclusive or: bitwise XOR a source operand into a destination.", setNzOnly);
    add(t, "EORI", "Exclusive or immediate: bitwise XOR an immediate value into a destination.", setNzOnly);
    add(t, "NOT", "Not: invert every bit of a destination.", setNzOnly);
    add(t, "LSL", "Logical shift left: shift zeroes into the low end; the register form takes a count, the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result");
    add(t, "LSR", "Logical shift right: shift zeroes into the high end; the register form takes a count, the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result");
    add(t, "ASL", "Arithmetic shift left: shift zeroes into the low end, so the sign can be lost; the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result, V set on a sign change");
    add(t, "ASR", "Arithmetic shift right: replicate the sign bit down, so a negative value stays negative; the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result");
    add(t, "ROL", "Rotate left: bits leaving the top re-enter at the bottom; the memory form rotates by one.", "C takes the last bit rotated out; N/Z set from the result, X unaffected");
    add(t, "ROR", "Rotate right: bits leaving the bottom re-enter at the top; the memory form rotates by one.", "C takes the last bit rotated out; N/Z set from the result, X unaffected");
    add(t, "ROXL", "Rotate left through extend: rotate bits through the X flag as a ninth bit; the memory form rotates by one.", "X and C take the last bit rotated out; N/Z set from the result");
    add(t, "ROXR", "Rotate right through extend: rotate bits through the X flag as a ninth bit; the memory form rotates by one.", "X and C take the last bit rotated out; N/Z set from the result");
    add(t, "BTST", "Test bit: test one bit of the destination, which may be memory or a register.", "Z set from the tested bit; other flags unaffected");
    add(t, "BSET", "Set bit: test one bit of the destination, then set it.", "Z set from the bit's old value; other flags unaffected");
    add(t, "BCLR", "Clear bit: test one bit of the destination, then clear it.", "Z set from the bit's old value; other flags unaffected");
    add(t, "BCHG", "Change bit: test one bit of the destination, then invert it.", "Z set from the bit's old value; other flags unaffected");
    add(t, "SWAP", "Swap: exchange the high and low words of a data register.", setNzOnly);

    // --- Program control -----------------------------------------------------
    add(t, "BRA", "Branch always: take a PC-relative branch.", noFlags);
    add(t, "BSR", "Branch to subroutine: push the return address, then take a PC-relative branch.", noFlags);
    add(t, "BCC", "Branch on carry clear: branch when C = 0 (that is, higher or same).", noFlags);
    add(t, "BHS", "Branch on higher or same: another name for BCC, branching when C = 0.", noFlags);
    add(t, "BCS", "Branch on carry set: branch when C = 1 (that is, lower).", noFlags);
    add(t, "BLO", "Branch on lower: another name for BCS, branching when C = 1.", noFlags);
    add(t, "BEQ", "Branch on equal: branch when Z = 1.", noFlags);
    add(t, "BNE", "Branch on not equal: branch when Z = 0.", noFlags);
    add(t, "BGE", "Branch on greater or equal: branch when N = V.", noFlags);
    add(t, "BLT", "Branch on less than: branch when N is not V.", noFlags);
    add(t, "BGT", "Branch on greater than: branch when Z = 0 and N = V.", noFlags);
    add(t, "BLE", "Branch on less or equal: branch when Z = 1 or N is not V.", noFlags);
    add(t, "BHI", "Branch on higher: branch when C = 0 and Z = 0.", noFlags);
    add(t, "BLS", "Branch on lower or same: branch when C = 1 or Z = 1.", noFlags);
    add(t, "BMI", "Branch on minus: branch when N = 1.", noFlags);
    add(t, "BPL", "Branch on plus: branch when N = 0.", noFlags);
    add(t, "BVC", "Branch on overflow clear: branch when V = 0.", noFlags);
    add(t, "BVS", "Branch on overflow set: branch when V = 1.", noFlags);
    add(t, "JMP", "Jump: continue at an address computed from a control addressing mode, without saving a return address.", noFlags);
    add(t, "JSR", "Jump to subroutine: push the return address, then continue at the computed address.", noFlags);
    add(t, "RTS", "Return from subroutine: pop the return address into the PC.", noFlags);
    add(t, "RTR", "Return and restore: pop the CCR, then pop the return address into the PC.", "X N Z V C restored from the stacked CCR");
    add(t, "RTE", "Return from exception: pop the SR and the PC, restoring the interrupted supervisor state.", "X N Z V C and the S bit restored from the stacked SR");

    // The DBcc family loops until its condition holds. Note that the branch it
    // takes is on the *negated* condition, which is why DBEQ leaves the loop
    // when Z is set: it branches while the values differ.
    add(t, "DBRA", "Decrement and branch: the plain counted loop, decrementing the low word of Dn and branching back until it reaches -1.", noFlags);
    add(t, "DBF", "Decrement and branch false: another name for DBRA, looping until Dn reaches -1.", noFlags);
    add(t, "DBCC", "Loop until carry clear (C = 0): decrement the low word of Dn and branch back each pass, leaving when C = 0 or Dn reaches -1.", noFlags);
    add(t, "DBHS", "Loop until higher or same (C = 0): another name for DBCC.", noFlags);
    add(t, "DBCS", "Loop until carry set (C = 1): decrement the low word of Dn and branch back each pass, leaving when C = 1 or Dn reaches -1.", noFlags);
    add(t, "DBLO", "Loop until lower (C = 1): another name for DBCS.", noFlags);
    add(t, "DBEQ", "Loop until equal (Z = 1): decrement the low word of Dn and branch back each pass, leaving when Z = 1 or Dn reaches -1.", noFlags);
    add(t, "DBNE", "Loop until not equal (Z = 0): decrement the low word of Dn and branch back each pass, leaving when Z = 0 or Dn reaches -1.", noFlags);
    add(t, "DBGE", "Loop until greater or equal (N = V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBLT", "Loop until less than (N is not V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBGT", "Loop until greater than (Z = 0 and N = V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBLE", "Loop until less or equal (Z = 1 or N is not V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBHI", "Loop until higher (C = 0 and Z = 0): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBLS", "Loop until lower or same (C = 1 or Z = 1): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBMI", "Loop until minus (N = 1): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBPL", "Loop until plus (N = 0): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBVC", "Loop until overflow clear (V = 0): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);
    add(t, "DBVS", "Loop until overflow set (V = 1): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags);

    add(t, "ST", "Set true: store $FF, the unconditional boolean true; the flags are not affected.", noFlags);
    add(t, "SF", "Set false: store $00, the unconditional boolean false; the flags are not affected.", noFlags);
    add(t, "SCC", "Set on carry clear: store $FF when C = 0, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SHS", "Set on higher or same: another name for SCC, storing $FF when C = 0.", noFlags);
    add(t, "SCS", "Set on carry set: store $FF when C = 1, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SLO", "Set on lower: another name for SCS, storing $FF when C = 1.", noFlags);
    add(t, "SEQ", "Set on equal: store $FF when Z = 1, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SNE", "Set on not equal: store $FF when Z = 0, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SGE", "Set on greater or equal: store $FF when N = V, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SLT", "Set on less than: store $FF when N is not V, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SGT", "Set on greater than: store $FF when Z = 0 and N = V, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SLE", "Set on less or equal: store $FF when Z = 1 or N is not V, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SHI", "Set on higher: store $FF when C = 0 and Z = 0, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SLS", "Set on lower or same: store $FF when C = 1 or Z = 1, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SMI", "Set on minus: store $FF when N = 1, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SPL", "Set on plus: store $FF when N = 0, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SVC", "Set on overflow clear: store $FF when V = 0, $00 otherwise; the flags are not affected.", noFlags);
    add(t, "SVS", "Set on overflow set: store $FF when V = 1, $00 otherwise; the flags are not affected.", noFlags);

    add(t, "TRAP", "Trap: enter supervisor mode through vector 32 + n, which is how an ST program calls GEMDOS and BIOS.", noFlags);
    add(t, "TRAPV", "Trap on overflow: enter supervisor mode through vector 7 when V is set, so an overflow raises an exception.", noFlags);
    add(t, "CHK", "Check register against bounds: trap through the CHK vector when the register is negative or above the bound.", "N set when the register is negative; Z, V and C are undefined");
    add(t, "ILLEGAL", "Illegal: force the illegal-instruction exception through vector 4.", noFlags);
    add(t, "NOP", "No operation: do nothing.", noFlags);
    add(t, "RESET", "Reset: assert the external reset line, resetting the peripherals attached to it.", noFlags);
    add(t, "STOP", "Stop: load the SR from the immediate operand, then halt until a reset, a trace or an interrupt.", "X N Z V C and the S bit loaded from the immediate");
    add(t, "TAS", "Test and set: test a byte and set its high bit as one indivisible operation, for a lock.", setNzOnly);

    return t;
}

/// The lookup key: lower case, with any size suffix the assembler syntax
/// allows on a mnemonic removed.
QString normalize(const QString &word)
{
    QString w = word.trimmed().toLower();
    for (QLatin1String suffix : {QLatin1String(".b"), QLatin1String(".w"), QLatin1String(".l")}) {
        if (w.endsWith(suffix)) {
            w.chop(suffix.size());
            break;
        }
    }
    return w;
}

} // namespace

const QList<InstructionInfo> &instructionTable()
{
    static const QList<InstructionInfo> table = buildTable();
    return table;
}

const InstructionInfo *instructionRef(const QString &word)
{
    // The index is built from the table, so a mnemonic added above is
    // immediately reachable and can never drift from the listing.
    static const QHash<QString, const InstructionInfo *> index = [] {
        QHash<QString, const InstructionInfo *> map;
        const QList<InstructionInfo> &table = instructionTable();
        for (const InstructionInfo &info : table)
            map.insert(info.mnemonic.toLower(), &info);
        return map;
    }();

    const QString key = normalize(word);
    if (key.isEmpty())
        return nullptr;
    return index.value(key, nullptr);
}

} // namespace pist
