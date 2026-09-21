// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QDialog>
#include <QString>

#include <functional>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryFile;

namespace pist {

/// First-run setup: what PiST found, and a guided fetch for what it did not.
///
/// Shown unprompted at startup when anythingMissing() holds, and reopenable
/// from Project Settings. Every fetch shows the URL and the pinned checksum
/// before anything is downloaded — a convenience, never a silent download
/// (docs/PLAN.md §7).
class SetupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SetupDialog(QWidget *parent = nullptr);

    /// True when a piece PiST needs is missing: the assembler, the emulator,
    /// or any TOS ROM.
    static bool anythingMissing();

    /// True when the dialog should appear unprompted at startup: something is
    /// missing AND the user has not dismissed the dialog before. Dismissal is
    /// the non-nagging mechanism — the emulator is the *only* piece missing on
    /// the macOS/Windows release archives, so gating on what a fetch can fix
    /// would mean the prompt never fires exactly where it is needed.
    static bool shouldPromptAtStartup();

    /// The QSettings key behind shouldPromptAtStartup, exposed so tests clear
    /// exactly what the production code reads.
    static const QString &dismissalKey();

    /// Closing while a download is in flight aborts it; closing while a build
    /// or install runs is refused (the worker cannot be safely cancelled), so
    /// a session is never destroyed from under its own worker.
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
    /// Records the dismissal behind shouldPromptAtStartup().
    void done(int result) override;

private:
    /// Re-run discovery and update every row. Called on open and after each
    /// successful install.
    void refresh();

    void fetchVasm();
    void fetchEmuTos();

    /// Download url to a temporary file, reporting progress on `bar`, then hand
    /// the local path to onReady (which runs the verify/install steps on a
    /// worker thread).
    void startDownload(const QString &url, QProgressBar *bar,
                       std::function<void(const QString &path)> onReady);

    /// Run fn off the GUI thread, delivering its result here. The fetch
    /// buttons are disabled for the duration; Close stays enabled and aborts
    /// or is refused per reject().
    void runWorker(std::function<QString(QString *log, QString *error)> fn);

    void appendLog(const QString &text);
    void setBusy(bool busy);

    QLabel *m_vasmStatus = nullptr;
    QLabel *m_vasmDetail = nullptr;
    QPushButton *m_vasmButton = nullptr;
    QProgressBar *m_vasmProgress = nullptr;

    QLabel *m_emulatorStatus = nullptr;
    QLabel *m_emulatorDetail = nullptr;

    QLabel *m_romStatus = nullptr;
    QLabel *m_romDetail = nullptr;
    QPushButton *m_romButton = nullptr;
    QProgressBar *m_romProgress = nullptr;

    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_closeButton = nullptr;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTemporaryFile *m_download = nullptr;

    bool m_busy = false;
};

} // namespace pist
