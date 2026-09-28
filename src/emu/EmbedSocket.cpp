// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/EmbedSocket.h"

#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QtGlobal>

namespace pist {


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
        connect(m_socket, &QLocalSocket::disconnected, this, [this] {
            m_socket->deleteLater();
            m_socket = nullptr;
        });
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
}

void EmbedSocket::writeLine(const QByteArray &line)
{
    if (!m_socket)
        return;
    m_socket->write(line);
    // Hatari only reads the socket from the SDL pump; an unflushed write can
    // sit in Qt's buffer until a later event, which is too late for a live
    // disk swap the user expects immediately.
    m_socket->flush();
}

QString floppySetoptCommand(int drive, const QString &path)
{
    if (drive < 0 || drive > 1)
        return {};
    const char *opt = drive == 0 ? "--disk-a" : "--disk-b";
    const QString arg = path.isEmpty() ? QStringLiteral("none") : path;
    return QStringLiteral("setopt %1 %2").arg(QLatin1String(opt), arg);
}

QString floppyImageForDebugger(const QString &sessionDir, int drive, const QString &path)
{
    if (path.isEmpty())
        return QStringLiteral("none");
    bool needsStage = false;
    for (const QChar c : path) {
        if (c.isSpace()) {
            needsStage = true;
            break;
        }
    }
    if (!needsStage || sessionDir.isEmpty() || drive < 0 || drive > 1)
        return path;

    // DebugUI_ParseCommand tokenizes with strtok(" \t") and evaluates quoted
    // spans as expressions, so a host path with spaces cannot be passed as-is.
    // A symlink in the (space-free) session dir keeps writes on the original.
    const QString staged = QDir(sessionDir).filePath(drive == 0 ? QStringLiteral("disk-a.st")
                                                                : QStringLiteral("disk-b.st"));
    QFile::remove(staged);
#ifndef Q_OS_WIN
    if (QFile::link(path, staged))
        return staged;
#endif
    if (QFile::copy(path, staged))
        return staged;
    return path;
}


} // namespace pist
