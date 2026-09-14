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
/// carries control commands and the embedded-video size report — never
/// debugger commands (§3.3).
///
/// Used by EmulatorHost (embed size + `hatari-debug` lines for pause) and by
/// HrdbBackend (embed size; the fork keeps the upstream control socket, and
/// without this the embedded display never learns its size).
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

    /// When true, each new emulator connection is asked for its video size
    /// immediately (the embedded-display case; the request is pointless when
    /// the display is separate, so it is opt-in).
    void setRequestOnConnect(bool on) { m_requestOnConnect = on; }

    /// A line to the emulator: `hatari-embed-info` asks for the video size,
    /// `hatari-debug …` injects a debugger command (native pause). Writes are
    /// dropped silently when no emulator is connected yet.
    void writeLine(const QByteArray &line);

    /// Ask the emulator to report its video size. Gated on nothing — callers
    /// decide whether the session embeds.
    void requestEmbedSize() { writeLine("hatari-embed-info\n"); }

signals:
    /// A "WxH" video-size report arrived and parsed.
    void sizeReported(int width, int height);
    void logLine(const QString &line);

private:
    void onData();

    QString m_path;

    bool m_requestOnConnect = false;
    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;
    QByteArray m_buffer;
};

} // namespace pist
