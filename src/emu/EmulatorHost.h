// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"
#include "emu/SessionConfig.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;
class QProcess;
class QTimer;

namespace pist {

class HatariCapabilities;

/// Owns one Hatari process and the debug channels attached to it.
///
/// Channel model (verified against Hatari 2.6.1; docs/PLAN.md §3.3). The two channels
/// are not interchangeable:
///
///   stdin   the *only* channel that works while the debugger is stopped. The
///           debugger blocks in DebugUI_GetCommand reading stdin, and nothing
///           under src/debug/ polls the control socket, so `hatari-debug`
///           commands sent while stopped are never read.
///   socket  serviced only from the SDL event pump (src/sdl/gui_event.c:134),
///           so it works only while emulation is running.
///
/// Completion framing: readline echoes the command and then writes the next
/// prompt to **stdout**, which is a true end-of-command signal because the loop
/// cannot print a prompt until it has finished executing. Command output itself
/// arrives on **stderr**.
class EmulatorHost : public QObject
{
    Q_OBJECT

public:
    explicit EmulatorHost(QObject *parent = nullptr);
    ~EmulatorHost() override;

    void setCapabilities(const HatariCapabilities &caps) { m_caps = &caps; }

    bool start(const SessionConfig &config, QString *error);
    void stop();

    bool isRunning() const;
    bool isStopped() const { return m_stopped; }
    SessionConfig config() const { return m_config; }

    /// Queue a debugger command. Delivered over stdin, so it works while the
    /// debugger is stopped. A `commandFinished` signal follows.
    void command(const QString &command);

    /// Send a Hatari control command over the socket. Only serviced while
    /// emulation is *running*; while stopped this silently does nothing.
    bool control(const QString &hatariCommand);

    void step();     // `s`
    void stepOver(); // `n`
    void resume();   // `c`

    /// Queue the commands needed to render a full state snapshot.
    void refresh();

    static QString writeBootstrapScript(const QString &directory,
                                        const HatariCapabilities &caps,
                                        QString *error);

signals:
    void runningChanged(bool running);
    void stoppedChanged(bool stopped);
    void commandFinished(const QString &command, const QString &response);
    void stateUpdated(const pist::MachineState &state);
    void logLine(const QString &line);
    void errorOccurred(const QString &message);

private:
    struct Pending
    {
        QString text;
        QString response;
        QByteArray raw;
    };

    void dispatchNext();
    void completeCurrent();
    void onPrompt();
    void onCommandTimeout();
    void handleStderrLine(const QString &line);
    void handleStdoutData(const QByteArray &data);
    void handleSocketData();
    void parseRegisters(const QString &response);
    void parseBasepage(const QString &response);
    void parseDisassembly(const QString &response);
    void openSocketServer(QString *error);
    void closeSocketServer();

    SessionConfig m_config;
    const HatariCapabilities *m_caps = nullptr;

    QProcess *m_process = nullptr;
    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;

    QByteArray m_stderrBuffer;
    QByteArray m_socketBuffer;

    QQueue<Pending> m_queue;
    bool m_haveCurrent = false;
    Pending m_current;
    QTimer *m_commandTimeout = nullptr;

    /// Prompt detection. The debugger prompt is `> ` written before each
    /// blocking read; which stream carries it depends on the build (stdout with
    /// readline, stderr without), so both are counted.
    QString m_stdoutText;
    int m_stdoutLoggedChars = 0;
    quint64 m_promptCount = 0;
    quint64 m_promptTarget = 0;
    bool m_promptStreamIsStdout = false;

    /// Set when the debugger has announced entry but its session dump is still
    /// being written; cleared once it reaches its prompt.
    bool m_awaitingEntryPrompt = false;

    /// Prompts still owed by commands that timed out. Their arrival must be
    /// swallowed so they cannot complete a later command.
    int m_owedPrompts = 0;

    MachineState m_state;
    bool m_stopped = false;
};

} // namespace pist
