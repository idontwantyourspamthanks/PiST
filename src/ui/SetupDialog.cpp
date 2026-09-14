// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SetupDialog.h"

#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "toolchain/ToolFetch.h"
#include "toolchain/Toolchain.h"

#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <tuple>

namespace pist {

namespace {

QLabel *makeDetail(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse
                                   | Qt::TextSelectableByKeyboard);
    return label;
}

} // namespace

SetupDialog::SetupDialog(QWidget *parent)
    : QDialog(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    setWindowTitle(tr("Set up tools and ROMs"));
    setObjectName(QStringLiteral("setupDialog"));

    auto *layout = new QVBoxLayout(this);

    auto *intro = makeDetail(this);
    intro->setText(tr("PiST drives external programs and never patches or links them, so a "
                      "few pieces have to come from their authors. Each fetch below names "
                      "exactly what is downloaded, from where, and the checksum it is "
                      "verified against before anything is built or installed."));
    layout->addWidget(intro);

    // ------------------------------------------------------- assembler (vasm)
    auto *vasmBox = new QGroupBox(tr("Assembler — vasmm68k_mot"), this);
    auto *vasmLayout = new QVBoxLayout(vasmBox);
    m_vasmStatus = makeDetail(vasmBox);
    m_vasmStatus->setObjectName(QStringLiteral("vasmStatus"));
    m_vasmDetail = makeDetail(vasmBox);
    m_vasmButton = new QPushButton(vasmBox);
    m_vasmButton->setObjectName(QStringLiteral("vasmFetchButton"));
    m_vasmProgress = new QProgressBar(vasmBox);
    m_vasmProgress->setVisible(false);
    vasmLayout->addWidget(m_vasmStatus);
    vasmLayout->addWidget(m_vasmDetail);
    vasmLayout->addWidget(m_vasmButton);
    vasmLayout->addWidget(m_vasmProgress);
    layout->addWidget(vasmBox);
    connect(m_vasmButton, &QPushButton::clicked, this, &SetupDialog::fetchVasm);

    // ------------------------------------------------------ emulator (Hatari)
    auto *emuBox = new QGroupBox(tr("Emulator — Hatari"), this);
    auto *emuLayout = new QVBoxLayout(emuBox);
    m_emulatorStatus = makeDetail(emuBox);
    m_emulatorStatus->setObjectName(QStringLiteral("emulatorStatus"));
    m_emulatorDetail = makeDetail(emuBox);
    emuLayout->addWidget(m_emulatorStatus);
    emuLayout->addWidget(m_emulatorDetail);
    layout->addWidget(emuBox);

    // ------------------------------------------------------------- TOS ROM
    auto *romBox = new QGroupBox(tr("TOS ROM"), this);
    auto *romLayout = new QVBoxLayout(romBox);
    m_romStatus = makeDetail(romBox);
    m_romStatus->setObjectName(QStringLiteral("romStatus"));
    m_romDetail = makeDetail(romBox);
    m_romButton = new QPushButton(romBox);
    m_romButton->setObjectName(QStringLiteral("romFetchButton"));
    m_romProgress = new QProgressBar(romBox);
    m_romProgress->setVisible(false);
    romLayout->addWidget(m_romStatus);
    romLayout->addWidget(m_romDetail);
    romLayout->addWidget(m_romButton);
    romLayout->addWidget(m_romProgress);
    layout->addWidget(romBox);
    connect(m_romButton, &QPushButton::clicked, this, &SetupDialog::fetchEmuTos);

    // ------------------------------------------------------------------ log
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    layout->addWidget(m_log);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_closeButton = buttons->button(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    resize(640, 640);
    refresh();
}

bool SetupDialog::anythingMissing()
{
    return !toolchain::findAssembler().found()
        || !toolchain::findEmulator().found()
        || findTosRoms().isEmpty();
}

const QString &SetupDialog::dismissalKey()
{
    static const QString key = QStringLiteral("setup/promptDismissed");
    return key;
}

bool SetupDialog::shouldPromptAtStartup()
{
    // All three pieces count — including the emulator, which is the *only*
    // thing missing on the macOS and Windows release archives, so excluding
    // it would mean the prompt never fires where it is needed most. The
    // non-nagging property comes from the dismissal instead: once the user has
    // seen the dialog it never opens unprompted again (the Tools menu entry
    // remains), which covers the "no in-dialog remedy for Hatari" case.
    return anythingMissing() && !QSettings().value(dismissalKey()).toBool();
}

void SetupDialog::done(int result)
{
    // Shown once, never unprompted again — dismissal is not tied to whether
    // anything was actually installed, because the dialog may have no remedy
    // for what is missing (the emulator on macOS/Windows).
    QSettings().setValue(dismissalKey(), true);
    QDialog::done(result);
}

void SetupDialog::reject()
{
    if (m_reply) {
        // Abort the download; the finished handler re-enables everything, and
        // the user can close again.
        m_reply->abort();
        return;
    }
    if (m_busy) {
        // A build/install is running on the worker and cannot be safely
        // cancelled; destroying the dialog now would orphan it.
        appendLog(tr("An install is in progress; please wait for it to finish."));
        return;
    }
    QDialog::reject();
}

void SetupDialog::closeEvent(QCloseEvent *event)
{
    if (m_reply || m_busy) {
        event->ignore();
        reject();
        return;
    }
    QDialog::closeEvent(event);
}

void SetupDialog::refresh()
{
    const ToolInfo assembler = toolchain::findAssembler();
    if (assembler.found()) {
        m_vasmStatus->setText(tr("Found: %1%2")
                                  .arg(assembler.path,
                                       assembler.version.isEmpty()
                                           ? QString()
                                           : tr(" (version %1)").arg(assembler.version)));
        m_vasmDetail->setVisible(false);
        m_vasmButton->setVisible(false);
    } else {
        m_vasmStatus->setText(tr("Not found. PiST cannot build anything without it."));
        QString detail = tr(
            "vasm is not free software: its licence permits redistribution unmodified for "
            "non-commercial use, which is why PiST fetches it from the author and never "
            "patches it. Building from the pinned source archive needs `make` and a C "
            "compiler.\n\nSource: %1\nsha256: %2\nInstalls to: %3")
                             .arg(QLatin1String(toolchain::pins::kVasmUrl),
                                  QLatin1String(toolchain::pins::kVasmSha256),
                                  toolchain::suggestedInstallDir());
        const bool canBuild = toolchain::canBuildVasm();
        if (!canBuild) {
            detail += tr("\n\nThis machine has no make/C compiler, so vasm cannot be built "
                         "here. The release bundles for Windows already carry a prebuilt "
                         "vasm beside the executable; otherwise install it yourself (see the "
                         "path above) or set an explicit path in Project Settings.");
        }
        m_vasmDetail->setText(detail);
        m_vasmDetail->setVisible(true);
        m_vasmButton->setText(tr("Download and build vasm %1")
                                  .arg(QLatin1String(toolchain::pins::kVasmVersion)));
        m_vasmButton->setEnabled(canBuild && !m_busy);
        m_vasmButton->setVisible(true);
    }

    const ToolInfo emulator = toolchain::findEmulator();
    if (emulator.found()) {
        m_emulatorStatus->setText(tr("Found: %1%2")
                                      .arg(emulator.path,
                                           emulator.version.isEmpty()
                                               ? QString()
                                               : tr(" (version %1)").arg(emulator.version)));
        m_emulatorDetail->setVisible(false);
    } else {
        m_emulatorStatus->setText(tr("Not found. Programs can be built but not run."));
        m_emulatorDetail->setText(toolchain::emulatorInstallHint());
        m_emulatorDetail->setVisible(true);
    }

    const QList<TosRom> roms = findTosRoms();
    if (!roms.isEmpty()) {
        QStringList names;
        for (const TosRom &rom : roms)
            names << rom.versionText();
        m_romStatus->setText(tr("Found %1: %2")
                                 .arg(roms.size())
                                 .arg(names.join(QStringLiteral(", "))));
        m_romDetail->setVisible(false);
        m_romButton->setVisible(false);
    } else {
        m_romStatus->setText(tr("None found. Nothing can run without a ROM."));
        m_romDetail->setText(tr(
            "EmuTOS is a free (GPLv2) replacement for the original Atari TOS that boots and "
            "autostarts programs from the emulated hard disk, so it is enough to build, run "
            "and debug with.\n\nDownload: %1\nsha256: %2\nInstalls to: %3")
                                 .arg(QLatin1String(toolchain::pins::kEmuTosUrl),
                                      QLatin1String(toolchain::pins::kEmuTosSha256),
                                      paths::suggestedRomDir()));
        m_romDetail->setVisible(true);
        m_romButton->setText(tr("Download EmuTOS %1")
                                 .arg(QLatin1String(toolchain::pins::kEmuTosVersion)));
        m_romButton->setEnabled(!m_busy);
        m_romButton->setVisible(true);
    }
}

void SetupDialog::fetchVasm()
{
    appendLog(tr("Downloading %1").arg(QLatin1String(toolchain::pins::kVasmUrl)));
    m_vasmProgress->setVisible(true);
    startDownload(QLatin1String(toolchain::pins::kVasmUrl), m_vasmProgress,
                  [this](const QString &path) {
                      runWorker([path](QString *log, QString *error) {
                          const QString installed = toolchain::installVasmFromTarball(
                              path, QLatin1String(toolchain::pins::kVasmSha256), log, error);
                          QFile::remove(path);
                          return installed;
                      });
                  });
}

void SetupDialog::fetchEmuTos()
{
    appendLog(tr("Downloading %1").arg(QLatin1String(toolchain::pins::kEmuTosUrl)));
    m_romProgress->setVisible(true);
    startDownload(QLatin1String(toolchain::pins::kEmuTosUrl), m_romProgress,
                  [this](const QString &path) {
                      runWorker([path](QString *log, QString *error) {
                          Q_UNUSED(log);
                          const QString installed = toolchain::installEmuTosFromZip(
                              path, QLatin1String(toolchain::pins::kEmuTosSha256), error);
                          QFile::remove(path);
                          return installed;
                      });
                  });
}

void SetupDialog::startDownload(const QString &url, QProgressBar *bar,
                                std::function<void(const QString &path)> onReady)
{
    setBusy(true);

    auto *file = new QTemporaryFile(QDir::temp().filePath(QStringLiteral("pist-dl-XXXXXX")), this);
    if (!file->open()) {
        appendLog(tr("Could not create a temporary file for the download."));
        file->deleteLater();
        setBusy(false);
        return;
    }
    m_download = file;

    QNetworkRequest request{QUrl(url)};
    // SourceForge refuses requests with Qt's default (absent) User-Agent with
    // a bare 403 before redirecting, so name ourselves honestly.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("PiST/%1 first-run setup fetch")
                          .arg(QCoreApplication::applicationVersion()));
    // SourceForge's /download link redirects to a mirror; Qt does not follow
    // redirects by default. NoLessSafe permits every redirect except an
    // https→http downgrade.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    // A stalled transfer must surface as a failure, not leave every control
    // dead: with no timeout a dead socket hangs the dialog indefinitely.
    request.setTransferTimeout(120000);

    m_reply = m_nam->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_download)
            m_download->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [bar](qint64 received, qint64 total) {
                bar->setMaximum(total > 0 ? int(total / 1024) : 0);
                bar->setValue(int(received / 1024));
            });
    connect(m_reply, &QNetworkReply::finished, this, [this, bar, onReady] {
        bar->setVisible(false);
        const QNetworkReply::NetworkError code = m_reply->error();
        const QString message = m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;

        if (code != QNetworkReply::NoError) {
            appendLog(tr("Download failed: %1").arg(message));
            m_download->deleteLater();
            m_download = nullptr;
            setBusy(false);
            refresh();
            return;
        }

        m_download->flush();
        const QString path = m_download->fileName();
        m_download->setAutoRemove(false);
        m_download->deleteLater();
        m_download = nullptr;
        appendLog(tr("Downloaded %1 KiB; verifying checksum.")
                      .arg(QFileInfo(path).size() / 1024));
        onReady(path);
    });
}

void SetupDialog::runWorker(std::function<QString(QString *log, QString *error)> fn)
{
    setBusy(true);

    auto *result = new std::tuple<QString, QString, QString>;
    QThread *thread = QThread::create([fn, result] {
        QString log, error;
        const QString installed = fn(&log, &error);
        *result = std::make_tuple(installed, log, error);
    });
    connect(thread, &QThread::finished, this, [this, thread, result] {
        thread->deleteLater();
        const auto [installed, log, error] = *result;
        delete result;
        if (!log.trimmed().isEmpty())
            appendLog(log.trimmed());
        if (installed.isEmpty()) {
            appendLog(tr("Failed: %1").arg(error));
        } else {
            appendLog(tr("Installed: %1").arg(installed));
        }
        setBusy(false);
        refresh();
    });
    thread->start();
}

void SetupDialog::appendLog(const QString &text)
{
    m_log->appendPlainText(text);
}
void SetupDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_vasmButton->setEnabled(!busy && toolchain::canBuildVasm());
    m_romButton->setEnabled(!busy);
}

} // namespace pist
