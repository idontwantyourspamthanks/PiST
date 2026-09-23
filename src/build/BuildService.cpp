// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/BuildService.h"

#include "build/ProcessUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
    // vasm writes `error`/`warning`/`fatal error` and vlink writes
    // `Error`/`Warning`/`Fatal error`; only the warning case changes severity.
    return word.compare(QLatin1String("warning"), Qt::CaseInsensitive) == 0
               ? Diagnostic::Warning
               : Diagnostic::Error;
}

} // namespace

bool parseLinkerDiagnostic(const QString &line, Diagnostic *diagnostic)
{
    // `main.o (CODE+0x4): Reference to undefined symbol helper.`
    //
    // The linker names a module and a section offset rather than a source line,
    // so the offset is recorded and turned into a line later, once the listings
    // are known. That is what lets a link error point at the offending line.
    //
    // The severity word is *captured*: matching it only to discard it made
    // `captured(1)` the numeric code, so every located warning was recorded as an
    // error (docs/code-review-glm-001.md, P3).
    static const QRegularExpression locatedRe(QStringLiteral(
        R"(^(Fatal error|Error|Warning) (\d+):\s*(?!\s)(?:([^\s:]+)\s+)?\((\w+)\+0x([0-9A-Fa-f]+)\):\s*(.*)$)"));
    static const QRegularExpression plainRe(QStringLiteral(
        R"(^(Fatal error|Error|Warning) (\d+):\s*(.*)$)"));

    const auto located = locatedRe.match(line);
    if (located.hasMatch() && !located.captured(3).isEmpty()) {
        diagnostic->severity = severityFrom(located.captured(1));
        diagnostic->code = located.captured(2).toInt();
        diagnostic->objectFile = located.captured(3);
        diagnostic->section = located.captured(4);
        diagnostic->sectionOffset = located.captured(5).toUInt(nullptr, 16);
        diagnostic->hasObjectOffset = true;
        diagnostic->message = located.captured(6).trimmed();
        return true;
    }

    const auto plain = plainRe.match(line);
    if (plain.hasMatch()) {
        diagnostic->severity = severityFrom(plain.captured(1));
        diagnostic->code = plain.captured(2).toInt();
        diagnostic->message = plain.captured(3).trimmed();
        return true;
    }

    return false;
}

bool parseVasmDiagnostic(const QString &line, Diagnostic *diagnostic)
{
    auto located = locatedRe().match(line);
    if (located.hasMatch()) {
        diagnostic->severity = severityFrom(located.captured(1));
        diagnostic->code = located.captured(2).toInt();
        diagnostic->line = located.captured(3).toInt();
        diagnostic->file = located.captured(4);
        diagnostic->message = located.captured(5).trimmed();
        return true;
    }

    // Module-level and fatal failures carry no file or line. They still need to
    // reach the Problems pane, attached to the build log rather than a source
    // location.
    auto unlocated = unlocatedRe().match(line);
    if (!unlocated.hasMatch())
        return false;
    diagnostic->severity = severityFrom(unlocated.captured(1));
    diagnostic->code = unlocated.captured(2).toInt();
    diagnostic->line = 0;
    diagnostic->message = unlocated.captured(3).trimmed();
    return true;
}

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
    killAndRelease(m_process, this);
    m_process->deleteLater();
    m_process = nullptr;
}

bool BuildService::usesLinker() const
{
    return !m_additionalSources.isEmpty();
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
    m_stderrBuffer.clear();
    m_stdoutBuffer.clear();
    m_steps.clear();
    m_nextStep = 0;
    m_sawFailure = false;
    m_listingFiles.clear();
    m_objectFiles.clear();

    // A stale output from a previous successful build would be silently run if
    // this build fails, so remove it up front.
    if (!m_outputFile.isEmpty())
        QFile::remove(m_outputFile);

    QString planError;
    planSteps(&planError);

    if (!m_steps.isEmpty()) {
        runNextStep();
        return;
    }

    QList<Diagnostic> diags;
    Diagnostic d;
    d.severity = Diagnostic::Error;
    // A plan that knows why it cannot proceed says so; an empty plan is the two
    // cases this guard has always covered.
    d.message = !planError.isEmpty()
                    ? planError
                    : (usesLinker()
                           ? tr("More than one source needs a linker, and none is configured.")
                           : tr("Nothing to build."));
    diags.append(d);
    emit finished(false, diags);
}

