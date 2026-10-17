// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The candidates the editor completes from and the palette jumps with: the
// assembler's own vocabulary plus the build's symbols, ranked one way
// (docs/glm-features.md §4 — one model, three surfaces).

#pragma once

#include <QList>
#include <QString>

namespace pist {

/// What a candidate is.
///
/// The editor's popup does not care beyond the word, but the palette shows the
/// kind, and the hover uses the symbol kinds to decide what a word means.
enum class CompletionKind { Mnemonic, Directive, Label, Equate };

/// One thing completion can offer, or the palette can jump to.
struct CompletionItem
{
    /// The word, lowercase for the assembler's own vocabulary (vasm is
    /// case-insensitive and a source reads lowercase) and as written for a symbol.
    QString text;
    CompletionKind kind = CompletionKind::Mnemonic;
    /// A label's or equate's definition. Empty when the build cannot say where it
    /// is — a command-line define has no source position — so a caller shows
    /// nothing rather than a made-up location.
    QString file;
    int line = 0;
    /// One line for the palette: a mnemonic's summary, or empty when there is
    /// nothing honest to add. The popup does not show it; the palette does.
    QString detail;
};

/// The candidates, filtered and ranked.
///
/// The vocabulary is static (`vocabulary()`); the symbols are pushed in from the
/// window, which is the only party that knows the build — the editor deliberately
/// sees no symbol table (MIN-86).
class CompletionModel
{
public:
    /// The assembler's own words as candidates: the mnemonics (the 68000 reference
    /// plus the 68020+/FPU names vasm accepts) and the directives, in
    /// `editor/Vocabulary.h`'s order — the order they are offered in.
    static QList<CompletionItem> vocabulary();

    explicit CompletionModel(QList<CompletionItem> items = {});

    void setItems(QList<CompletionItem> items);
    const QList<CompletionItem> &items() const { return m_items; }

    /// Ranked candidates for `prefix`, at most `limit` of them (`<= 0` keeps every
    /// one): names that *start* with it first, then names that merely contain it,
    /// each keeping the model's order. An empty prefix matches everything, which is
    /// what Ctrl+Space and an empty palette filter both mean.
    QList<CompletionItem> matches(const QString &prefix, int limit = 0) const;

private:
    QList<CompletionItem> m_items;
};

} // namespace pist
