// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The completion feature's widget-free half: the assembler's vocabulary, the
// ranking the popup and the palette share, and the cursor resolution the dock and
// the hover card share (docs/glm-features.md §4).

#include "editor/CompletionModel.h"
#include "editor/CursorReference.h"
#include "editor/Vocabulary.h"

#include <QSet>
#include <QtTest>

using namespace pist;

namespace {

/// The symbol with `name` in a candidate list, or nullptr.
const CompletionItem *byName(const QList<CompletionItem> &items, const QString &name)
{
    for (const CompletionItem &item : items) {
        if (item.text == name)
            return &item;
    }
    return nullptr;
}

CompletionItem item(const QString &text)
{
    CompletionItem entry;
    entry.text = text;
    return entry;
}

} // namespace

class TstCompletion : public QObject
{
    Q_OBJECT

private slots:
    /// The two families are the assembler's own: what the highlighter paints and
    /// what completion offers are the same words, which is the point of the shared
    /// table.
    void theVocabularyIsTheAssemblersOwn();
    /// The ranking both surfaces share: prefixes before mere substrings, model
    /// order within each, case-insensitively.
    void theModelRanksPrefixMatchesFirst();
    /// A palette line is the reference's summary where there is one, and empty
    /// rather than invented where there is not.
    void theVocabularyCarriesTheReferenceSummary();
    /// The one resolver: an OS call wins on its line, a trap without a known
    /// function is context, and a plain line is its word.
    void theResolverFindsCallsContextAndWords();
};

void TstCompletion::theVocabularyIsTheAssemblersOwn()
{
    const QStringList &mnemonics = mnemonicWords();
    const QStringList &directives = directiveWords();
    QVERIFY(!mnemonics.isEmpty());
    QVERIFY(!directives.isEmpty());

    QVERIFY(mnemonics.contains(QStringLiteral("move")));
    QVERIFY2(mnemonics.contains(QStringLiteral("bkpt")),
             "the 68020+ name vasm accepts and the 68000 reference leaves out");
    QVERIFY2(!mnemonics.contains(QStringLiteral("dc.b")), "a directive is not a mnemonic");
    QVERIFY(directives.contains(QStringLiteral("dc.b")));
    QVERIFY(directives.contains(QStringLiteral("equ")));
    QVERIFY2(!directives.contains(QStringLiteral("move")), "a mnemonic is not a directive");

    // Lowercase: vasm is case-insensitive and a source reads lowercase, so a
    // candidate that inserted `ADDQ` into one would look foreign.
    for (const QString &word : mnemonics)
        QCOMPARE(word, word.toLower());
    for (const QString &word : directives)
        QCOMPARE(word, word.toLower());

    // No repeats: a duplicated word would be offered twice in the popup and listed
    // twice in the palette.
    const QSet<QString> uniqueMnemonics(mnemonics.begin(), mnemonics.end());
    const QSet<QString> uniqueDirectives(directives.begin(), directives.end());
    QCOMPARE(uniqueMnemonics.size(), mnemonics.size());
    QCOMPARE(uniqueDirectives.size(), directives.size());
}

void TstCompletion::theModelRanksPrefixMatchesFirst()
{
    CompletionModel model({item(QStringLiteral("movem")), item(QStringLiteral("move")),
                           item(QStringLiteral("amove")), item(QStringLiteral("mv"))});

    const QList<CompletionItem> matches = model.matches(QStringLiteral("mov"));
    QCOMPARE(matches.size(), 3);
    QCOMPARE(matches.at(0).text, QStringLiteral("movem")); // prefixes first, in model order
    QCOMPARE(matches.at(1).text, QStringLiteral("move"));
    QCOMPARE(matches.at(2).text, QStringLiteral("amove")); // contains it: ranked after

    // vasm is case-insensitive, so the filter is too.
    QCOMPARE(model.matches(QStringLiteral("MOVE")).size(), 3);

    // The limit keeps the best matches, not the first found.
    const QList<CompletionItem> limited = model.matches(QStringLiteral("mov"), 1);
    QCOMPARE(limited.size(), 1);
    QCOMPARE(limited.first().text, QStringLiteral("movem"));

    // An empty prefix is everything: Ctrl+Space and an empty palette filter.
    QCOMPARE(model.matches(QString()).size(), 4);
}

void TstCompletion::theVocabularyCarriesTheReferenceSummary()
{
    const QList<CompletionItem> vocabulary = CompletionModel::vocabulary();

    const CompletionItem *move = byName(vocabulary, QStringLiteral("move"));
    QVERIFY(move);
    QCOMPARE(move->kind, CompletionKind::Mnemonic);
    QVERIFY2(!move->detail.isEmpty(), "the reference's summary is the palette's line");

    const CompletionItem *directive = byName(vocabulary, QStringLiteral("dc.b"));
    QVERIFY(directive);
    QCOMPARE(directive->kind, CompletionKind::Directive);

    // An extra the reference does not carry has no summary, and says so by leaving
    // the line empty rather than inventing one.
    const CompletionItem *extra = byName(vocabulary, QStringLiteral("bkpt"));
    QVERIFY(extra);
    QVERIFY(extra->detail.isEmpty());
}

void TstCompletion::theResolverFindsCallsContextAndWords()
{
    // Cconws: push the string, push function 9, trap.
    const QStringList cconws = {
        QStringLiteral("start:"), QStringLiteral("\tmove.l\t#msg,-(a7)"),
        QStringLiteral("\tmove.w\t#9,-(a7)"), QStringLiteral("\ttrap\t#1"),
        QStringLiteral("\taddq.l\t#6,a7")};
    const CursorReference trap = referenceAt(cconws, 3, 1);
    QVERIFY(trap.call);
    QCOMPARE(trap.call->name, QStringLiteral("Cconws"));
    QCOMPARE(trap.args, QList<QString>{QStringLiteral("#msg")});

    // The push above the trap is part of the call: the call wins over the word.
    QVERIFY(referenceAt(cconws, 1, 9).call);

    // A function number in a register is context, not a call: nothing is guessed.
    const QStringList unknown = {QStringLiteral("\tmove.w\td0,-(sp)"),
                                 QStringLiteral("\ttrap\t#1")};
    const CursorReference context = referenceAt(unknown, 1, 1);
    QVERIFY(!context.call);
    QVERIFY(context.trapContext);

    // A plain line is its word, in the lexer's alphabet: the size suffix and the
    // label's underscore stay in it.
    const QStringList plain = {QStringLiteral("\tmove.w\tfoo_bar(a4)")};
    QCOMPARE(referenceAt(plain, 0, 3).word, QStringLiteral("move.w"));
    QCOMPARE(referenceAt(plain, 0, 12).word, QStringLiteral("foo_bar"));

    // Out of range resolves to nothing rather than crashing.
    const CursorReference none = referenceAt(plain, 9, 0);
    QVERIFY(!none.call);
    QVERIFY(!none.trapContext);
    QVERIFY(none.word.isEmpty());
}

QTEST_MAIN(TstCompletion)
#include "tst_completion.moc"
