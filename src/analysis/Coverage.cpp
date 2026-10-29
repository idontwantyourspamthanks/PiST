// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/Coverage.h"

#include "analysis/ProgramTrace.h"

#include <QRegularExpression>

namespace pist {

namespace {

const QRegularExpression &sampleLine()
{
    static const QRegularExpression pattern(QStringLiteral("^\\$([0-9a-f]{6}) ([1-9][0-9]*)$"));
    return pattern;
}

} // namespace

bool parseCoverage(const QString &text, QVector<CoverageSample> *samples, QString *error)
{
    if (samples)
        samples->clear();
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    QStringList rows;
    for (QString line : text.split(QLatin1Char('\n'))) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        rows.append(line);
    }
    while (!rows.isEmpty() && rows.last().isEmpty())
        rows.removeLast();
    if (rows.size() < 2 || rows.at(0) != QLatin1String("Hatari CPU coverage"))
        return fail(QStringLiteral("not a Hatari CPU coverage file"));

    const QString prefix = QStringLiteral("Addresses: ");
    if (!rows.at(1).startsWith(prefix))
        return fail(QStringLiteral("not a Hatari CPU coverage file"));
    bool countOk = false;
    const uint declared = rows.at(1).mid(prefix.size()).toUInt(&countOk);
    if (!countOk || rows.size() != int(declared) + 2)
        return fail(QStringLiteral("the coverage file's address count does not match its lines"));

    QVector<CoverageSample> parsed;
    parsed.reserve(int(declared));
    quint32 previous = 0;
    for (int i = 0; i < int(declared); ++i) {
        const QRegularExpressionMatch match = sampleLine().match(rows.at(i + 2));
        if (!match.hasMatch())
            return fail(QStringLiteral("the coverage file has a line that is not an address and a count"));
        bool addressOk = false;
        bool hitsOk = false;
        const quint32 address = match.captured(1).toUInt(&addressOk, 16);
        const quint32 count = match.captured(2).toUInt(&hitsOk, 10);
        if (!addressOk || (address & 1u) != 0)
            return fail(QStringLiteral("the coverage file has an address that is not an even ST RAM word"));
        if (!hitsOk)
            return fail(QStringLiteral("the coverage file has a count that is not a number"));
        if (i > 0 && address <= previous)
            return fail(QStringLiteral("the coverage file's addresses are not in ascending order"));
        previous = address;
        parsed.append(CoverageSample{address, count});
    }
    if (samples)
        *samples = parsed;
    if (error)
        error->clear();
    return true;
}

QVector<quint32> coverageImageAddresses(const QVector<CoverageSample> &samples, quint32 textBase,
                                        quint32 dataBase, quint32 bssBase, quint32 textSize,
                                        quint32 dataSize, quint32 bssSize)
{
    QVector<quint32> image;
    image.reserve(samples.size());
    for (const CoverageSample &sample : samples) {
        const std::optional<quint32> at = imageAddressForPc(sample.address, textBase, dataBase, bssBase,
                                                            textSize, dataSize, bssSize);
        if (at)
            image.append(*at);
    }
    return image;
}

} // namespace pist
