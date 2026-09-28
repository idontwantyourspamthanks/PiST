// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/MediaServer.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QTcpServer>
#include <QRandomGenerator>
#include <QTimer>
#include <QTcpSocket>
#include <QtEndian>

namespace pist {

namespace {

/// The listener's port band. Below 32768 on purpose: the loopback
/// self-connect measured in the spike (docs/PLAN.md §12.7) needs the server
/// port to fall in the kernel's ephemeral range, so listening entirely below
/// the floor removes that class of failure. A random port inside the band,
/// because a predictable one can be pre-bound by another local process.
constexpr quint16 kPortBandFirst = 20000;
constexpr quint16 kPortBandLast = 32767;
constexpr int kPortAttempts = 10;

/// "PSA1" — the AUTH a client must open with, followed by the 16-byte token.
constexpr char kAuthMagic[4] = {'P', 'S', 'A', '1'};
constexpr qsizetype kAuthBytes = 4 + 16;
/// How long a connection gets to produce its AUTH before it is dropped.
constexpr int kAuthTimeoutMs = 3000;

/// "PSH1" — the HELLO a validated client is answered with.
constexpr char kHelloMagic[4] = {'P', 'S', 'H', '1'};
/// caps bit 0: the IDE wants video frames; bit 1: the IDE sends input.
constexpr quint32 kCapWantsVideo = 1u;
constexpr quint32 kCapSendsInput = 2u;

/// Frame header magic 0x46524D31.
constexpr quint32 kFrameMagic = 0x46524D31u;
/// magic, w, h, pitch, seq, bpp, rmask, gmask, bmask.
constexpr int kHeaderSize = 9 * 4;

/// Sanity bounds for a header we are willing to trust. A real ST/TT/Falcon
/// screen area is far inside these (640x480x4 and 1280x960x4 are the widest
/// useful cases); anything outside is a garbled magic match, not a frame we
/// should buffer a payload for.
constexpr quint32 kMaxDimension = 4096;
constexpr quint32 kMaxPitch = 65536;
constexpr qint64 kMaxPayload = 64LL * 1024 * 1024;

/// The receive buffer may never hold more than one maximal frame plus slack;
/// beyond that the stream is not a frame stream and the backlog is dropped.
constexpr qsizetype kMaxBuffer = kMaxPayload + kHeaderSize + 4096;

bool plausible(const MediaFrame &frame, qint64 payload)
{
    return frame.w >= 1 && frame.w <= kMaxDimension
        && frame.h >= 1 && frame.h <= kMaxDimension
        && frame.bpp == 32
        && frame.pitch >= frame.w * 4
        && frame.pitch <= kMaxPitch
        && payload > 0 && payload <= kMaxPayload;
}

void appendU32(QByteArray *out, quint32 value)
{
    const int at = out->size();
    out->resize(at + 4);
    qToLittleEndian<quint32>(value, reinterpret_cast<uchar *>(out->data()) + at);
}

} // namespace

MediaServer::MediaServer(QObject *parent)
    : QObject(parent)
{
    // Registered before any connection can be made, so a queued connection or
    // a QSignalSpy can carry a frame.
    qRegisterMetaType<MediaFrame>("pist::MediaFrame");
}

MediaServer::~MediaServer()
{
    close();
}

bool MediaServer::listen(QString *error)
{
    close();

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &MediaServer::onNewConnection);

    // The per-session credential: 16 random bytes the fork must open with.
    // Generated here so the caller can pass it to the fork's environment.
    m_token.resize(16);
    for (int i = 0; i < 16; i += 8)
        qToLittleEndian<quint64>(QRandomGenerator::system()->generate64(),
                                 reinterpret_cast<uchar *>(m_token.data()) + i);

    QString lastError;
    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        const quint16 port = quint16(kPortBandFirst
            + QRandomGenerator::system()->bounded(kPortBandLast - kPortBandFirst + 1));
        if (m_server->listen(QHostAddress::LocalHost, port)) {
            emit logLine(tr("media: listening on 127.0.0.1:%1").arg(port));
            return true;
        }
        lastError = m_server->errorString();
        // Only a collision could be cured by another port; any other listen
        // failure is the same on all of them.
        if (m_server->serverError() != QAbstractSocket::AddressInUseError)
            break;
    }

    m_token.clear();
    m_server->deleteLater();
    m_server = nullptr;
    if (error)
        *error = lastError.isEmpty()
            ? tr("no free port in %1-%2 after %3 attempts")
                  .arg(kPortBandFirst).arg(kPortBandLast).arg(kPortAttempts)
            : lastError;
    return false;
}

QByteArray MediaServer::token() const
{
    return isListening() ? m_token : QByteArray();
}

