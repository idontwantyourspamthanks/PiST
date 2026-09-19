// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pist {

/// FAT12 floppy image authoring and listing.
///
/// TOS 1.00/1.02 have no GEMDOS-HD autostart (Hatari refuses GEMDOS HD
/// outright below 1.04 — docs/PLAN.md §5 rule 3), but every TOS executes
/// AUTO/*.PRG from the boot floppy. Writing the image ourselves keeps the
/// fallback free of an mtools/dosfstools dependency, and the layout is pinned
/// against a real mkfs.vfat image, not guesswork.
///
/// The same writer also builds a 720 KiB image from arbitrary host files
/// (the project-files "Export…" path), as raw `.st` or compressed `.msa`.
namespace floppy {

/// Write a 720 KiB FAT12 image containing AUTO/PROG.PRG (the program TOS will
/// execute on boot) and an empty EMUDESK.INF at the root. The INF matters
/// beyond cosmetics: Hatari's `--debug-except autostart,...` mask arms when
/// TOS loads the INF, and without one the exception breakpoints never arm at
/// all on this boot path.
bool writeAutoFolderImage(const QString &imagePath, const QString &programPath,
                          QString *error);

/// One entry inside a floppy's FAT12 tree. `path` uses `/` and TOS 8.3 names
/// (`AUTO/PROG.PRG`). Directories are listed in addition to the files they
/// contain, so an empty folder is still visible.
struct Entry {
    QString path;
    bool isDirectory = false;
    quint32 size = 0;
    /// The entry's first data cluster, so a caller can read this exact entry's
    /// bytes without re-resolving its name — FAT allows duplicate 8.3 names, and
    /// a name lookup returns the first match for all of them (finding B9).
    quint16 cluster = 0;
};

/// One host file or folder to place on a floppy. `destPath` is relative to the
/// floppy root (`AUTO/PROG.PRG`); each component is converted to 8.3.
struct Item {
    QString destPath;
    QByteArray data;
    bool isDirectory = false;
};

/// Decode a `.st` (raw) or `.msa` (Magic Shadow Archiver) image to a flat
/// sector dump. `.dim` is the same dump with a 32-byte header stripped.
bool loadRaw(const QString &imagePath, QByteArray *raw, QString *error);

/// Encode `raw` as `.msa` when `imagePath` ends in that suffix, otherwise
/// write it as a raw dump (`.st` / `.img`).
bool saveRaw(const QString &imagePath, const QByteArray &raw, QString *error);

/// List the FAT12 directory tree of a decoded image.
QVector<Entry> listRaw(const QByteArray &raw, QString *error);

/// Load an image file and list it.
QVector<Entry> listImage(const QString &imagePath, QString *error);

/// Whether a decoded image already has the layout `updateImage` writes: the
/// canonical 720 KiB geometry with the `mkfs.fat` OEM field. Anything else —
/// a game disk's own boot code, a 1.44 MB disk — is *rewritten* rather than
/// edited, losing whatever made it different, so a caller can ask first.
/// `firstSector` is the decoded image's first 512 bytes (not the file's: a
/// `.msa` file starts with its own header), and `imageSize` the decoded size.
bool looksLikeCanonical720k(const QByteArray &firstSector, qint64 imageSize);

/// Read one file's content out of a decoded image. `entryPath` is the
/// `/`-separated path `listRaw` reports (`AUTO/PROG.PRG`).
bool readFileRaw(const QByteArray &raw, const QString &entryPath,
                 QByteArray *data, QString *error);

/// Rewrite an existing image in place: drop `removals` (an entry removes its
/// whole subtree), then add `additions` — the operations behind copying files
/// onto a floppy and moving them off one. An addition whose 8.3 name already
/// exists on the disk is given a numbered name rather than failing the write.
/// The new contents are staged to a sibling temporary file, so a failure never
/// truncates the original, and the staged image only replaces the original
/// once complete. The container format follows the suffix (`.st`, `.img`,
/// `.msa`); `.dim` and `.ipf` cannot be written back and are refused. The
/// FAT12 layout is always rebuilt with the canonical 720 KiB geometry.
bool updateImage(const QString &imagePath, const QVector<Item> &additions,
                 const QStringList &removals, QString *error);

/// Write a 720 KiB FAT12 image containing `items` (files and directories).
/// Empty files are allowed; empty directories are kept. Fails when the
/// contents do not fit, or when the root directory would exceed 112 entries.
bool writeImage(const QString &imagePath, const QVector<Item> &items, QString *error);

/// Walk `hostPaths` (files and folders) relative to `rootDirectory` and
/// collect floppy items. Hidden names (`.git`, …) are skipped when recursing
/// into a folder, but an explicitly selected hidden file is included.
bool collectHostItems(const QString &rootDirectory, const QStringList &hostPaths,
                      QVector<Item> *items, QString *error);

} // namespace floppy

} // namespace pist
