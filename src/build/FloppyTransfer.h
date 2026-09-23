// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/FloppyImage.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace pist {
namespace floppy {

/// The three disk-image transfers the file browser offers: host files onto a
/// disk, entries off a disk, and entries between two disks.
///
/// All three are one algorithm over different source and destination pairs —
/// walk the chosen subtrees, sequence load → list → read → update, and ask
/// before an image is rebuilt. Written out once per direction inside
/// FileBrowser they drifted: the three bodies carried three copies of the
/// confirm/resync preamble, two subtree walks and three orderings of the same
/// steps, and a path-rebase bug lived in exactly one of them (finding MAJ-43).
/// The sequencing therefore lives here once, and the browser keeps the dialogs,
/// the pane refresh and its clipboard.
///
/// The controller is deliberately widget-free: nothing here shows a message,
/// and the two things a transfer cannot answer for itself — which image a drive
/// holds, and whether rebuilding one is acceptable — come in through `Host`. A
/// `Result` names the step that failed and the disk, host file or entry it was
/// about, so the caller phrases the message in its own translation unit.
class Transfer
{
public:
    /// The browser side of a transfer.
    class Host
    {
    public:
        virtual ~Host() = default;

        /// The image mounted on `drive` (0 = A:, 1 = B:), empty when the drive
        /// is empty.
        virtual QString mountedImage(int drive) const = 0;

        /// Ask whether `imagePath` may be rebuilt in place; false when the user
        /// declines. The implementation runs a dialog, so this returns after an
        /// arbitrary amount of user interaction — which is why the drives are
        /// re-read once every question has been answered.
        virtual bool confirmRewrite(const QString &imagePath) = 0;
    };

    /// How a transfer ended. `Completed` is the only success. `Declined`,
    /// `Resynced` and `Invalid` are silent — the user said no, a drive moved on
    /// while a question was up, or the call asked for nothing — and each other
    /// outcome is one distinct message the caller shows.
    enum Outcome {
        Completed,               ///< the disk or the disks were written
        Declined,                ///< the user refused a rewrite
        Resynced,                ///< a drive changed while a question was up
        Invalid,                 ///< the call's own preconditions failed
        Unreadable,              ///< `subject` could not be read; `error` says why
        CopyFailed,              ///< `names` could not be copied out of `subject`
        AddFailed,               ///< the files could not be added to `subject`
        WriteFailed,             ///< `subject` could not be written
        SourceRemovalFailed,     ///< `subject` kept entries the copy took off it
        DestinationInsideSource, ///< `subject` cannot be put inside itself
        NothingToCopy,           ///< the chosen entries matched nothing on the disk
    };

    /// Why an entry of a copied set could not be written to the host. The
    /// controller reports the cause rather than a sentence, so this wording
    /// stays in the caller — a `floppy::` error string would not be translated.
    enum Cause {
        NoCause,      ///< not about one entry; `error` carries the reason
        EntryMissing, ///< the disk has no entry named by `entry`
        EntryEscapes, ///< `entry` would land outside the destination folder
    };

    struct Result {
        Outcome outcome = Invalid;

        /// The disk, host file or entry the outcome is about, as a path: the
        /// caller names it (`QFileInfo::fileName`) or shows it whole
        /// (`DestinationInsideSource`, whose subject is an entry in the image).
        QString subject;

        /// What the floppy writer or the host file system reported. Empty when
        /// the caller phrases the failure itself — see `cause`.
        QString error;

        /// The entries a failed copy could not handle, as its message lists
        /// them.
        QStringList names;

        /// Why the entry that failed last could not be copied out.
        Cause cause = NoCause;

        /// The entry `cause` is about: its own name for `EntryMissing`
        /// (`PROG.PRG` for `AUTO/PROG.PRG`), its path inside the image for
        /// `EntryEscapes`.
        QString entry;

        /// The drives to re-read. A failed move still has a half-done state to
        /// show — the entries that did come out are gone from the source disk.
        QVector<int> refresh;

        bool ok() const { return outcome == Completed; }
    };

    explicit Transfer(Host *host);

    /// Copy `hostPaths` into the image directory `dirInImage` of `drive` (empty
    /// for the image root). Each host file enters under its own name, and a
    /// folder keeps its shape below it.
    ///
    /// The caller removes the host paths itself when the transfer is a move: it
    /// is what tells the rest of the application a path is gone.
    Result hostToImage(int drive, const QString &dirInImage, const QStringList &hostPaths);

    /// Copy `entryPaths` — image-relative, as `listRaw` reports them — out of
    /// `drive` into the host directory `targetDir`. A chosen folder is created
    /// in `targetDir` under its own name and its contents follow inside it;
    /// nothing already in `targetDir` is overwritten. When `removeSource`, the
    /// entries are also dropped from the image (a move).
    Result imageToHost(int drive, const QStringList &entryPaths, const QString &targetDir,
                       bool removeSource);

    /// Copy `entryPaths` off `sourceDrive` onto `targetDrive`, into the image
    /// directory `dirInImage` (empty for the image root) — the source and the
    /// target may be the same image, which is what copying inside one disk is.
    Result imageToImage(int sourceDrive, const QStringList &entryPaths, int targetDrive,
                        const QString &dirInImage, bool removeSource);

    /// A free path in `dir` for `name`, suffixed " (2)", " (3)"… when it is
    /// taken. The destination-naming rule every copy the browser makes follows,
    /// so a paste never silently overwrites; shared with the hard-drive
    /// transfer, which parses the same host-side collisions.
    static QString uniqueHostDestination(const QString &dir, const QString &name);

private:
    /// One disk a transfer is about to rebuild, and the drive it sits in.
    struct Target {
        int drive;
        QString image;
    };

    /// The confirm/resync preamble every transfer runs before it writes: ask
    /// about each disk in turn, then re-read every drive. A dialog runs an
    /// event loop, so a drive can be changed, ejected or refilled while a
    /// question is up — a transfer writes the disks that were asked about, or
    /// none. Returns false with `outcome` = `Declined` for the user's answer and
    /// `Resynced` for a drive that moved on.
    bool confirmTargets(const QVector<Target> &targets, Outcome *outcome);

    /// A `Result` carrying nothing but the refusal above.
    static Result refusal(Outcome outcome);

    Host *m_host = nullptr;
};

} // namespace floppy
} // namespace pist
