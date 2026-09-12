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

    /// Additional sources to assemble and link with the primary one.
    ///
    /// More than one source switches the build to separate compilation: each is
    /// assembled to an object and the set is linked into the final program, since
    /// `vasm -Ftos` can only produce a program from a single file. The first
    /// source listed is the entry module, and TOS starts executing at the
    /// beginning of text, so its code must come first (docs/PLAN.md §4.3).
    void setAdditionalSources(const QStringList &paths) { m_additionalSources = paths; }
    QStringList additionalSources() const { return m_additionalSources; }

    /// The linker. Required when there is more than one source.
    void setLinkerPath(const QString &path) { m_linkerPath = path; }
    QString linkerPath() const { return m_linkerPath; }

    /// Where the linker writes its placement map. Read back to map source lines
    /// to addresses across modules.
    void setLinkMapFile(const QString &path) { m_linkMapFile = path; }
    QString linkMapFile() const { return m_linkMapFile; }

    /// Listings produced by this build, primary first. One per module.
    QStringList listingFiles() const { return m_listingFiles; }

    /// Objects produced by this build, in the same order as the sources.
    QStringList objectFiles() const { return m_objectFiles; }

    bool usesLinker() const;

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

public slots:
    void build();
    void cancel();

signals:
    void finished(bool success, const QList<pist::Diagnostic> &diagnostics);
    void outputLine(const QString &line);
    void stepStarted(const QString &description);

private:
    /// One command in the build. A single-source build is one step; a linked
    /// build is one assembly step per module plus the link.
    struct Step
    {
        QString program;
        QStringList arguments;
        QString description;
        bool isLinker = false;
    };

    void planSteps();
    void runNextStep();
    void finishBuild(bool success);
    void handleStderrLine(const QString &line);
    void handleStdoutLine(const QString &line);
    void handleLinkerLine(const QString &line);
    bool parseLocated(const QString &line);

    QString m_assemblerPath;
    QString m_linkerPath;
    QString m_sourceFile;
    QStringList m_additionalSources;
    QString m_outputFile;
    QString m_listingFile;
    QString m_linkMapFile;
    QStringList m_includePaths;
    QStringList m_defines;
    QString m_cpu;
    QStringList m_extraArgs;

    QProcess *m_process = nullptr;
    QList<Diagnostic> m_diagnostics;
    QList<Step> m_steps;
    int m_nextStep = 0;
    bool m_running = false;
    QStringList m_listingFiles;
    QStringList m_objectFiles;
    bool m_sawFailure = false;
    QByteArray m_stderrBuffer;
    QByteArray m_stdoutBuffer;
};

} // namespace pist
