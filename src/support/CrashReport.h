// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QString>

namespace pist {
namespace crash {

/// QSettings key for the opt-in. Spelled once so the dialog and the uploader
/// cannot drift (the MIN-53 pattern).
inline constexpr auto kSettingsKey = "crashReports";

/// Where a report is posted. The desk at pist.kswapps.com reads the same store.
inline constexpr auto kEndpoint = "https://pistapi.kswapps.com/api/crash-reports";

bool reportsEnabled();
void setReportsEnabled(bool on);

/// Remember a session phase. The text is a short label ("build", "run"), never
/// file contents: the receiver drops anything that is not on its allowlist, and
/// this side does not put source on the list in the first place.
void note(const char *phase);
void clearNotes();

/// Park a report left by the previous process and arm the handler for this one.
/// Call once, after the application name is set, so the cache path is stable.
void arm();

/// POST the parked report when reports are enabled. A network failure leaves
/// the file, so the next launch tries again.
void uploadPending();

/// The on-disk record the handler writes, and the JSON body the endpoint accepts.
/// Exposed so a test can pin both without raising a signal.
QByteArray formatRecord(const QString &signalName, const QByteArray &stack);
bool parseRecord(const QByteArray &record, QByteArray *json, QString *error);

} // namespace crash
} // namespace pist
