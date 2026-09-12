// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/Diagnostic.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

namespace pist {

/// Drives `vasmm68k_mot` as a subprocess and turns its stderr into diagnostics.
///
/// The assembler is never modified or linked against: everything is expressed
/// as command-line arguments, which keeps bundled binaries redistributable
/// under their own terms (docs/PLAN.md §7).
class BuildService : public QObject
{
    Q_OBJECT

public:
    explicit BuildService(QObject *parent = nullptr);
    ~BuildService() override;

    void setAssemblerPath(const QString &path) { m_assemblerPath = path; }
    QString assemblerPath() const { return m_assemblerPath; }

    void setSourceFile(const QString &path) { m_sourceFile = path; }
    QString sourceFile() const { return m_sourceFile; }

    void setOutputFile(const QString &path) { m_outputFile = path; }
    QString outputFile() const { return m_outputFile; }

    void setListingFile(const QString &path) { m_listingFile = path; }
    QString listingFile() const { return m_listingFile; }

    void setIncludePaths(const QStringList &paths) { m_includePaths = paths; }
    void setDefines(const QStringList &defines) { m_defines = defines; }

    /// Target CPU, e.g. "68000". Empty leaves vasm's default.
    void setCpu(const QString &cpu) { m_cpu = cpu; }

    void setExtraArgs(const QStringList &args) { m_extraArgs = args; }

    bool isRunning() const;

    /// The exact argv used by the most recent build, for the build log.
    QStringList lastCommand() const { return m_lastCommand; }

public slots:
    void build();
    void cancel();

signals:
    void started(const QStringList &argv);
    void finished(bool success, const QList<pist::Diagnostic> &diagnostics);
    void outputLine(const QString &line);

private:
    void handleStderrLine(const QString &line);
    void handleStdoutLine(const QString &line);
    void flushPendingExcerpt();

    QString m_assemblerPath;
    QString m_sourceFile;
    QString m_outputFile;
    QString m_listingFile;
    QStringList m_includePaths;
    QStringList m_defines;
    QString m_cpu;
    QStringList m_extraArgs;
    QStringList m_lastCommand;

    QProcess *m_process = nullptr;
    QList<Diagnostic> m_diagnostics;
    QByteArray m_stderrBuffer;
    QByteArray m_stdoutBuffer;

    /// Index into m_diagnostics of the diagnostic awaiting its `> excerpt` line.
    int m_awaitingExcerpt = -1;
};

} // namespace pist
