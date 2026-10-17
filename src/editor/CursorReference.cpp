// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/CursorReference.h"

#include "editor/IncludeNav.h"
#include "editor/OsCallScan.h"

namespace pist {

CursorReference referenceAt(const QStringList &lines, int lineIndex, int column)
{
    CursorReference reference;
    if (lineIndex < 0 || lineIndex >= lines.size())
        return reference;

    const OsCallMatch match = osCallAt(lines, lineIndex);
    if (match.call) {
        reference.call = match.call;
        reference.args = match.args;
        return reference;
    }
    if (match.trapContext) {
        reference.trapContext = true;
        return reference;
    }

    // The scanner says nothing about this line, so the word under the cursor is
    // what is left — looked up by the caller, which is the only party with the
    // reference table and the symbols.
    reference.word = wordAtCursor(lines.at(lineIndex), column);
    return reference;
}

} // namespace pist
