// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/Vocabulary.h"

#include "editor/InstrRef.h"

namespace pist {
namespace {

/// Mnemonics vasm's mot module accepts that the 68000 reference does not list:
/// the 68020+ system instructions and the FPU. The reference is a 68000
/// reference on purpose (`InstrRef.h` says why), so the union with it lives in
/// this file rather than in a second hand-written copy of the 68000 set — that
/// copy had already drifted by nineteen entries, and the `blo.s` in demo/hello.s
/// went uncoloured.
const char *const kExtraMnemonics[] = {
    // 68020+.
    "bkpt", "rtd", "callm", "rtm", "extb",
    // FPU.
    "fmove", "fadd", "fsub", "fmul", "fdiv", "fsqrt", "fabs", "fneg", "fint",
    "fintrz", "fcmp", "ftst", "fsin", "fcos", "ftan", "fatan", "fgetexp",
    "fgetman", "fscale", "fmod", "frem", "fsgldiv", "fsglmul", "fmovecr",
    "fsave", "frestore", "faddd", "fsubd", "fmuld", "fdivd",
    nullptr
};

} // namespace

const QStringList &mnemonicWords()
{
    static const QStringList words = [] {
        QStringList list;
        const QList<InstructionInfo> &table = instructionTable();
        list.reserve(table.size() + 40); // the extras below
        for (const InstructionInfo &info : table)
            list.append(info.mnemonic.toLower());
        for (int i = 0; kExtraMnemonics[i]; ++i)
            list.append(QString::fromLatin1(kExtraMnemonics[i]));
        return list;
    }();
    return words;
}

const QStringList &directiveWords()
{
    static const QStringList words = {
        // The data family, spelled out: the same words the highlighter's pattern
        // used to write as `dc\.[bwls]`, so a directive is one word here.
        QStringLiteral("dc.b"),    QStringLiteral("dc.w"),  QStringLiteral("dc.l"),
        QStringLiteral("dc.s"),    QStringLiteral("ds.b"),  QStringLiteral("ds.w"),
        QStringLiteral("ds.l"),    QStringLiteral("ds.s"),  QStringLiteral("dcb.b"),
        QStringLiteral("dcb.w"),   QStringLiteral("dcb.l"), QStringLiteral("dcb.s"),
        // Definitions, alignment and sections.
        QStringLiteral("equ"),     QStringLiteral("set"),   QStringLiteral("even"),
        QStringLiteral("odd"),     QStringLiteral("cnop"),  QStringLiteral("align"),
        QStringLiteral("section"), QStringLiteral("text"),  QStringLiteral("data"),
        QStringLiteral("bss"),     QStringLiteral("end"),   QStringLiteral("rsreset"),
        QStringLiteral("rs"),      QStringLiteral("org"),   QStringLiteral("rorg"),
        // Including.
        QStringLiteral("include"), QStringLiteral("incbin"), QStringLiteral("incdir"),
        // Macros and their options.
        QStringLiteral("macro"),   QStringLiteral("endm"),  QStringLiteral("repeat"),
        QStringLiteral("endr"),    QStringLiteral("opt"),
        // Conditional assembly.
        QStringLiteral("if"),      QStringLiteral("else"),  QStringLiteral("endif"),
        QStringLiteral("ifd"),     QStringLiteral("ifnd"),
        // Symbol visibility.
        QStringLiteral("global"),  QStringLiteral("xdef"),  QStringLiteral("xref"),
        QStringLiteral("import"),  QStringLiteral("export"), QStringLiteral("far"),
        QStringLiteral("near"),    QStringLiteral("clrfo"), QStringLiteral("clrso"),
    };
    return words;
}

} // namespace pist
