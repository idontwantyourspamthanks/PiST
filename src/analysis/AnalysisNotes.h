// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QString>
#include <QVector>

namespace pist {

/// Suffix of the notes file that sits beside a program: `game.prg` keeps its
/// names, comments and code/data marks in `game.prg.pistanalysis`. The program
/// itself is never written.
inline constexpr const char *kAnalysisNotesSuffix = ".pistanalysis";

/// One user decision about a span of the image. Addresses are image-relative.
/// Ranges in a notes value do not overlap; a later mark replaces the bytes it
/// shares with an earlier one, and adjacent marks of the same kind are merged.
struct AnalysisOverride
{
    enum class Kind { Code, Data };

    quint32 address = 0;
    quint32 length = 0;
    Kind kind = Kind::Data;
};

/// What the user has said about a program. Cross-references are not stored:
/// the listing computes them from the bytes and these marks.
struct AnalysisNotes
{
    QHash<quint32, QString> names;
    QHash<quint32, QString> comments;
    QVector<AnalysisOverride> overrides;
};

QString analysisNotesPath(const QString &programPath);

/// A missing file is an empty note set, not an error. A file that is present
/// but unreadable or not version 1 sets `*error` and leaves `*out` empty.
bool loadAnalysisNotes(const QString &path, AnalysisNotes *out, QString *error);

/// Writes version 1. An empty note set removes `path` when it exists. A
/// failure leaves whatever was already there.
bool saveAnalysisNotes(const QString &path, const AnalysisNotes &notes, QString *error);

/// An empty name removes the user's name. A symbol from the executable is
/// left alone. A name is an identifier: a letter, '_' or '.', then letters,
/// digits, '_', '.' and '$', at most 64 characters.
bool setAnalysisName(AnalysisNotes *notes, quint32 address, const QString &name, QString *error);

/// An empty comment removes it. A comment is one line of at most 200 characters.
bool setAnalysisComment(AnalysisNotes *notes, quint32 address, const QString &text, QString *error);

/// `length` is a number of bytes. Code starts on an even address. The new
/// mark replaces any mark it overlaps.
bool setAnalysisOverride(AnalysisNotes *notes, quint32 address, quint32 length,
                         AnalysisOverride::Kind kind, QString *error);

/// Drops any mark on `[address, address + length)`.
bool clearAnalysisOverride(AnalysisNotes *notes, quint32 address, quint32 length, QString *error);

} // namespace pist
