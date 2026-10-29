// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QVector>

namespace pist {

/// One address from a hatari-pist `coverage save` file. The address is the
/// absolute PC the instruction finished at, not an image offset.
struct CoverageSample
{
    quint32 address = 0;
    quint32 count = 0;
};

/// Read the text `coverage save` writes. True only for that exact layout:
/// the header, `Addresses: N`, then N lines of `$` + six lowercase hex digits,
/// a space, and a decimal count, ascending. An empty run is the header and
/// `Addresses: 0`.
bool parseCoverage(const QString &text, QVector<CoverageSample> *samples, QString *error);

/// The image addresses among `samples` that fall in this program's text, data
/// or bss. Anything else — TOS, the gap between the sections, a machine with
/// no loaded program — is left out. The same translation as the stopped PC.
QVector<quint32> coverageImageAddresses(const QVector<CoverageSample> &samples, quint32 textBase,
                                        quint32 dataBase, quint32 bssBase, quint32 textSize,
                                        quint32 dataSize, quint32 bssSize);

} // namespace pist
