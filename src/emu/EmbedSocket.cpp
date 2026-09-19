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

namespace {

/// Plausible ST video dimensions. Rejecting out-of-range values drops socket
/// noise and, crucially, a *partial* report that happens to parse: a split
/// "320x2" (height 2) is not a real mode, so it is left in the buffer to be
/// completed by the next read into "320x200" (finding C9).
constexpr int kMinEmbedDim = 16;
constexpr int kMaxEmbedDim = 4096;
/// A single "<w>x<h>" report is far shorter than this; exceeding it without a
/// parse means coalesced or garbage bytes, so drop the buffer rather than let it
/// grow without bound (finding C9).
constexpr int kMaxEmbedReportBytes = 64;

/// Parse a "<w>x<h>" embed-size report into width and height. Returns false
/// for anything that is not two integers around an 'x' within a plausible
/// range, so the container is never resized to junk or a truncated report.
bool parseEmbedSize(const QString &line, int *width, int *height)
{
    const int x = line.indexOf(QLatin1Char('x'));
    if (x <= 0)
        return false;
    bool okWidth = false, okHeight = false;
    const int w = line.left(x).toInt(&okWidth);
    const int h = line.mid(x + 1).toInt(&okHeight);
    if (!okWidth || !okHeight || w < kMinEmbedDim || w > kMaxEmbedDim
        || h < kMinEmbedDim || h > kMaxEmbedDim)
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
    } else if (m_buffer.size() > kMaxEmbedReportBytes) {
        // Not a single (or partial) report and too long to become one — a
        // coalesced pair like "320x200640x400" never parses, so without this the
        // buffer accumulated forever and every later report was lost with it.
        m_buffer.clear();
    }
}

} // namespace pist
