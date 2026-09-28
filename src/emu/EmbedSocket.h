// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace pist {

/// The Hatari control-socket server side, shared by both debug backends.
///
/// Hatari is the *client*: it calls connect() and never binds, so the IDE
/// listens before the process starts (docs/PLAN.md §5 rule 10). The socket is
/// serviced only from Hatari's SDL event pump while emulation runs, so it
/// carries control commands — never debugger commands (§3.3).
///
/// Used by EmulatorHost (`hatari-debug` lines for pause) and HrdbBackend.
/// The name predates the media cutover: the socket also carried the embedded
/// display's video-size reports, which the media channel's frame headers made
/// obsolete.
class EmbedSocket : public QObject
{
    Q_OBJECT

public:
    explicit EmbedSocket(QObject *parent = nullptr);
    ~EmbedSocket() override;

    /// Listen on path. Must be called before the emulator process starts.
    /// Errors are reported; a failure leaves the socket closed.
    bool listen(const QString &path, QString *error);
    void close();

    /// Whether an emulator is currently connected.
    bool connected() const;

    /// A line to the emulator: `hatari-debug …` injects a debugger command
    /// (native pause). Writes are dropped silently when no emulator is
    /// connected yet.
    void writeLine(const QByteArray &line);

signals:
    void logLine(const QString &line);

private:
    QString m_path;

    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;
};

/// Debugger `setopt` line that inserts or ejects a floppy. `path` must already
/// be safe for Hatari's space-splitting tokenizer (`floppyImageForDebugger`);
/// `none` ejects. Used while the debugger is stopped (stdin / HRDB), because
/// the control socket is not serviced then (docs/PLAN.md §3.3).
QString floppySetoptCommand(int drive, const QString &path);

/// A path `setopt` can tokenize. Hatari's debugger splits on spaces and
/// treats quotes as expressions, so a live image whose host path contains
/// whitespace is staged as a symlink (or copy) under `sessionDir`.
QString floppyImageForDebugger(const QString &sessionDir, int drive, const QString &path);

} // namespace pist