void MediaServer::close()
{
    dropClient();
    m_token.clear();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

void MediaServer::resetClient()
{
    dropClient();
}

bool MediaServer::isListening() const
{
    return m_server && m_server->isListening();
}

int MediaServer::port() const
{
    return isListening() ? int(m_server->serverPort()) : 0;
}

void MediaServer::dropClient()
{
    m_buffer.clear();
    m_authed = false;
    if (m_authTimer)
        m_authTimer->stop();
    if (!m_client)
        return;
    QTcpSocket *socket = m_client;
    m_client = nullptr;
    disconnect(socket, nullptr, this, nullptr);
    socket->abort();
    socket->deleteLater();
}

void MediaServer::onNewConnection()
{
    QTcpSocket *socket = m_server ? m_server->nextPendingConnection() : nullptr;
    if (!socket)
        return;

    // One fork at a time. A new connection replaces a stale one: a fork that
    // restarted while its old socket lingered must not be ignored.
    if (m_client) {
        emit logLine(tr("media: a new client replaced the previous one"));
        dropClient();
    }

    m_client = socket;
    connect(socket, &QTcpSocket::readyRead, this, &MediaServer::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (socket == m_client)
            dropClient();
    });
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                if (m_client)
                    emit logLine(tr("media: %1").arg(m_client->errorString()));
            });

    // Nothing is said until the client proves it is the fork PiST spawned:
    // AUTH within the timeout, or the connection is dropped without a word.
    if (!m_authTimer) {
        m_authTimer = new QTimer(this);
        m_authTimer->setSingleShot(true);
        connect(m_authTimer, &QTimer::timeout, this, [this] {
            if (m_client && !m_authed) {
                emit logLine(tr("media: client produced no AUTH in %1 ms; dropped")
                                 .arg(kAuthTimeoutMs));
                dropClient();
            }
        });
    }
    m_authTimer->start(kAuthTimeoutMs);
}

void MediaServer::onReadyRead()
{
    if (!m_client)
        return;
    m_buffer.append(m_client->readAll());

    if (!m_authed) {
        if (m_buffer.size() < kAuthBytes)
            return; // AUTH still arriving; the timer bounds the wait
        const bool valid = m_buffer.startsWith(kAuthMagic)
            && m_buffer.mid(4, 16) == m_token;
        if (!valid) {
            emit logLine(tr("media: client's AUTH did not match; dropped"));
            dropClient();
            return;
        }
        m_authed = true;
        m_authTimer->stop();
        m_buffer.remove(0, kAuthBytes);

        // HELLO once, now: the fork streams nothing until it arrives.
        QByteArray hello;
        hello.reserve(12);
        hello.append(kHelloMagic, 4);
        appendU32(&hello, 2); // protocol version
        appendU32(&hello, kCapWantsVideo | kCapSendsInput);
        m_client->write(hello);

        emit clientConnected();
        emit logLine(tr("media: client connected (%1)")
                         .arg(m_client->peerAddress().toString()));
    }

    if (m_buffer.size() > kMaxBuffer) {
        emit logLine(tr("media: dropping an oversized backlog (%1 bytes)").arg(m_buffer.size()));
        m_buffer.clear();
        return;
    }
    parseBuffer();
}

void MediaServer::parseBuffer()
{
    for (;;) {
        if (m_buffer.size() < 4)
            return;

        const qsizetype start = [this] {
            const int last = m_buffer.size() - 4;
            for (int i = 0; i <= last; ++i) {
                if (qFromLittleEndian<quint32>(
                        reinterpret_cast<const uchar *>(m_buffer.constData()) + i)
                    == kFrameMagic)
                    return qsizetype(i);
            }
            return qsizetype(-1);
        }();

        if (start < 0) {
            // No frame can begin here. Keep the last three bytes: a magic split
            // across two reads must still be recognised on the next append.
            if (m_buffer.size() > 3)
                m_buffer.remove(0, m_buffer.size() - 3);
            return;
        }
        if (start > 0)
            m_buffer.remove(0, start);

        if (m_buffer.size() < kHeaderSize)
            return; // header still arriving

        const uchar *p = reinterpret_cast<const uchar *>(m_buffer.constData());
        MediaFrame frame;
        frame.w = qFromLittleEndian<quint32>(p + 4);
        frame.h = qFromLittleEndian<quint32>(p + 8);
        frame.pitch = qFromLittleEndian<quint32>(p + 12);
        frame.seq = qFromLittleEndian<quint32>(p + 16);
        frame.bpp = qFromLittleEndian<quint32>(p + 20);
        frame.rmask = qFromLittleEndian<quint32>(p + 24);
        frame.gmask = qFromLittleEndian<quint32>(p + 28);
        frame.bmask = qFromLittleEndian<quint32>(p + 32);

        const qint64 payload = qint64(frame.pitch) * frame.h;
        if (!plausible(frame, payload)) {
            // Garbage that happened to carry the magic: step past its first
            // byte and look for the next one, so a valid frame that follows is
            // not skipped.
            m_buffer.remove(0, 1);
            continue;
        }

        if (m_buffer.size() < kHeaderSize + payload)
            return; // hold the partial frame; a partial is never emitted

        frame.pixels = m_buffer.mid(kHeaderSize, int(payload));
        m_buffer.remove(0, kHeaderSize + int(payload));
        emit frameReceived(frame);
    }
}

} // namespace pist
