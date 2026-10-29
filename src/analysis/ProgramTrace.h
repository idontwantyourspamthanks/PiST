// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/TosProgram.h"

#include <QString>
#include <QVector>

namespace pist {

/// One row of the read-only listing. Code rows are instructions reached from
/// the entry point. Everything else in text and data is `dc.b`.
struct ListingRow
{
    quint32 address = 0;
    QString label;
    QString bytes;
    QString text;
    QString note;
};

struct ProgramListing
{
    QString summary;
    QString error;
    QVector<ListingRow> rows;
};

/// Parse `bytes` and walk the text segment from address 0. Indirect jumps are
/// not followed. `trap #1` of Pterm0 or Pterm ends the path. False starts are
/// left as data, including a string that merely follows a call that returns.
ProgramListing listProgram(const QByteArray &bytes);

} // namespace pist
