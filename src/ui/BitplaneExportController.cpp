// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/BitplaneExportController.h"

#include "support/FileWrite.h"

#include <QFileInfo>

namespace pist {

BitplaneExportController::BitplaneExportController(const ImageDocument &doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
{
}

bool BitplaneExportController::writeBytes(const QString &path, const QByteArray &bytes)
{
    // Through the shared rule: an export replaces a file the user may already
    // have, and a write that cannot reach the disk must leave that file alone
    // rather than a truncated one.
    QString error;
    if (!files::write(path, bytes, &error)) {
        m_lastError = error;
        return false;
    }
    return true;
}

bool BitplaneExportController::exportBytes(const QString &path, int phase,
                                           const BitplaneDataOptions &options,
                                           const QString &scroller)
{
    // Encode both before opening either: an encoder that refuses (a phase that
    // is gone, no blocks selected) must not leave a truncated `.dat` where a
    // good one was. The `.dat` is the product and goes first; the scroller is
    // its companion, so a scroller that will not open is reported against a
    // `.dat` that is already out — the same report the explicit export makes.
    QString error;
    const QByteArray data = exportBitplaneData(m_doc, phase, options, &error);
    if (data.isEmpty()) {
        m_lastError = error.isEmpty() ? tr("Export failed") : error;
        return false;
    }
    QByteArray demo;
    if (!scroller.isEmpty()) {
        demo = exportScrollDemo(m_doc, phase, options, QFileInfo(path).fileName(), &error);
        if (demo.isEmpty()) {
            m_lastError = error.isEmpty() ? tr("Export failed") : error;
            return false;
        }
    }
    if (!writeBytes(path, data))
        return false;
    if (scroller.isEmpty())
        return true;
    if (!writeBytes(scroller, demo)) {
        m_lastError = tr("wrote %1, but could not write the scroller %2: %3")
                          .arg(QFileInfo(path).fileName(), QFileInfo(scroller).fileName(),
                               m_lastError);
        return false;
    }
    return true;
}

void BitplaneExportController::setScroller(const QString &scroller)
{
    if (m_recipe.path.isEmpty())
        return;
    m_recipe.scroller = scroller;
    emit recipeChanged();
}

void BitplaneExportController::forget()
{
    m_recipe = Recipe{};
    emit recipeChanged();
}

bool BitplaneExportController::exportBitplane(const QString &path, int phase,
                                              const BitplaneDataOptions &options)
{
    if (!exportBytes(path, phase, options, QString()))
        return false;
    // The choices are not in the `.pim` and not in the project: export stays
    // explicit, so how to repeat it lives here, for this document, this
    // session. A fresh export re-seeds it; the scroller comes in behind via
    // setScroller().
    m_recipe = Recipe{path, phase, options, QString()};
    emit recipeChanged();
    return true;
}

bool BitplaneExportController::writeScrollDemo(const QString &path, int phase,
                                               const BitplaneDataOptions &options,
                                               const QString &dataFile)
{
    QString error;
    const QByteArray bytes = exportScrollDemo(m_doc, phase, options, dataFile, &error);
    if (bytes.isEmpty()) {
        m_lastError = error.isEmpty() ? tr("Export failed") : error;
        return false;
    }
    return writeBytes(path, bytes);
}

bool BitplaneExportController::reExport()
{
    const QString path = m_recipe.path;
    const QString scroller = m_recipe.scroller;
    if (m_recipe.path.isEmpty()) {
        m_lastError = tr("No bitplane export to repeat yet — export one first.");
        emit bitplaneReExported(path, scroller, m_lastError);
        return false;
    }
    // Overwrite, no dialog and no prompt: the remembered export is the user
    // saying what this file is, and the action is only enabled after one.
    const bool ok = exportBytes(m_recipe.path, m_recipe.phase, m_recipe.options,
                                m_recipe.scroller);
    emit bitplaneReExported(path, scroller, ok ? QString() : m_lastError);
    return ok;
}

} // namespace pist
