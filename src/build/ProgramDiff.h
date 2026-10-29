// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/LinkMap.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace pist {

/// A label and where it sits in an output section, as an offset from the start
/// of that section. `section` is `text`, `data` or `bss`.
struct SectionAnchor
{
    QString section;
    quint32 offset = 0;
    QString name;
};

enum class SectionDecision { Identical, DifferingRanges, RestartRequired };

enum class RestartReason { None, Grew, Shrank, Moved };

/// One run of bytes that differ, inside a section whose length did not change.
struct ByteRange
{
    quint32 offset = 0;
    int length = 0;
    /// The nearest anchor at or before `offset` in this section. Empty when
    /// none contains the start.
    QString symbol;
    /// At most eight bytes, so a long run does not become a hex dump.
    QByteArray oldPreview;
    QByteArray newPreview;
};

struct SectionChange
{
    QString name;
    SectionDecision decision = SectionDecision::Identical;
    RestartReason reason = RestartReason::None;
    quint32 oldSize = 0;
    quint32 newSize = 0;
    QList<ByteRange> ranges;
};

/// What changed between two TOS executables. A preview: nothing here writes
/// to a machine, and a size or layout change is restart-required rather than
/// a list of ranges that would look patchable.
struct ProgramDiff
{
    bool ok = false;
    QString error;
    QList<SectionChange> sections;
};

/// Compare two TOS program images.
///
/// Text and data are the bytes in the file. BSS has a length in the header
/// and no bytes. When both link maps are supplied, a section whose placements
/// moved is restart-required even if the file length matches: same length does
/// not mean the bytes still sit where a running program expects them.
ProgramDiff diffTosPrograms(const QByteArray &previous, const QByteArray &current,
                            const QList<SectionAnchor> &anchors,
                            const LinkMap *previousMap, const LinkMap *currentMap);

/// `<program dir>/build/previous/<program file name>`. The snapshot taken
/// before a build replaces the program.
QString previousBuildProgram(const QString &programPath);

/// The same directory, named after the link map.
QString previousBuildLinkMap(const QString &linkMapPath);

/// Copy the program, and the link map when `linkMapPath` names a file, into
/// the previous-build directory. A program that is not there yet is not an
/// error. A failed copy leaves whatever snapshot was already there.
bool snapshotPreviousBuild(const QString &programPath, const QString &linkMapPath, QString *error);

} // namespace pist
