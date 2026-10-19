// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "support/CrashReport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include <atomic>
#include <cstring>

#if defined(Q_OS_UNIX)
#  include <execinfo.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#endif

namespace pist {
namespace crash {

namespace {

constexpr int kMaxCrumbs = 40;
constexpr int kCrumbLen = 80;
constexpr int kSnapBytes = 8192;
constexpr int kStackBytes = 47000;

struct Snap {
    char data[kSnapBytes];
    int len = 0;
};

QStringList g_crumbs;
Snap g_snaps[2];
std::atomic<int> g_published{0};
int g_fd = -1;
QString g_pendingPath;

QString cacheDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    return dir;
}

QString reportPath()
{
    return cacheDir() + QStringLiteral("/crash-report.txt");
}

QString clip(QString text, int limit)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text = text.trimmed();
    if (text.size() > limit)
        text.truncate(limit);
    return text;
}

QString packageKind()
{
    if (!qEnvironmentVariable("APPIMAGE").isEmpty())
        return QStringLiteral("appimage");
#if defined(Q_OS_MAC)
    return QStringLiteral("dmg");
#elif defined(Q_OS_WIN)
    return QStringLiteral("msi");
#else
    return QStringLiteral("desktop");
#endif
}

void publishSnapshot()
{
    QStringList lines;
    lines << QStringLiteral("pist-crash 1");
    lines << QStringLiteral("version %1").arg(clip(QStringLiteral(PIST_VERSION), 32));
    lines << QStringLiteral("os %1").arg(clip(QSysInfo::kernelType(), 32));
    lines << QStringLiteral("arch %1").arg(clip(QSysInfo::currentCpuArchitecture(), 32));
    lines << QStringLiteral("package %1").arg(packageKind());
    lines << QStringLiteral("qt %1").arg(clip(QStringLiteral(QT_VERSION_STR), 32));
    lines << QStringLiteral("kind ide");
    for (const QString &crumb : g_crumbs)
        lines << QStringLiteral("crumb %1").arg(crumb);

    const QByteArray bytes = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    const int next = 1 - g_published.load(std::memory_order_relaxed);
    Snap &slot = g_snaps[next];
    const int n = qMin(bytes.size(), kSnapBytes);
    if (n > 0)
        memcpy(slot.data, bytes.constData(), static_cast<size_t>(n));
    slot.len = n;
    g_published.store(next, std::memory_order_release);
}

#if defined(Q_OS_UNIX)
const char *signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    default:      return "SIG";
    }
}

void writeAll(int fd, const void *data, size_t n)
{
    const char *bytes = static_cast<const char *>(data);
    while (n > 0) {
        const ssize_t wrote = ::write(fd, bytes, n);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        bytes += wrote;
        n -= static_cast<size_t>(wrote);
    }
}

void handleSignal(int sig)
{
    if (g_fd < 0)
        return;
    const int slot = g_published.load(std::memory_order_acquire);
    const Snap &snap = g_snaps[slot];
    if (snap.len > 0)
        writeAll(g_fd, snap.data, static_cast<size_t>(snap.len));
    const char *name = signalName(sig);
    writeAll(g_fd, "signal ", 7);
    writeAll(g_fd, name, strlen(name));
    writeAll(g_fd, "\n---\n", 5);
    void *frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, g_fd);
    ::fsync(g_fd);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
#endif

} // namespace

bool reportsEnabled()
{
    return QSettings().value(QLatin1String(kSettingsKey), false).toBool();
}

void setReportsEnabled(bool on)
{
    QSettings().setValue(QLatin1String(kSettingsKey), on);
}

void clearNotes()
{
    g_crumbs.clear();
    publishSnapshot();
}

void note(const char *phase)
{
    if (!phase || !phase[0])
        return;
    const QString crumb = clip(QString::fromUtf8(phase), kCrumbLen);
    if (crumb.isEmpty())
        return;
    g_crumbs.append(crumb);
    while (g_crumbs.size() > kMaxCrumbs)
        g_crumbs.removeFirst();
    publishSnapshot();
}

QByteArray formatRecord(const QString &signalName, const QByteArray &stack)
{
    publishSnapshot();
    const int slot = g_published.load(std::memory_order_acquire);
    const Snap &snap = g_snaps[slot];
    QByteArray out(snap.data, snap.len);
    if (!signalName.isEmpty()) {
        out += "signal ";
        out += signalName.toUtf8();
        out += '\n';
    }
    out += "---\n";
    out += stack;
    if (!stack.endsWith('\n'))
        out += '\n';
    return out;
}