bool BuildService::planSteps(QString *error)
{
    const QString base = m_outputFile;

    if (!usesLinker()) {
        // Single source: still one assembler invocation producing the program
        // directly, which needs no linker at all.
        m_listingFiles.append(m_listingFile);
        m_objectFiles.append(QString());

        Step step;
        step.program = m_assemblerPath;
        step.arguments << QStringLiteral("-quiet") << QStringLiteral("-Ftos");
        if (!m_cpu.isEmpty())
            step.arguments << (QStringLiteral("-m") + m_cpu);
        for (const QString &inc : m_includePaths)
            step.arguments << (QStringLiteral("-I") + inc);
        for (const QString &def : m_defines)
            step.arguments << (QStringLiteral("-D") + def);
        if (!m_listingFile.isEmpty())
            step.arguments << QStringLiteral("-L") << m_listingFile;
        step.arguments << QStringLiteral("-o") << m_outputFile;
        step.arguments << m_extraArgs;
        step.arguments << m_sourceFile;
        m_steps.append(step);
        return true;
    }

    // More than one source needs a linker, and this one is not configured: plan
    // nothing, so build()'s own guard reports exactly that, rather than a link
    // step whose empty program QProcess can only answer with "Could not run ''.
    // Is it installed?" — which is what the caller used to see instead (finding
    // MIN-36).
    if (m_linkerPath.isEmpty())
        return true;

    // Separate compilation. Each module is assembled to an object with its own
    // listing, because a listing's offsets are relative to the module and the
    // linker will move it (docs/PLAN.md §4.3).
    QStringList sources{m_sourceFile};
    sources += m_additionalSources;

    // Resolve every module's paths before any step is built. Two sources whose
    // names differ only in their suffix (`a.s` and `a.asm`) derive the same
    // `a.o`/`a.lst`: the second assembly overwrote the first's object and the
    // linker received the same path twice, so one module was linked twice, the
    // other was missing, and nothing said so (finding MIN-40). Refuse the plan
    // instead, naming both sources.
    QStringList objects;
    QStringList listings;
    QHash<QString, QString> owners;   // resolved object path -> the source that claims it
    for (const QString &source : sources) {
        const QFileInfo info(source);
        const QString object = info.absolutePath() + QLatin1Char('/')
                             + info.completeBaseName() + QStringLiteral(".o");
        const QString listing = info.absolutePath() + QLatin1Char('/')
                              + info.completeBaseName() + QStringLiteral(".lst");
        // The collision is on the resolved path, not on the spelling: `a.s` and
        // `./a.s` name one module, whose object is the same file.
        const QString key = QDir::cleanPath(QFileInfo(object).absoluteFilePath());
        const auto claimed = owners.constFind(key);
        if (claimed != owners.constEnd()) {
            *error = tr("%1 and %2 both assemble to %3, so one module would overwrite the other. "
                        "Rename one of them.")
                         .arg(claimed.value(), source, object);
            return false;
        }
        owners.insert(key, source);
        objects.append(object);
        listings.append(listing);
    }

    for (int i = 0; i < sources.size(); ++i) {
        const QString &source = sources.at(i);
        const QString &object = objects.at(i);
        const QString &listing = listings.at(i);
        m_objectFiles.append(object);
        m_listingFiles.append(listing);

        Step step;
        step.program = m_assemblerPath;
        step.arguments << QStringLiteral("-quiet") << QStringLiteral("-Fvobj");
        if (!m_cpu.isEmpty())
            step.arguments << (QStringLiteral("-m") + m_cpu);
        for (const QString &inc : m_includePaths)
            step.arguments << (QStringLiteral("-I") + inc);
        for (const QString &def : m_defines)
            step.arguments << (QStringLiteral("-D") + def);
        step.arguments << QStringLiteral("-L") << listing;
        step.arguments << QStringLiteral("-o") << object;
        step.arguments << m_extraArgs;
        step.arguments << source;
        m_steps.append(step);
    }

    Step link;
    link.program = m_linkerPath;
    link.isLinker = true;
    link.arguments << QStringLiteral("-b") << QStringLiteral("ataritos");
    if (!m_linkMapFile.isEmpty())
        link.arguments << (QStringLiteral("-M") + m_linkMapFile);
    link.arguments << QStringLiteral("-o") << base;
    link.arguments << objects;
    m_steps.append(link);
    return true;
}

