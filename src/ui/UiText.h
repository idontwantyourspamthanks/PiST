// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// The first line of a multi-line message. Refusal explanations are written for
/// a dialog and run to several lines; the status bar is one line, while the
/// console keeps the whole text.
inline QString firstLine(const QString &text)
{
    return text.section(QLatin1Char('\n'), 0, 0).trimmed();
}

} // namespace pist
