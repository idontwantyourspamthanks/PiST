// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace pist {

/// One DRI/GST symbol from a TOS executable. `value` is image-relative for
/// text, data, and bss (an offset from the first byte of text, which is where
/// GEMDOS loads the program). An equate keeps the value stored in the file.
struct TosSymbol
{
    enum class Kind { Text, Data, Bss, Equate, Other };

    QString name;
    Kind kind = Kind::Other;
    quint32 value = 0;
    bool global = false;
};

/// A TOS relocatable executable (magic `$601A`), addresses counted from the
/// start of the text segment. The image is text followed by data. Bss is a
/// length only. Relocation offsets name longwords GEMDOS will fix up.
struct TosProgram
{
    quint32 textSize = 0;
    quint32 dataSize = 0;
    quint32 bssSize = 0;
    quint32 flags = 0;
    bool relocatable = false;
    QByteArray image;
    QVector<TosSymbol> symbols;
    QVector<quint32> relocations;
};

/// False, with `*error` set, when `bytes` is not a complete TOS executable.
bool parseTosProgram(const QByteArray &bytes, TosProgram *out, QString *error);

} // namespace pist