void BuildService::runNextStep()
{
    if (m_nextStep >= m_steps.size()) {
        finishBuild(!m_sawFailure);
        return;
    }

    const Step &step = m_steps.at(m_nextStep);

    // A failed module stops the chain: linking whatever assembled would report
    // the same problem a second time, from the linker, with less useful wording.
    if (m_sawFailure && step.isLinker) {
        finishBuild(false);
        return;
    }

    m_stderrBuffer.clear();
    m_stdoutBuffer.clear();

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

    const QString program = step.program;
    connectFailedToStart(m_process, this, [this, program] {
        // Qt emits `finished` only when a child dies, so a process that never
        // started would otherwise complete nothing at all: the caller would wait
        // forever, a pending Run would stay armed, and the QProcess would leak.
        Diagnostic d;
        d.severity = Diagnostic::Error;
        d.message = tr("Could not run '%1'. Is it installed?").arg(program);
        m_diagnostics.append(d);
        m_sawFailure = true;

        killAndRelease(m_process, this);
        m_process->deleteLater();
        m_process = nullptr;
        ++m_nextStep;
        runNextStep();
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                // The readyRead loops only consume up to '\n', so a step's final
                // line — when it has no trailing newline, as vlink's last diagnostic
                // often is — sits in the buffer and would be dropped when the next
                // step clears it. Drain any last bytes and flush the remainder as a
                // final line (finding B12).
                m_stderrBuffer += m_process->readAllStandardError();
                m_stdoutBuffer += m_process->readAllStandardOutput();
                if (!m_stderrBuffer.isEmpty()) {
                    handleStderrLine(
                        QString::fromUtf8(m_stderrBuffer).remove(QLatin1Char('\r')));
                    m_stderrBuffer.clear();
                }
                if (!m_stdoutBuffer.isEmpty()) {
                    handleStdoutLine(
                        QString::fromUtf8(m_stdoutBuffer).remove(QLatin1Char('\r')));
                    m_stdoutBuffer.clear();
                }
                if (!(status == QProcess::NormalExit && exitCode == 0))
                    m_sawFailure = true;
                m_process->deleteLater();
                m_process = nullptr;
                ++m_nextStep;
                runNextStep();
            });

    m_process->start(step.program, step.arguments);
}

void BuildService::finishBuild(bool success)
{
    // The linker's own output file is the program; nothing else to check, since a
    // failed step is already recorded in the diagnostics.
    emit finished(success, m_diagnostics);
}

void BuildService::handleStderrLine(const QString &line)
{
    // The two tools use different diagnostic grammars, so the parser depends on
    // which step produced the line.
    if (m_nextStep >= 0 && m_nextStep < m_steps.size() && m_steps.at(m_nextStep).isLinker) {
        handleLinkerLine(line);
        return;
    }

    Diagnostic d;
    if (parseVasmDiagnostic(line, &d)) {
        m_diagnostics.append(d);
        emit outputLine(line);
        return;
    }

    if (!line.trimmed().isEmpty())
        emit outputLine(line);
}

void BuildService::handleLinkerLine(const QString &line)
{
    Diagnostic d;
    if (parseLinkerDiagnostic(line, &d)) {
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

} // namespace pist
