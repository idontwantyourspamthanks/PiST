// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The reference's authoring rules, as rules rather than as good intentions
// (docs/glm-features.md §2): every entry carries an `Example:` and a
// `Typical use:`, every example assembles under vasm, and an example belongs to
// the entry it is attached to. Prose is deliberately not asserted — whether a
// sentence is *true* is the author's job and a reviewer's, and a test that
// pinned wording would simply be re-pinned by whoever changed the wording.
//
// The two fields are the reason this suite exists: a reference that shows the
// instruction and says when to reach for it is worth reading on its own, and an
// example that stops assembling is exactly the kind of claim that must fail a
// suite rather than ship as prose.

#include "editor/InstrRef.h"
#include "editor/OsCallRef.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

namespace {

/// The context an `Example:` may assume, exactly as `InstrRef.h` and
/// `OsCallRef.h` document it: the label `start`, and `msg` / `buffer` as data.
/// Anything else is not self-contained, which is what the assembler run below
/// is for.
QString wrapped(const QString &example)
{
    return QStringLiteral("\ttext\nstart:\n%1\n"
                          "msg:\tdc.b\t\"hi\",0\n"
                          "buffer:\tds.b\t64\n"
                          "\tend\n")
        .arg(example);
}

/// Assemble `source` with vasm. `-m68060` because the reference covers the
/// 68020+/FPU names vasm accepts — the point is the grammar, not the target.
bool assembles(const QString &vasm, const QString &dir, const QString &source, QString *why)
{
    const QString asmPath = dir + QStringLiteral("/ref.s");
    const QString outPath = dir + QStringLiteral("/ref.bin");
    QFile file(asmPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *why = file.errorString();
        return false;
    }
    file.write(source.toUtf8());
    file.close();

    QProcess process;
    process.start(vasm, {QStringLiteral("-quiet"), QStringLiteral("-m68060"),
                         QStringLiteral("-Ftos"), QStringLiteral("-o"), outPath, asmPath});
    if (!process.waitForFinished(20000)) {
        *why = QStringLiteral("vasm did not finish");
        return false;
    }
    if (process.exitCode() != 0) {
        *why = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

/// The first word of an example, lower-cased: for an instruction that is the
/// mnemonic it is an example of. The fragment is indented like source, so this
/// splits on any whitespace rather than on a tab or a space specifically.
QString firstToken(const QString &example)
{
    const QString firstLine = example.section(QLatin1Char('\n'), 0, 0).trimmed();
    return firstLine.split(QRegularExpression(QStringLiteral("\\s+"))).value(0).toLower();
}

} // namespace

class TstReference : public QObject
{
    Q_OBJECT

private slots:
    /// Nothing in either table is left bare: an entry without its summary, its
    /// example or its one sentence of when is unfinished, and the two escape
    /// hatches the headers describe ("empty when it is not worth one") are
    /// decisions to revisit the entry, not silent omissions.
    void everyEntryIsAuthored();

    /// Each example is a claim that this is how the thing is written. vasm is
    /// the only authority for that, so it is the one asked — skipped cleanly
    /// when it is not installed, like every other suite that needs it.
    void everyExampleAssembles();

    /// An example under the wrong entry is worse than no example: the mnemonic
    /// an instruction's fragment starts with must resolve to that instruction,
    /// and a call's fragment must make the trap the entry describes.
    void examplesBelongToTheirEntry();

    /// A `Typical use:` is one sentence, not a paragraph: the dock has room for
    /// a line and the field is meant to be read at a glance.
    void typicalUseReadsAsOneSentence();

    /// The lookup rules the dock promises the user: `addq.l`, `ADDQ` and
    /// `move.w` resolve, and every entry resolves from its own spelling.
    void theLookupRulesHold();
};

void TstReference::everyEntryIsAuthored()
{
    // Every unfinished entry is reported in one pass: a list of names is what
    // the authoring work is done from, and a test that stopped at the first
    // would turn one review into two hundred runs.
    const auto state = [](bool present) {
        return present ? QStringLiteral("ok") : QStringLiteral("missing");
    };
    QStringList unfinished;
    const auto note = [&unfinished, &state](const QString &name, const InstructionInfo &info) {
        if (info.summary.isEmpty() || info.example.isEmpty() || info.typicalUse.isEmpty()) {
            unfinished << QStringLiteral("%1 (summary %2, example %3, typical use %4)")
                              .arg(name)
                              .arg(state(!info.summary.isEmpty()),
                                   state(!info.example.isEmpty()),
                                   state(!info.typicalUse.isEmpty()));
        }
    };
    for (const InstructionInfo &info : instructionTable())
        note(info.mnemonic, info);
    for (const OsCallInfo &info : osCallTable())
        note(info.name, InstructionInfo{info.name, info.summary, {}, info.example, info.typicalUse});

    if (unfinished.isEmpty())
        return;
    const QStringList shown = unfinished.mid(0, 20);
    QString message = QStringLiteral("%1 entries are unfinished:\n%2")
                          .arg(unfinished.size())
                          .arg(shown.join(QLatin1Char('\n')));
    if (unfinished.size() > shown.size())
        message += QStringLiteral("\n… and %1 more").arg(unfinished.size() - shown.size());
    QVERIFY2(false, qPrintable(message));
}

void TstReference::everyExampleAssembles()
{
    const QString vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (vasm.isEmpty())
        QSKIP("needs vasmm68k_mot");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    for (const InstructionInfo &info : instructionTable()) {
        if (info.example.isEmpty())
            continue; // reported by everyEntryIsAuthored
        QString why;
        const QString source = wrapped(info.example);
        QVERIFY2(assembles(vasm, dir.path(), source, &why),
                 qPrintable(QStringLiteral("%1: %2\n%3").arg(info.mnemonic, why, source)));
    }

    for (const OsCallInfo &info : osCallTable()) {
        if (info.example.isEmpty())
            continue;
        QString why;
        const QString source = wrapped(info.example);
        QVERIFY2(assembles(vasm, dir.path(), source, &why),
                 qPrintable(QStringLiteral("%1: %2\n%3").arg(info.name, why, source)));
    }
}

void TstReference::examplesBelongToTheirEntry()
{
    for (const InstructionInfo &info : instructionTable()) {
        if (info.example.isEmpty())
            continue;
        const InstructionInfo *resolved = instructionRef(firstToken(info.example));
        QVERIFY2(resolved, qPrintable(info.mnemonic + ": its example starts with a word the "
                                                    "reference does not know"));
        QCOMPARE(resolved->mnemonic, info.mnemonic);
    }

    for (const OsCallInfo &info : osCallTable()) {
        if (info.example.isEmpty())
            continue;
        const QString trap = QStringLiteral("trap #%1").arg(info.trap);
        QVERIFY2(info.example.contains(trap),
                 qPrintable(QStringLiteral("%1: its example never makes %2").arg(info.name, trap)));
    }
}

void TstReference::typicalUseReadsAsOneSentence()
{
    const auto check = [](const QString &what, const QString &sentence) {
        if (sentence.isEmpty())
            return; // reported by everyEntryIsAuthored
        QVERIFY2(!sentence.contains(QLatin1Char('\n')), qPrintable(what + ": more than one line"));
        QVERIFY2(sentence.trimmed() == sentence, qPrintable(what + ": leading or trailing space"));
        QVERIFY2(sentence.endsWith(QLatin1Char('.')), qPrintable(what + ": not a sentence"));
        QVERIFY2(sentence.size() <= 120, qPrintable(what + ": too long to read at a glance"));
    };
    for (const InstructionInfo &info : instructionTable())
        check(info.mnemonic, info.typicalUse);
    for (const OsCallInfo &info : osCallTable())
        check(info.name, info.typicalUse);
}

void TstReference::theLookupRulesHold()
{
    for (const InstructionInfo &info : instructionTable()) {
        const InstructionInfo *exact = instructionRef(info.mnemonic);
        QVERIFY2(exact, qPrintable(info.mnemonic + " does not resolve"));
        QCOMPARE(exact->mnemonic, info.mnemonic);

        // Case-insensitive, and a size suffix is not part of the name.
        const InstructionInfo *upper = instructionRef(info.mnemonic.toUpper());
        QVERIFY2(upper, qPrintable(info.mnemonic.toUpper() + " does not resolve"));
        QCOMPARE(upper->mnemonic, info.mnemonic);
        const InstructionInfo *suffixed = instructionRef(info.mnemonic + QStringLiteral(".l"));
        QVERIFY2(suffixed, qPrintable(info.mnemonic + ".l does not resolve"));
        QCOMPARE(suffixed->mnemonic, info.mnemonic);
    }

    for (const OsCallInfo &info : osCallTable()) {
        const OsCallInfo *exact = osCallRef(info.trap, info.opcode);
        QVERIFY2(exact, qPrintable(QStringLiteral("%1 does not resolve").arg(info.name)));
        QCOMPARE(exact->name, info.name);
        QVERIFY2(!osCallLayerName(info.trap).isEmpty(),
                 qPrintable(QStringLiteral("%1 has no layer name").arg(info.trap)));
        QVERIFY2(!osCallLayerKey(info.trap).isEmpty(),
                 qPrintable(QStringLiteral("%1 has no layer key").arg(info.trap)));
    }
}

QTEST_MAIN(TstReference)
#include "tst_reference.moc"