bool parseRecord(const QByteArray &record, QByteArray *json, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    const int marker = record.indexOf("\n---\n");
    if (!record.startsWith("pist-crash 1\n") || marker < 0)
        return fail(QStringLiteral("not a crash record"));

    QJsonObject body;
    QJsonArray crumbs;
    const QList<QByteArray> lines = record.left(marker).split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i);
        if (line.isEmpty())
            continue;
        const int space = line.indexOf(' ');
        if (space <= 0)
            return fail(QStringLiteral("bad record line"));
        const QByteArray key = line.left(space);
        const QString value = clip(QString::fromUtf8(line.mid(space + 1)), 80);
        if (value.isEmpty())
            continue;
        if (key == "crumb") {
            if (crumbs.size() < kMaxCrumbs)
                crumbs.append(value);
        } else if (key == "version") {
            body.insert(QStringLiteral("version"), clip(value, 32));
        } else if (key == "os") {
            body.insert(QStringLiteral("os"), clip(value, 32));
        } else if (key == "arch") {
            body.insert(QStringLiteral("arch"), clip(value, 32));
        } else if (key == "package") {
            body.insert(QStringLiteral("package"), clip(value, 32));
        } else if (key == "qt") {
            body.insert(QStringLiteral("qt"), clip(value, 32));
        } else if (key == "kind") {
            if (value == QLatin1String("ide") || value == QLatin1String("hatari"))
                body.insert(QStringLiteral("kind"), value);
        } else if (key == "signal") {
            body.insert(QStringLiteral("signal"), clip(value, 32));
        } else if (key == "hatari") {
            body.insert(QStringLiteral("hatari_version"), clip(value, 32));
        } else if (key == "command") {
            body.insert(QStringLiteral("last_command"), value);
        } else if (key == "build") {
            body.insert(QStringLiteral("build_id"), clip(value, 64));
        }
    }

    QString stack = QString::fromUtf8(record.mid(marker + 5)).trimmed();
    if (stack.size() > kStackBytes)
        stack.truncate(kStackBytes);
    if (!body.contains(QStringLiteral("version")) || stack.isEmpty())
        return fail(QStringLiteral("record has no version or stack"));

    body.insert(QStringLiteral("stack"), stack);
    if (!crumbs.isEmpty())
        body.insert(QStringLiteral("breadcrumbs"), crumbs);
    if (json)
        *json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return true;
}

void arm()
{
    const QString path = reportPath();
    QFile existing(path);
    if (existing.exists() && existing.size() > 0) {
        g_pendingPath = cacheDir() + QStringLiteral("/crash-report.pending");
        QFile::remove(g_pendingPath);
        if (!QFile::rename(path, g_pendingPath))
            g_pendingPath.clear();
    }
    if (g_pendingPath.isEmpty()) {
        // A record parked by an earlier launch that has not been posted yet: a
        // dropped connection, or reports switched off since. Without this the
        // file is never looked at again — uploadPending() only knows the path
        // it was handed here — so the "the next launch tries again" promise in
        // the header would hold only when *this* launch also crashed.
        const QString parked = cacheDir() + QStringLiteral("/crash-report.pending");
        if (QFileInfo::exists(parked))
            g_pendingPath = parked;
    }

    publishSnapshot();

#if defined(Q_OS_UNIX)
    // backtrace() loads the unwinder and allocates on its first call, which is
    // not async-signal-safe. Doing it here, in ordinary context, is what makes
    // the handler's own call safe later.
    {
        void *frames[1];
        (void)::backtrace(frames, 1);
    }
    g_fd = ::open(QFile::encodeName(path).constData(),
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        sigaction(sig, &action, nullptr);
#else
    Q_UNUSED(path);
    Q_UNUSED(g_fd);
#endif
}

void uploadPending()
{
    if (!reportsEnabled() || g_pendingPath.isEmpty())
        return;

    QFile file(g_pendingPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray record = file.readAll();
    file.close();

    QByteArray json;
    QString error;
    if (!parseRecord(record, &json, &error)) {
        QFile::remove(g_pendingPath);
        g_pendingPath.clear();
        return;
    }

    const QString path = g_pendingPath;
    auto *nam = new QNetworkAccessManager(qApp);
    QNetworkRequest request(QUrl(QString::fromUtf8(kEndpoint)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = nam->post(request, json);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, nam, path] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // 200 is stored. 4xx will never become valid, so keeping the file only
        // resends a reject. A dropped connection leaves it for the next launch.
        if (status == 200 || (status >= 400 && status < 500 && status != 429))
            QFile::remove(path);
        reply->deleteLater();
        nam->deleteLater();
    });
}

} // namespace crash
} // namespace pist
