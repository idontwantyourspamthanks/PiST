// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/AnalysisNotes.h"
#include "analysis/TosProgram.h"

#include <QString>
#include <QVector>

#include <optional>

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
    quint32 textSize = 0;
    quint32 dataSize = 0;
    quint32 bssSize = 0;
    /// Addresses an instruction names: a branch or call target, an absolute or
    /// pc-relative effective address, or a relocated immediate. Image-relative.
    QVector<quint32> referenced;
    QVector<ListingRow> rows;
};

/// Parse `bytes` and walk from address 0, and from every code mark in `notes`.
/// A data mark is not decoded and stops fall-through. Indirect jumps are not
/// followed. `trap #1` of Pterm0 or Pterm ends the path. False starts are left
/// as data, including a string that merely follows a call that returns. Names
/// and comments in `notes` are applied on top of the executable's symbols.
ProgramListing listProgram(const QByteArray &bytes, const AnalysisNotes &notes = {});

/// Where `pc` sits in the image, given the live bases from `info basepage` and
/// this program's own section sizes. Text, data and bss are not assumed to be
/// contiguous: a PC in the gap between them is not an image address. A text
/// base of 0 means no program is loaded. The result is not a relocated address
/// and is never written into the notes file.
std::optional<quint32> imageAddressForPc(quint32 pc, quint32 textBase, quint32 dataBase,
                                         quint32 bssBase, quint32 textSize, quint32 dataSize,
                                         quint32 bssSize);

} // namespace pist
