// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace pist {

struct AvrInfo
{
    int rate = 0;
    bool signedSamples = false;
    quint32 length = 0;
};

/// `header` is the 128 bytes at the start of an AVR file. False when the magic,
/// channel count, or bit depth is not the one case this converts: mono, 8-bit,
/// rate in 4000..50000, length in 1..65536.
bool parseAvrHeader(const QByteArray &header, AvrInfo *out, QString *error);

/// `fileBytes` holds the header and the samples. Writes nothing; returns the
/// WAV from wavFromPcm8. The sample bytes are fileBytes.mid(128, info.length).
QByteArray wavFromAvr(const QByteArray &fileBytes, QString *error);

struct MagicHit
{
    int offset = 0;
    QString kind;
};

/// AVR (mono 8-bit only), IFF ILBM, Ice and Atomik. At most 64 hits.
/// A `2BIT` whose header is stereo or 16-bit is not a hit. Degas and NeoChrome
/// are not scanned.
QVector<MagicHit> scanMagic(const QByteArray &bytes);

/// "ice" for ICE! / Ice!, "atomik" for ATM5, empty otherwise.
/// Looks only at the first four bytes.
QString packerMagic(const QByteArray &bytes);

struct SndhTags
{
    /// True when SNDH was found in the first 512 bytes.
    bool found = false;
    /// True when a tag other than the string tags and HDNS was seen, or HDNS
    /// was not reached.
    bool truncated = false;
    QString title;
    QString composer;  ///< COMM
    QString ripper;    ///< RIPP
    QString converter; ///< CONV
    QString year;      ///< YEAR
};

SndhTags parseSndhTags(const QByteArray &bytes);

} // namespace pist
