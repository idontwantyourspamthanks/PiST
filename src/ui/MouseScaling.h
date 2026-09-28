// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QPoint>

namespace pist {

/// Host-pixel → guest-pixel mouse deltas at a fixed scale, carrying the
/// sub-pixel remainder across events (docs/PLAN.md §12.3): the panel paints
/// the frame aspect-fit, so one host pixel is rarely one guest pixel, and
/// without the accumulator a fractional scale makes the guest cursor outrun
/// or lag the hand on every move.
struct MouseScaler
{
    /// Guest pixels per host pixel (frame width / fitted width).
    double scale = 1.0;

    void setScale(double s) { scale = s > 0.0 ? s : 1.0; }
    void reset() { m_accX = m_accY = 0.0; }

    /// Accumulate one host delta and return the whole guest pixels it yields
    /// (possibly zero); the fraction stays for the next call.
    QPoint mapDelta(const QPoint &hostDelta)
    {
        m_accX += hostDelta.x() * scale;
        m_accY += hostDelta.y() * scale;
        const int gx = int(m_accX);
        const int gy = int(m_accY);
        m_accX -= gx;
        m_accY -= gy;
        return QPoint(gx, gy);
    }

private:
    double m_accX = 0.0;
    double m_accY = 0.0;
};

} // namespace pist
