// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/AnalysisNotes.h"
#include "analysis/TosProgram.h"

#include <QString>
#include <QVector>

namespace pist {

/// One row of the listing. Code rows are instructions the walk reached.
/// Everything else in text and data is `dc.b`. `length` is the number of image
/// bytes the row covers; a bss label that has no bytes of its own is 0.
/// `xrefs` names the instructions that refer to this row. `comment` is the
/// user's, from the notes.
struct ListingRow
{
    quint32 address = 0;
    quint32 length = 0;
    QString label;
    QString bytes;
    QString text;
    QString note;
    QString xrefs;
    QString comment;
};

struct ProgramListing
{
    QString summary;
    QString error;
    /// Text plus data. Bss has no bytes.
    quint32 imageSize = 0;
    /// The first address past bss. A name may sit anywhere below this.
    quint32 endAddress = 0;
    QVector<ListingRow> rows;
};

/// Parse `bytes` and walk from address 0, and from every code mark in `notes`.
/// A data mark is not decoded and stops fall-through. Indirect jumps are not
/// followed. `trap #1` of Pterm0 or Pterm ends the path. False starts are left
/// as data, including a string that merely follows a call that returns. Names
/// and comments in `notes` are applied on top of the executable's symbols.
ProgramListing listProgram(const QByteArray &bytes, const AnalysisNotes &notes = {});

} // namespace pist
