// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "support/FileWrite.h"

#include <QFileDevice>
#include <QObject>
#include <QSaveFile>

namespace pist {
namespace files {

bool write(const QString &path, const QByteArray &bytes, QString *error)
{
    const auto fail = [path, error](const QString &reason) {
        if (error)
            *error = QObject::tr("cannot write '%1': %2").arg(path, reason);
        return false;
    };

    // QSaveFile for the temporary file and the atomic replace, which is what it
    // is for — but not for the error detection, which is the part Qt 6.8.1 gets
    // wrong (see the header).
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(file.errorString());

    bool onDisk = file.write(bytes) == bytes.size();
    // write() only fills a buffer, so the flush is what surfaces a write that
    // cannot reach the disk; checking the device error catches one that the
    // flush itself reported only by return value.
    if (onDisk && !file.flush())
        onDisk = false;
    if (!onDisk || file.error() != QFileDevice::NoError) {
        const QString reason = file.errorString();
        // Cancelling is what makes the commit discard the temporary file
        // instead of renaming it over the destination.
        file.cancelWriting();
        file.commit();
        return fail(reason);
    }
    if (!file.commit())
        return fail(file.errorString());
    return true;
}

} // namespace files
} // namespace pist
