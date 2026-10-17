// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/StopHooksDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace pist {

StopHooksDialog::StopHooksDialog(const QString &scriptPath, bool enabled, bool autoContinue,
                                 const QString &lastRun, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Stop hooks"));

    auto *intro = new QLabel(
        tr("Runs a script at every debugger stop, with the machine held stopped — so it can look "
           "at and change registers and memory, and decide to continue."),
        this);
    intro->setObjectName(QStringLiteral("stopHooksIntro"));
    intro->setWordWrap(true);

    m_enabled = new QCheckBox(tr("Run the script at every stop"), this);
    m_enabled->setObjectName(QStringLiteral("stopHooksEnabled"));
    m_enabled->setChecked(enabled);

    m_path = new QLineEdit(scriptPath, this);
    m_path->setObjectName(QStringLiteral("stopHooksPath"));
    m_path->setPlaceholderText(tr("a .lua file in the project"));
    auto *browse = new QPushButton(tr("Browse…"), this);
    browse->setObjectName(QStringLiteral("stopHooksBrowse"));
    connect(browse, &QPushButton::clicked, this, &StopHooksDialog::browse);

    auto *pathRow = new QWidget(this);
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->addWidget(m_path, 1);
    pathLayout->addWidget(browse);

    m_autoContinue = new QCheckBox(tr("Let the script continue without asking"), this);
    m_autoContinue->setObjectName(QStringLiteral("stopHooksAutoContinue"));
    m_autoContinue->setChecked(autoContinue);
    m_autoContinue->setToolTip(
        tr("A script's continue() keeps the program running. With this off it is logged and the "
           "machine stays stopped."));

    auto *api = new QLabel(
        tr("The script is Lua. It may call readmem(addr, len), writemem(addr, value), "
           "readreg(name), writereg(name, value), log(text) and continue(). No file or network "
           "access is offered."),
        this);
    api->setObjectName(QStringLiteral("stopHooksApi"));
    api->setWordWrap(true);

    auto *last = new QLabel(
        lastRun.isEmpty() ? tr("Last run: none yet this session.")
                          : tr("Last run: %1").arg(lastRun),
        this);
    last->setObjectName(QStringLiteral("stopHooksLastRun"));
    last->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Script:"), pathRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &StopHooksDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_enabled);
    layout->addLayout(form);
    layout->addWidget(m_autoContinue);
    layout->addWidget(api);
    layout->addWidget(last);
    layout->addStretch(1);
    layout->addWidget(buttons);

    // The path only means something while the hook is on; the auto-continue
    // question only matters once it runs at all.
    const auto syncEnabled = [this] {
        const bool on = m_enabled->isChecked();
        m_path->setEnabled(on);
        m_autoContinue->setEnabled(on);
    };
    connect(m_enabled, &QCheckBox::toggled, this, syncEnabled);
    syncEnabled();
}

QString StopHooksDialog::scriptPath() const
{
    return m_path->text().trimmed();
}

bool StopHooksDialog::enabled() const
{
    return m_enabled->isChecked();
}

bool StopHooksDialog::autoContinue() const
{
    return m_autoContinue->isChecked();
}

void StopHooksDialog::browse()
{
    const QString start = scriptPath().isEmpty() ? QString() : scriptPath();
    const QString chosen = QFileDialog::getOpenFileName(
        this, tr("Stop hook script"), start,
        tr("Lua scripts (*.lua);;All files (*)"));
    if (!chosen.isEmpty())
        m_path->setText(chosen);
}

void StopHooksDialog::accept()
{
    if (enabled()) {
        const QString path = scriptPath();
        if (path.isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("Choose the script the hook should run."));
            return;
        }
        if (!QFileInfo(path).isReadable()) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("'%1' cannot be read.").arg(path));
            return;
        }
    }
    QDialog::accept();
}

} // namespace pist
