// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/InstrRef.h"

#include <QHash>
#include <QLatin1String>

namespace pist {

namespace {

void add(QList<InstructionInfo> &table, const char *mnemonic, const char *summary,
         const char *flags, const char *example, const char *typicalUse)
{
    table.append(InstructionInfo{QString::fromLatin1(mnemonic),
                                 QString::fromLatin1(summary),
                                 QString::fromLatin1(flags),
                                 QString::fromLatin1(example),
                                 QString::fromLatin1(typicalUse)});
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
    add(t, "MOVE", "Move: copy a source operand to a destination. The same mnemonic reads and writes SR, CCR and USP.", "X unaffected; N/Z set, V/C cleared", "\tmove.w\td0,(a0)", "Copying a value between registers, or between a register and memory.");
    add(t, "MOVEA", "Move address: copy a source to an address register. The word form sign-extends.", noFlags, "\tmovea.l\ta0,a1", "Moving a pointer into an address register without touching the flags.");
    add(t, "MOVEQ", "Move quick: load a sign-extended 8-bit immediate into a data register.", setNzOnly, "\tmoveq\t#0,d0", "Loading a small constant, such as zero or a loop counter.");
    add(t, "MOVEM", "Move multiple: load or store a list of registers as consecutive memory words or longs.", noFlags, "\tmovem.l\td0-d1,-(sp)", "Saving or restoring a set of registers around a subroutine.");
    add(t, "MOVEP", "Move peripheral: transfer between a data register and every other byte of memory, for 8-bit peripherals.", noFlags, "\tmovep.w\t0(a0),d0", "Reading a byte-wide peripheral whose registers sit every other byte.");
    add(t, "LEA", "Load effective address: compute an addressing mode's address without reading memory, into an address register.", noFlags, "\tlea\tbuffer,a0", "Taking the address of an object into an address register.");
    add(t, "PEA", "Push effective address: compute an addressing mode's address and push it on the stack.", noFlags, "\tpea\tmsg", "Passing the address of a string or structure as a call argument.");
    add(t, "LINK", "Link: push the frame pointer, set it to the stack pointer, then reserve the frame's bytes.", noFlags, "\tlink\ta6,#-4", "Opening a stack frame that keeps locals and saved registers reachable.");
    add(t, "UNLK", "Unlink: restore the stack pointer from the frame pointer and pop the saved frame pointer.", noFlags, "\tunlk\ta6", "Closing the frame that link opened, just before returning.");
    add(t, "EXG", "Exchange: swap the complete contents of two registers.", noFlags, "\texg\td0,d1", "Swapping the contents of two registers without a temporary.");

    // --- Integer arithmetic --------------------------------------------------
    add(t, "ADD", "Add: add a source operand to a destination.", setAll, "\tadd.w\td0,d1", "Adding one value to another, in a register or in memory.");
    add(t, "ADDA", "Add address: add a source operand to an address register; the word form sign-extends.", noFlags, "\tadda.l\td0,a0", "Adding a displacement or index to a pointer.");
    add(t, "ADDI", "Add immediate: add an immediate value to a destination.", setAll, "\taddi.w\t#1,d0", "Adding a constant to a destination.");
    add(t, "ADDQ", "Add quick: add a 1-8 immediate to the destination.", setAll, "\taddq.l\t#1,d0", "Stepping a counter or pointer by a small constant.");
    add(t, "ADDX", "Add extended: add two operands with the X flag carried in, for multi-precision arithmetic.", setAll, "\taddx.l\td0,d1", "Adding the high halves of a multi-precision sum, carrying X in.");
    add(t, "SUB", "Subtract: subtract a source operand from a destination.", setAll, "\tsub.w\td1,d0", "Subtracting one value from another.");
    add(t, "SUBA", "Subtract address: subtract a source operand from an address register; the word form sign-extends.", noFlags, "\tsuba.l\td0,a0", "Subtracting a value from a pointer.");
    add(t, "SUBI", "Subtract immediate: subtract an immediate value from a destination.", setAll, "\tsubi.w\t#1,d0", "Subtracting a constant from a destination.");
    add(t, "SUBQ", "Subtract quick: subtract a 1-8 immediate from the destination.", setAll, "\tsubq.l\t#1,d0", "Decrementing a counter or pointer by a small constant.");
    add(t, "SUBX", "Subtract extended: subtract with the X flag borrowed in, for multi-precision arithmetic.", setAll, "\tsubx.l\td0,d1", "Subtracting the high halves of a multi-precision difference.");
    add(t, "CMP", "Compare: subtract the source from the destination and set the flags only.", cmpFlags, "\tcmp.w\td0,d1", "Testing two values for order or equality without changing either.");
    add(t, "CMPA", "Compare address: compare a source with an address register and set the flags only.", cmpFlags, "\tcmpa.l\ta0,a1", "Comparing a pointer with another value, for a bounds or equality test.");
    add(t, "CMPI", "Compare immediate: compare an immediate with the destination and set the flags only.", cmpFlags, "\tcmpi.w\t#10,d0", "Comparing a destination against a constant.");
    add(t, "CMPM", "Compare memory: compare two post-increment memory operands and set the flags only.", cmpFlags, "\tcmpm.b\t(a0)+,(a1)+", "Comparing two memory strings as they are scanned in step.");
    add(t, "MULS", "Multiply signed: multiply two 16-bit values into a 32-bit signed product.", setNzOnly, "\tmuls.w\td0,d1", "Multiplying two signed 16-bit values, such as fixed-point coordinates.");
    add(t, "MULU", "Multiply unsigned: multiply two 16-bit values into a 32-bit unsigned product.", setNzOnly, "\tmulu.w\td0,d1", "Multiplying two unsigned 16-bit values, such as byte counts.");
    add(t, "DIVS", "Divide signed: divide a 32-bit value by a 16-bit divisor, leaving quotient in the low word and remainder in the high word.", "X unaffected; N/Z set from the quotient, V on overflow or divide by zero, C cleared", "\tdivs.w\td0,d1", "Dividing a signed value to get both a quotient and a remainder.");
    add(t, "DIVU", "Divide unsigned: divide a 32-bit value by a 16-bit divisor, leaving quotient in the low word and remainder in the high word.", "X unaffected; N/Z set from the quotient, V on overflow or divide by zero, C cleared", "\tdivu.w\td0,d1", "Dividing an unsigned value to get both a quotient and a remainder.");
    add(t, "NEG", "Negate: subtract the destination from zero (two's complement).", setAll, "\tneg.w\td0", "Reversing the sign of a value.");
    add(t, "NEGX", "Negate extended: subtract the destination from zero with the X flag borrowed in.", setAll, "\tnegx.l\td0", "Negating the high part of a multi-precision value.");
    add(t, "CLR", "Clear: set a destination to zero without reading its old value.", "X unaffected; N cleared, Z set, V/C cleared", "\tclr.l\td0", "Setting a destination to zero without reading it first.");
    add(t, "TST", "Test: compare a destination against zero and set the flags only.", setNzOnly, "\ttst.w\td0", "Setting the flags from a value without changing it.");
    add(t, "EXT", "Extend: sign-extend a byte to a word; later processors extend a word to a long.", setNzOnly, "\text.w\td0", "Sign-extending a byte or word so it can be used at a wider size.");
    add(t, "ABCD", "Add decimal with extend: add two packed BCD bytes with the X flag carried in.", "X and C set on decimal carry; Z cleared unless the result is zero", "\tabcd\td0,d1", "Adding packed BCD digits, such as a decimal score or counter.");
    add(t, "SBCD", "Subtract decimal with extend: subtract two packed BCD bytes with the X flag borrowed in.", "X and C set on decimal borrow; Z cleared unless the result is zero", "\tsbcd\td0,d1", "Subtracting packed BCD digits, such as a decimal countdown.");
    add(t, "NBCD", "Negate decimal: subtract a packed BCD byte from zero with the X flag borrowed in, giving its ten's complement.", "X and C set on decimal borrow; Z cleared unless the result is zero", "\tnbcd\td0", "Forming the ten's complement of a packed BCD byte.");

    // --- Logic and shifts ----------------------------------------------------
    add(t, "AND", "And: bitwise AND a source operand into a destination.", setNzOnly, "\tand.w\td0,d1", "Masking off the bits that should be cleared.");
    add(t, "ANDI", "And immediate: bitwise AND an immediate value into a destination.", setNzOnly, "\tandi.w\t#$00ff,d0", "Masking a destination against a constant bit pattern.");
    add(t, "OR", "Or: bitwise OR a source operand into a destination.", setNzOnly, "\tor.w\td0,d1", "Setting bits that are already present in another operand.");
    add(t, "ORI", "Or immediate: bitwise OR an immediate value into a destination.", setNzOnly, "\tori.b\t#$80,d0", "Setting one or more constant bits.");
    add(t, "EOR", "Exclusive or: bitwise XOR a source operand into a destination.", setNzOnly, "\teor.w\td0,d1", "Toggling bits selected by another operand.");
    add(t, "EORI", "Exclusive or immediate: bitwise XOR an immediate value into a destination.", setNzOnly, "\teori.w\t#1,d0", "Toggling one or more constant bits.");
    add(t, "NOT", "Not: invert every bit of a destination.", setNzOnly, "\tnot.w\td0", "Inverting every bit of a value.");
    add(t, "LSL", "Logical shift left: shift zeroes into the low end; the register form takes a count, the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result", "\tlsl.w\t#2,d0", "Multiplying an unsigned value by a power of two.");
    add(t, "LSR", "Logical shift right: shift zeroes into the high end; the register form takes a count, the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result", "\tlsr.w\t#2,d0", "Dividing an unsigned value by a power of two.");
    add(t, "ASL", "Arithmetic shift left: shift zeroes into the low end, so the sign can be lost; the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result, V set on a sign change", "\tasl.w\t#1,d0", "Multiplying a signed value by a power of two.");
    add(t, "ASR", "Arithmetic shift right: replicate the sign bit down, so a negative value stays negative; the memory form shifts by one.", "C takes the last bit shifted out and X follows C; N/Z set from the result", "\tasr.w\t#2,d0", "Dividing a signed value by a power of two, keeping the sign.");
    add(t, "ROL", "Rotate left: bits leaving the top re-enter at the bottom; the memory form rotates by one.", "C takes the last bit rotated out; N/Z set from the result, X unaffected", "\trol.w\t#1,d0", "Rotating bits left within a value, as in a hash or checksum.");
    add(t, "ROR", "Rotate right: bits leaving the bottom re-enter at the top; the memory form rotates by one.", "C takes the last bit rotated out; N/Z set from the result, X unaffected", "\tror.w\t#1,d0", "Rotating bits right within a value, as in a hash or checksum.");
    add(t, "ROXL", "Rotate left through extend: rotate bits through the X flag as a ninth bit; the memory form rotates by one.", "X and C take the last bit rotated out; N/Z set from the result", "\troxl.w\t#1,d0", "Rotating a value left through the carry, for a multi-word shift.");
    add(t, "ROXR", "Rotate right through extend: rotate bits through the X flag as a ninth bit; the memory form rotates by one.", "X and C take the last bit rotated out; N/Z set from the result", "\troxr.w\t#1,d0", "Rotating a value right through the carry, for a multi-word shift.");
    add(t, "BTST", "Test bit: test one bit of the destination, which may be memory or a register.", "Z set from the tested bit; other flags unaffected", "\tbtst\t#0,d0", "Testing whether one bit of a value is set.");
    add(t, "BSET", "Set bit: test one bit of the destination, then set it.", "Z set from the bit's old value; other flags unaffected", "\tbset\t#1,d0", "Setting one bit, such as arming a hardware control bit.");
    add(t, "BCLR", "Clear bit: test one bit of the destination, then clear it.", "Z set from the bit's old value; other flags unaffected", "\tbclr\t#2,d0", "Clearing one bit, such as disarming a hardware control bit.");
    add(t, "BCHG", "Change bit: test one bit of the destination, then invert it.", "Z set from the bit's old value; other flags unaffected", "\tbchg\t#3,d0", "Toggling one bit, such as flipping a state flag.");
    add(t, "SWAP", "Swap: exchange the high and low words of a data register.", setNzOnly, "\tswap\td0", "Swapping the halves of a value, as for byte-order conversion.");

    // --- Program control -----------------------------------------------------
    add(t, "BRA", "Branch always: take a PC-relative branch.", noFlags, "\tbra\tstart", "Jumping to a nearby label unconditionally.");
    add(t, "BSR", "Branch to subroutine: push the return address, then take a PC-relative branch.", noFlags, "\tbsr\tstart", "Calling a nearby subroutine that fits within a branch.");
    add(t, "BCC", "Branch on carry clear: branch when C = 0 (that is, higher or same).", noFlags, "\tbcc\tstart", "Branching after a comparison when the first value is higher or same.");
    add(t, "BHS", "Branch on higher or same: another name for BCC, branching when C = 0.", noFlags, "\tbhs\tstart", "Branching after an unsigned comparison when carry is clear.");
    add(t, "BCS", "Branch on carry set: branch when C = 1 (that is, lower).", noFlags, "\tbcs\tstart", "Branching after an unsigned comparison when the first value is lower.");
    add(t, "BLO", "Branch on lower: another name for BCS, branching when C = 1.", noFlags, "\tblo\tstart", "Branching after an unsigned comparison when carry is set.");
    add(t, "BEQ", "Branch on equal: branch when Z = 1.", noFlags, "\tbeq\tstart", "Branching when the last compared or tested values were equal.");
    add(t, "BNE", "Branch on not equal: branch when Z = 0.", noFlags, "\tbne\tstart", "Branching when the last compared or tested values differed.");
    add(t, "BGE", "Branch on greater or equal: branch when N = V.", noFlags, "\tbge\tstart", "Branching when a signed comparison is greater or equal.");
    add(t, "BLT", "Branch on less than: branch when N is not V.", noFlags, "\tblt\tstart", "Branching when a signed comparison is less than.");
    add(t, "BGT", "Branch on greater than: branch when Z = 0 and N = V.", noFlags, "\tbgt\tstart", "Branching when a signed comparison is strictly greater than.");
    add(t, "BLE", "Branch on less or equal: branch when Z = 1 or N is not V.", noFlags, "\tble\tstart", "Branching when a signed comparison is less or equal.");
    add(t, "BHI", "Branch on higher: branch when C = 0 and Z = 0.", noFlags, "\tbhi\tstart", "Branching when an unsigned comparison is strictly higher.");
    add(t, "BLS", "Branch on lower or same: branch when C = 1 or Z = 1.", noFlags, "\tbls\tstart", "Branching when an unsigned comparison is lower or same.");
    add(t, "BMI", "Branch on minus: branch when N = 1.", noFlags, "\tbmi\tstart", "Branching when the result of the last operation was negative.");
    add(t, "BPL", "Branch on plus: branch when N = 0.", noFlags, "\tbpl\tstart", "Branching when the result of the last operation was positive.");
    add(t, "BVC", "Branch on overflow clear: branch when V = 0.", noFlags, "\tbvc\tstart", "Branching when the last operation did not overflow.");
    add(t, "BVS", "Branch on overflow set: branch when V = 1.", noFlags, "\tbvs\tstart", "Branching when the last operation overflowed.");
    add(t, "JMP", "Jump: continue at an address computed from a control addressing mode, without saving a return address.", noFlags, "\tjmp\t(a0)", "Transferring control through a jump table or a stored pointer.");
    add(t, "JSR", "Jump to subroutine: push the return address, then continue at the computed address.", noFlags, "\tjsr\t(a0)", "Calling a routine through a pointer or jump table entry.");
    add(t, "RTS", "Return from subroutine: pop the return address into the PC.", noFlags, "\trts", "Returning from a subroutine.");
    add(t, "RTR", "Return and restore: pop the CCR, then pop the return address into the PC.", "X N Z V C restored from the stacked CCR", "\trtr", "Returning from a handler that must restore the flags too.");
    add(t, "RTE", "Return from exception: pop the SR and the PC, restoring the interrupted supervisor state.", "X N Z V C and the S bit restored from the stacked SR", "\trte", "Returning from an interrupt or exception handler.");

    // The DBcc family loops until its condition holds. Note that the branch it
    // takes is on the *negated* condition, which is why DBEQ leaves the loop
    // when Z is set: it branches while the values differ.
    add(t, "DBRA", "Decrement and branch: the plain counted loop, decrementing the low word of Dn and branching back until it reaches -1.", noFlags, "\tdbra\td0,start", "Running a loop a fixed number of times.");
    add(t, "DBF", "Decrement and branch false: another name for DBRA, looping until Dn reaches -1.", noFlags, "\tdbf\td0,start", "Running a loop a fixed number of times.");
    add(t, "DBCC", "Loop until carry clear (C = 0): decrement the low word of Dn and branch back each pass, leaving when C = 0 or Dn reaches -1.", noFlags, "\tdbcc\td0,start", "Looping until an unsigned comparison reports carry clear.");
    add(t, "DBHS", "Loop until higher or same (C = 0): another name for DBCC.", noFlags, "\tdbhs\td0,start", "Looping until an unsigned comparison reports carry clear.");
    add(t, "DBCS", "Loop until carry set (C = 1): decrement the low word of Dn and branch back each pass, leaving when C = 1 or Dn reaches -1.", noFlags, "\tdbcs\td0,start", "Looping until an unsigned comparison reports carry set.");
    add(t, "DBLO", "Loop until lower (C = 1): another name for DBCS.", noFlags, "\tdblo\td0,start", "Looping until an unsigned comparison reports carry set.");
    add(t, "DBEQ", "Loop until equal (Z = 1): decrement the low word of Dn and branch back each pass, leaving when Z = 1 or Dn reaches -1.", noFlags, "\tdbeq\td0,start", "Looping until a comparison reports equality.");
    add(t, "DBNE", "Loop until not equal (Z = 0): decrement the low word of Dn and branch back each pass, leaving when Z = 0 or Dn reaches -1.", noFlags, "\tdbne\td0,start", "Looping until a comparison reports inequality.");
    add(t, "DBGE", "Loop until greater or equal (N = V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbge\td0,start", "Looping until a signed comparison is greater or equal.");
    add(t, "DBLT", "Loop until less than (N is not V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdblt\td0,start", "Looping until a signed comparison is less than.");
    add(t, "DBGT", "Loop until greater than (Z = 0 and N = V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbgt\td0,start", "Looping until a signed comparison is strictly greater than.");
    add(t, "DBLE", "Loop until less or equal (Z = 1 or N is not V): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdble\td0,start", "Looping until a signed comparison is less or equal.");
    add(t, "DBHI", "Loop until higher (C = 0 and Z = 0): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbhi\td0,start", "Looping until an unsigned comparison is strictly higher.");
    add(t, "DBLS", "Loop until lower or same (C = 1 or Z = 1): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbls\td0,start", "Looping until an unsigned comparison is lower or same.");
    add(t, "DBMI", "Loop until minus (N = 1): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbmi\td0,start", "Looping until the last result was negative.");
    add(t, "DBPL", "Loop until plus (N = 0): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbpl\td0,start", "Looping until the last result was positive.");
    add(t, "DBVC", "Loop until overflow clear (V = 0): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbvc\td0,start", "Looping until the last operation did not overflow.");
    add(t, "DBVS", "Loop until overflow set (V = 1): decrement the low word of Dn and branch back each pass, leaving then or when Dn reaches -1.", noFlags, "\tdbvs\td0,start", "Looping until the last operation overflowed.");

    add(t, "ST", "Set true: store $FF, the unconditional boolean true; the flags are not affected.", noFlags, "\tst\td0", "Storing a constant true, such as a default flag value.");
    add(t, "SF", "Set false: store $00, the unconditional boolean false; the flags are not affected.", noFlags, "\tsf\td0", "Storing a constant false, such as clearing a flag.");
    add(t, "SCC", "Set on carry clear: store $FF when C = 0, $00 otherwise; the flags are not affected.", noFlags, "\tscc\td0", "Storing $FF or $00 from the state of carry.");
    add(t, "SHS", "Set on higher or same: another name for SCC, storing $FF when C = 0.", noFlags, "\tshs\td0", "Storing $FF when an unsigned comparison is higher or same.");
    add(t, "SCS", "Set on carry set: store $FF when C = 1, $00 otherwise; the flags are not affected.", noFlags, "\tscs\td0", "Storing $FF when an unsigned comparison set carry.");
    add(t, "SLO", "Set on lower: another name for SCS, storing $FF when C = 1.", noFlags, "\tslo\td0", "Storing $FF when an unsigned comparison is lower.");
    add(t, "SEQ", "Set on equal: store $FF when Z = 1, $00 otherwise; the flags are not affected.", noFlags, "\tseq\td0", "Storing $FF when the last comparison found equality.");
    add(t, "SNE", "Set on not equal: store $FF when Z = 0, $00 otherwise; the flags are not affected.", noFlags, "\tsne\td0", "Storing $FF when the last comparison found inequality.");
    add(t, "SGE", "Set on greater or equal: store $FF when N = V, $00 otherwise; the flags are not affected.", noFlags, "\tsge\td0", "Storing $FF when a signed comparison is greater or equal.");
    add(t, "SLT", "Set on less than: store $FF when N is not V, $00 otherwise; the flags are not affected.", noFlags, "\tslt\td0", "Storing $FF when a signed comparison is less than.");
    add(t, "SGT", "Set on greater than: store $FF when Z = 0 and N = V, $00 otherwise; the flags are not affected.", noFlags, "\tsgt\td0", "Storing $FF when a signed comparison is strictly greater than.");
    add(t, "SLE", "Set on less or equal: store $FF when Z = 1 or N is not V, $00 otherwise; the flags are not affected.", noFlags, "\tsle\td0", "Storing $FF when a signed comparison is less or equal.");
    add(t, "SHI", "Set on higher: store $FF when C = 0 and Z = 0, $00 otherwise; the flags are not affected.", noFlags, "\tshi\td0", "Storing $FF when an unsigned comparison is strictly higher.");
    add(t, "SLS", "Set on lower or same: store $FF when C = 1 or Z = 1, $00 otherwise; the flags are not affected.", noFlags, "\tsls\td0", "Storing $FF when an unsigned comparison is lower or same.");
    add(t, "SMI", "Set on minus: store $FF when N = 1, $00 otherwise; the flags are not affected.", noFlags, "\tsmi\td0", "Storing $FF from the sign bit of a value.");
    add(t, "SPL", "Set on plus: store $FF when N = 0, $00 otherwise; the flags are not affected.", noFlags, "\tspl\td0", "Storing $FF when the sign bit is clear.");
    add(t, "SVC", "Set on overflow clear: store $FF when V = 0, $00 otherwise; the flags are not affected.", noFlags, "\tsvc\td0", "Storing $FF when no overflow was detected.");
    add(t, "SVS", "Set on overflow set: store $FF when V = 1, $00 otherwise; the flags are not affected.", noFlags, "\tsvs\td0", "Storing $FF when an overflow was detected.");

    add(t, "TRAP", "Trap: enter supervisor mode through vector 32 + n, which is how an ST program calls GEMDOS and BIOS.", noFlags, "\ttrap\t#1", "Calling the operating system: GEMDOS, BIOS or XBIOS.");
    add(t, "TRAPV", "Trap on overflow: enter supervisor mode through vector 7 when V is set, so an overflow raises an exception.", noFlags, "\ttrapv", "Raising an exception from a routine that detects its own overflow.");
    add(t, "CHK", "Check register against bounds: trap through the CHK vector when the register is negative or above the bound.", "N set when the register is negative; Z, V and C are undefined", "\tchk.w\td0,d1", "Trapping an out-of-range index or count before it is used.");
    add(t, "ILLEGAL", "Illegal: force the illegal-instruction exception through vector 4.", noFlags, "\tillegal", "Raising the illegal-instruction vector deliberately, as an assertion.");
    add(t, "NOP", "No operation: do nothing.", noFlags, "\tnop", "Padding, aligning, or marking a place to patch.");
    add(t, "RESET", "Reset: assert the external reset line, resetting the peripherals attached to it.", noFlags, "\treset", "Resetting the attached peripherals when the machine is reinitialised.");
    add(t, "STOP", "Stop: load the SR from the immediate operand, then halt until a reset, a trace or an interrupt.", "X N Z V C and the S bit loaded from the immediate", "\tstop\t#$2700", "Halting the processor until the next interrupt, in an idle loop.");
    add(t, "TAS", "Test and set: test a byte and set its high bit as one indivisible operation, for a lock.", setNzOnly, "\ttas\t(a0)", "Acquiring a byte lock indivisibly, for a simple semaphore.");

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
