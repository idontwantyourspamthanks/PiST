// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/EmbedSocket.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace pist {

namespace {

/// Parse a "<w>x<h>" embed-size report into width and height. Returns false
/// for anything that is not exactly two positive integers around an 'x', so
/// socket noise is never treated as a size and the container is never resized
/// to junk.
bool parseEmbedSize(const QString &line, int *width, int *height)
{
    const int x = line.indexOf(QLatin1Char('x'));
    if (x <= 0)
        return false;
    bool okWidth = false, okHeight = false;
    const int w = line.left(x).toInt(&okWidth);
    const int h = line.mid(x + 1).toInt(&okHeight);
    if (!okWidth || !okHeight || w <= 0 || h <= 0)
        return false;
    *width = w;
    *height = h;
    return true;
}

} // namespace

EmbedSocket::EmbedSocket(QObject *parent)
    : QObject(parent)
{
}

EmbedSocket::~EmbedSocket()
{
    close();
}

bool EmbedSocket::listen(const QString &path, QString *error)
{
    close();
    m_path = path;

    m_server = new QLocalServer(this);
    // A socket file left by a crashed session would otherwise fail the bind.
    QLocalServer::removeServer(path);

    if (!m_server->listen(path)) {
        if (error)
            *error = QStringLiteral("cannot listen on control socket '%1': %2")
                         .arg(path, m_server->errorString());
        delete m_server;
        m_server = nullptr;
        return false;
    }


    connect(m_server, &QLocalServer::newConnection, this, [this] {
        m_socket = m_server->nextPendingConnection();
        if (!m_socket)
            return;
        connect(m_socket, &QLocalSocket::readyRead, this, &EmbedSocket::onData);
        connect(m_socket, &QLocalSocket::disconnected, this, [this] {
            m_socket->deleteLater();
            m_socket = nullptr;
        });
        // Ask for the video size when the session embeds the display.
        if (m_requestOnConnect)
            requestEmbedSize();
        emit logLine(tr("Control socket connected."));
    });
    return true;
}

bool EmbedSocket::connected() const
{
    return m_socket != nullptr;
}

void EmbedSocket::close()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (!m_path.isEmpty()) {
        QLocalServer::removeServer(m_path);
        m_path.clear();
    }
    // A partial report from the previous session must not concatenate with
    // the next one's first (docs/code-review-glm-001.md §P2 — the guarantee
    // moves with the state).
    m_buffer.clear();
}

void EmbedSocket::writeLine(const QByteArray &line)
{
    if (m_socket)
        m_socket->write(line);
}

void EmbedSocket::onData()
{
    if (!m_socket)
        return;
    m_buffer += m_socket->readAll();

    // Hatari reports the embedded video size as "<w>x<h>" with NO terminator:
    // Control_SendEmbedSize's sprintf writes no newline (its own comment
    // claims one, but the code writes none), so this must not wait for a
    // line — doing so left every report sitting in the buffer unparsed, which
    // is why the embedded display never learned its size. Each report is a
    // separate write, spaced by a video mode change, so one report per read
    // is the rule. Parse whatever has arrived, and only clear the buffer once
    // a complete size has been consumed.
    const QString text = QString::fromUtf8(m_buffer).trimmed();
    int width = 0, height = 0;
    if (parseEmbedSize(text, &width, &height)) {
        emit logLine(tr("Emulator window size: %1x%2").arg(width).arg(height));
        emit sizeReported(width, height);
        m_buffer.clear();
    }
}

} // namespace pist
