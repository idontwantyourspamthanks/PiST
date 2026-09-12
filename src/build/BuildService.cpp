// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/BuildService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace pist {

namespace {

/// `error 10 in line 2 of "bad.s": number or identifier expected`
/// The regex contains `)"`, so a custom raw-string delimiter is required.
const QRegularExpression &locatedRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"RX(^(error|warning|fatal error)\s+(\d+)\s+in line\s+(\d+)\s+of\s+"([^"]+)":\s*(.*)$)RX"));
    return re;
}

/// `error 3004: section attributes <r> not supported`
/// `fatal error 3008: output module doesn't allow multiple sections of the same type ()`
const QRegularExpression &unlocatedRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(error|warning|fatal error)\s+(\d+):\s*(.*)$)"));
    return re;
}

Diagnostic::Severity severityFrom(const QString &word)
{
    if (word.startsWith(QLatin1String("fatal")))
        return Diagnostic::Error;
    if (word == QLatin1String("warning"))
        return Diagnostic::Warning;
    return Diagnostic::Error;
}

} // namespace

BuildService::BuildService(QObject *parent)
    : QObject(parent)
{
}

BuildService::~BuildService()
{
    cancel();
}

bool BuildService::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void BuildService::cancel()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    m_process->deleteLater();
    m_process = nullptr;
}

void BuildService::build()
{
    if (isRunning())
        return;

    if (m_sourceFile.isEmpty()) {
        QList<Diagnostic> diags;
        Diagnostic d;
        d.severity = Diagnostic::Error;
        d.message = tr("No source file selected.");
        diags.append(d);
        emit finished(false, diags);
        return;
    }

    m_diagnostics.clear();
    m_awaitingExcerpt = -1;
    m_stderrBuffer.clear();
    m_stdoutBuffer.clear();

    // A stale output file from a previous successful build would be silently
    // run if this build fails, so remove it up front.
    if (!m_outputFile.isEmpty())
        QFile::remove(m_outputFile);

    QStringList argv;
    argv << m_assemblerPath
         << QStringLiteral("-quiet")
         << QStringLiteral("-Ftos");

    if (!m_cpu.isEmpty())
        argv << (QStringLiteral("-m") + m_cpu);

    for (const QString &inc : m_includePaths)
        argv << (QStringLiteral("-I") + inc);
    for (const QString &def : m_defines)
        argv << (QStringLiteral("-D") + def);

    if (!m_listingFile.isEmpty())
        argv << QStringLiteral("-L") << m_listingFile;

    argv << QStringLiteral("-o") << m_outputFile;
    argv << m_extraArgs;
    argv << m_sourceFile;

    m_lastCommand = argv;
    emit started(argv);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderrBuffer += m_process->readAllStandardError();
        int nl;
        while ((nl = m_stderrBuffer.indexOf('\n')) >= 0) {
            const QByteArray raw = m_stderrBuffer.left(nl);
            m_stderrBuffer.remove(0, nl + 1);
            handleStderrLine(QString::fromUtf8(raw).remove(QLatin1Char('\r')));
        }
    });

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_stdoutBuffer += m_process->readAllStandardOutput();
        int nl;
        while ((nl = m_stdoutBuffer.indexOf('\n')) >= 0) {
            const QByteArray raw = m_stdoutBuffer.left(nl);
            m_stdoutBuffer.remove(0, nl + 1);
            handleStdoutLine(QString::fromUtf8(raw).remove(QLatin1Char('\r')));
        }
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        Diagnostic d;
        d.severity = Diagnostic::Error;
        d.message = tr("Could not run '%1'. Is the assembler installed?")
                        .arg(m_assemblerPath);
        m_diagnostics.append(d);
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                flushPendingExcerpt();
                const bool ok = (status == QProcess::NormalExit && exitCode == 0);
                m_process->deleteLater();
                m_process = nullptr;
                emit finished(ok, m_diagnostics);
            });

    m_process->start(argv.first(), argv.mid(1));
}

void BuildService::handleStderrLine(const QString &line)
{
    // A `>` line is the source excerpt belonging to the diagnostic above it.
    if (line.startsWith(QLatin1Char('>'))) {
        if (m_awaitingExcerpt >= 0 && m_awaitingExcerpt < m_diagnostics.size()) {
            m_diagnostics[m_awaitingExcerpt].excerpt = line.mid(1).trimmed();
            m_awaitingExcerpt = -1;
        }
        emit outputLine(line);
        return;
    }

    flushPendingExcerpt();

    auto located = locatedRe().match(line);
    if (located.hasMatch()) {
        Diagnostic d;
        d.severity = severityFrom(located.captured(1));
        d.code = located.captured(2).toInt();
        d.line = located.captured(3).toInt();
        d.file = located.captured(4);
        d.message = located.captured(5).trimmed();
        m_diagnostics.append(d);
        m_awaitingExcerpt = m_diagnostics.size() - 1;
        emit outputLine(line);
        return;
    }

    // Module-level and fatal failures carry no file or line. They still need to
    // reach the Problems pane, attached to the build log rather than a source
    // location.
    auto unlocated = unlocatedRe().match(line);
    if (unlocated.hasMatch()) {
        Diagnostic d;
        d.severity = severityFrom(unlocated.captured(1));
        d.code = unlocated.captured(2).toInt();
        d.line = 0;
        d.message = unlocated.captured(3).trimmed();
        m_diagnostics.append(d);
        emit outputLine(line);
        return;
    }

    if (!line.trimmed().isEmpty())
        emit outputLine(line);
}

void BuildService::handleStdoutLine(const QString &line)
{
    // vasm prints its banner and section sizes on stdout; -quiet suppresses the
    // banner but not the sizes. Surface them in the build log.
    if (!line.trimmed().isEmpty())
        emit outputLine(line);
}

void BuildService::flushPendingExcerpt()
{
    // vasm always prints the excerpt immediately after the diagnostic, so a
    // pending index at this point means there was none (e.g. end of output).
    m_awaitingExcerpt = -1;
}

} // namespace pist
