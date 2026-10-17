// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/CompletionModel.h"

#include "editor/InstrRef.h"
#include "editor/Vocabulary.h"

#include <utility>

namespace pist {

QList<CompletionItem> CompletionModel::vocabulary()
{
    QList<CompletionItem> items;
    const QStringList &mnemonics = mnemonicWords();
    const QStringList &directives = directiveWords();
    items.reserve(mnemonics.size() + directives.size());

    for (const QString &word : mnemonics) {
        CompletionItem item;
        item.text = word;
        item.kind = CompletionKind::Mnemonic;
        // The reference's own summary, where the table has the word: the extras
        // (bkpt, the FPU names) are deliberately not in it, and a palette line is
        // better left empty than filled with an invented description.
        if (const InstructionInfo *info = instructionRef(word))
            item.detail = info->summary;
        items.append(item);
    }
    for (const QString &word : directives) {
        CompletionItem item;
        item.text = word;
        item.kind = CompletionKind::Directive;
        items.append(item);
    }
    return items;
}

CompletionModel::CompletionModel(QList<CompletionItem> items)
    : m_items(std::move(items))
{
}

void CompletionModel::setItems(QList<CompletionItem> items)
{
    m_items = std::move(items);
}

QList<CompletionItem> CompletionModel::matches(const QString &prefix, int limit) const
{
    QList<CompletionItem> ranked;
    if (prefix.isEmpty()) {
        // "Show me what there is": Ctrl+Space with no word under the cursor, and a
        // palette with an empty filter.
        ranked = m_items;
    } else {
        ranked.reserve(m_items.size());
        // Prefix matches first: typing `mov` means `move`/`movem`, not `movem`
        // found in the middle of some longer name.
        for (const CompletionItem &item : m_items) {
            if (item.text.startsWith(prefix, Qt::CaseInsensitive))
                ranked.append(item);
        }
        for (const CompletionItem &item : m_items) {
            if (!item.text.startsWith(prefix, Qt::CaseInsensitive)
                && item.text.contains(prefix, Qt::CaseInsensitive)) {
                ranked.append(item);
            }
        }
    }

    if (limit > 0 && ranked.size() > limit)
        ranked = ranked.mid(0, limit);
    return ranked;
}

} // namespace pist
