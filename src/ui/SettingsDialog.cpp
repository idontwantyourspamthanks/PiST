// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SettingsDialog.h"

#include "emu/TosRom.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

/// Data role holding a ROM's absolute path on a combo entry.
constexpr int kRomPathRole = Qt::UserRole + 1;

/// One argument per line, blank lines ignored.
QStringList linesToArgs(const QString &text)
{
    QStringList args;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            args.append(trimmed);
    }
    return args;
}

} // namespace

SettingsDialog::SettingsDialog(const ProjectSettings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Project settings"));
    buildUi();
    loadValues(settings);
    resize(620, 520);
}

void SettingsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);

    // ---------------------------------------------------------------- build
    auto *buildTab = new QWidget(this);
    auto *buildLayout = new QFormLayout(buildTab);

    m_cpu = new QComboBox(buildTab);
    // Taken from vasm's documented -m values. The ST is a plain 68000; the rest
    // are here because vasm accepts them and some projects target accelerators.
    m_cpu->addItems({QStringLiteral("68000"), QStringLiteral("68010"),
                     QStringLiteral("68020"), QStringLiteral("68030"),
                     QStringLiteral("68040"), QStringLiteral("68060")});
    buildLayout->addRow(tr("CPU:"), m_cpu);

    // Include paths
    m_includePaths = new QListWidget(buildTab);
    m_includePaths->setSelectionMode(QAbstractItemView::SingleSelection);
    auto *incButtons = new QHBoxLayout;
    auto *addInc = new QPushButton(tr("Add folder…"), buildTab);
    auto *removeInc = new QPushButton(tr("Remove"), buildTab);
    incButtons->addWidget(addInc);
    incButtons->addWidget(removeInc);
    incButtons->addStretch(1);
    connect(addInc, &QPushButton::clicked, this, &SettingsDialog::addIncludePath);
    connect(removeInc, &QPushButton::clicked, this, &SettingsDialog::removeIncludePath);

    auto *incBox = new QWidget(buildTab);
    auto *incLayout = new QVBoxLayout(incBox);
    incLayout->setContentsMargins(0, 0, 0, 0);
    incLayout->addWidget(m_includePaths);
    incLayout->addLayout(incButtons);
    buildLayout->addRow(tr("Include paths:"), incBox);

    // Defines
    m_defines = new QListWidget(buildTab);
    auto *defButtons = new QHBoxLayout;
    auto *addDef = new QPushButton(tr("Add…"), buildTab);
    auto *removeDef = new QPushButton(tr("Remove"), buildTab);
    defButtons->addWidget(addDef);
    defButtons->addWidget(removeDef);
    defButtons->addStretch(1);
    connect(addDef, &QPushButton::clicked, this, &SettingsDialog::addDefine);
    connect(removeDef, &QPushButton::clicked, this, &SettingsDialog::removeDefine);

    auto *defBox = new QWidget(buildTab);
    auto *defLayout = new QVBoxLayout(defBox);
    defLayout->setContentsMargins(0, 0, 0, 0);
    defLayout->addWidget(m_defines);
    defLayout->addLayout(defButtons);
    buildLayout->addRow(tr("Defines:"), defBox);

    m_extraBuildArgs = new QPlainTextEdit(buildTab);
    m_extraBuildArgs->setPlaceholderText(tr("One argument per line"));
    m_extraBuildArgs->setMaximumHeight(70);
    buildLayout->addRow(tr("Extra assembler args:"), m_extraBuildArgs);

    tabs->addTab(buildTab, tr("Build"));

    // ------------------------------------------------------------- emulator
    auto *emuTab = new QWidget(this);
    auto *emuLayout = new QFormLayout(emuTab);

    m_machine = new QComboBox(emuTab);
    for (Machine machine : allMachines())
        m_machine->addItem(machineDisplayName(machine), static_cast<int>(machine));
    connect(m_machine, &QComboBox::currentIndexChanged, this, &SettingsDialog::onMachineChanged);
    emuLayout->addRow(tr("Machine:"), m_machine);

    // ROM: a combo plus a browse button, because a user's ROMs may live anywhere.
    auto *romRow = new QWidget(emuTab);
    auto *romLayout = new QHBoxLayout(romRow);
    romLayout->setContentsMargins(0, 0, 0, 0);
    m_rom = new QComboBox(romRow);
    m_rom->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto *browse = new QPushButton(tr("Browse…"), romRow);
    connect(browse, &QPushButton::clicked, this, &SettingsDialog::browseRom);
    romLayout->addWidget(m_rom, 1);
    romLayout->addWidget(browse);
    emuLayout->addRow(tr("TOS ROM:"), romRow);

    m_romNote = new QLabel(emuTab);
    m_romNote->setWordWrap(true);
    m_romNote->setStyleSheet(QStringLiteral("color: palette(mid);"));
    emuLayout->addRow(QString(), m_romNote);

    m_monitor = new QComboBox(emuTab);
    m_monitor->addItems({QStringLiteral("mono"), QStringLiteral("rgb"),
                         QStringLiteral("vga"), QStringLiteral("tv")});
    emuLayout->addRow(tr("Monitor:"), m_monitor);

    m_ram = new QSpinBox(emuTab);
    m_ram->setRange(0, 14);
    m_ram->setSuffix(tr(" MiB"));
    m_ram->setSpecialValueText(tr("512 KiB (default)"));
    emuLayout->addRow(tr("ST RAM:"), m_ram);

    auto *hdRow = new QWidget(emuTab);
    auto *hdLayout = new QHBoxLayout(hdRow);
    hdLayout->setContentsMargins(0, 0, 0, 0);
    m_hardDisk = new QLineEdit(hdRow);
    m_hardDisk->setPlaceholderText(tr("Optional disk image for ACSI/IDE"));
    auto *browseHd = new QPushButton(tr("Browse…"), hdRow);
    connect(browseHd, &QPushButton::clicked, this, &SettingsDialog::browseHardDisk);
    hdLayout->addWidget(m_hardDisk, 1);
    hdLayout->addWidget(browseHd);
    emuLayout->addRow(tr("Hard disk:"), hdRow);

    m_floppyNote = new QLabel(
        tr("Floppy images are not configurable yet. The GEMDOS hard-disk route is used for "
           "running programs, which is what makes debugging attach to them."),
        emuTab);
    m_floppyNote->setWordWrap(true);
    m_floppyNote->setStyleSheet(QStringLiteral("color: palette(mid);"));
    emuLayout->addRow(QString(), m_floppyNote);

    m_extraEmuArgs = new QPlainTextEdit(emuTab);
    m_extraEmuArgs->setPlaceholderText(tr("One argument per line"));
    m_extraEmuArgs->setMaximumHeight(70);
    emuLayout->addRow(tr("Extra emulator args:"), m_extraEmuArgs);

    tabs->addTab(emuTab, tr("Emulator"));

    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::loadValues(const ProjectSettings &settings)
{
    const int cpuIndex = m_cpu->findText(settings.cpu);
    if (cpuIndex >= 0)
        m_cpu->setCurrentIndex(cpuIndex);

    for (const QString &path : settings.includePaths)
        m_includePaths->addItem(path);
    for (const QString &define : settings.defines)
        m_defines->addItem(define);

    m_extraBuildArgs->setPlainText(settings.extraBuildArgs.join(QLatin1Char('\n')));

    const int machineIndex = m_machine->findData(static_cast<int>(settings.machine));
    if (machineIndex >= 0)
        m_machine->setCurrentIndex(machineIndex);

    const int monitorIndex = m_monitor->findText(settings.monitor);
    if (monitorIndex >= 0)
        m_monitor->setCurrentIndex(monitorIndex);

    m_ram->setValue(settings.memSizeMiB);
    m_hardDisk->setText(settings.hardDiskImage);
    m_extraEmuArgs->setPlainText(settings.extraEmulatorArgs.join(QLatin1Char('\n')));

    refreshRomList();
}

void SettingsDialog::refreshRomList()
{
    const Machine machine = static_cast<Machine>(m_machine->currentData().toInt());

    m_rom->clear();
    m_rom->addItem(tr("Automatic (best for this machine)"), QString());

    for (const TosRom &rom : findTosRoms()) {
        const bool suitable = rom.supportsMachine(machine);
        QString label = QStringLiteral("TOS %1 — %2").arg(rom.versionText(), rom.fileName);
        if (!suitable) {
            // Still offered, but marked: refusing outright would be wrong for a
            // ROM version we cannot read, and hiding it would strand a user whose
            // only ROM is one this build does not recognise.
            label = tr("%1  (not for %2)").arg(label, machineDisplayName(machine));
        }
        m_rom->addItem(label, rom.path);
    }

    // Restore the configured ROM if it is still present.
    const QString configured = m_settings.tosPath;
    if (!configured.isEmpty()) {
        const int index = m_rom->findData(configured);
        if (index >= 0)
            m_rom->setCurrentIndex(index);
        else {
            m_rom->addItem(tr("Configured: %1").arg(configured), configured);
            m_rom->setCurrentIndex(m_rom->count() - 1);
        }
    }

    updateCompatibilityNote();
}

void SettingsDialog::updateCompatibilityNote()
{
    const Machine machine = static_cast<Machine>(m_machine->currentData().toInt());

    QString note = tr("Needs %1.").arg(requiredTosHint(machine));

    // Refuse nothing here, but say plainly what Hatari will do with a mismatch,
    // because its own behaviour is to silently switch the machine.
    const QString romPath = m_rom->currentData().toString();
    if (!romPath.isEmpty()) {
        for (const TosRom &rom : findTosRoms()) {
            if (rom.path != romPath)
                continue;
            if (!rom.supportsMachine(machine)) {
                note = tr("%1\n\nThis ROM is not for the %2: Hatari will override the machine "
                          "to match the ROM, so the emulated machine will not be the one "
                          "selected here.")
                           .arg(note, machineDisplayName(machine));
            } else if (rom.versionKnown && !rom.supportsAutostart()) {
                note = tr("%1\n\nThis ROM is too old to autostart a program from the GEMDOS "
                          "hard disk (TOS 1.04 or later is needed), so Run will not attach "
                          "the debugger.")
                           .arg(note);
            }
            break;
        }
    }

    m_romNote->setText(note);
}

void SettingsDialog::onMachineChanged()
{
    refreshRomList();
}

void SettingsDialog::browseRom()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select a TOS ROM image"), QString(),
        tr("ROM images (*.img *.rom);;All files (*)"));
    if (path.isEmpty())
        return;

    m_rom->addItem(tr("Selected: %1").arg(path), path);
    m_rom->setCurrentIndex(m_rom->count() - 1);
    updateCompatibilityNote();
}

void SettingsDialog::browseHardDisk()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select a hard disk image"), QString(),
        tr("Disk images (*.img *.hd *.vhd *.st);;All files (*)"));
    if (!path.isEmpty())
        m_hardDisk->setText(path);
}

void SettingsDialog::addIncludePath()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Add include path"));
    if (!dir.isEmpty())
        m_includePaths->addItem(dir);
}

void SettingsDialog::removeIncludePath()
{
    delete m_includePaths->takeItem(m_includePaths->currentRow());
}

void SettingsDialog::addDefine()
{
    bool accepted = false;
    const QString define = QInputDialog::getText(
        this, tr("Add define"),
        tr("Symbol to define, as NAME or NAME=value. No -D prefix."),
        QLineEdit::Normal, QString(), &accepted);
    if (accepted && !define.trimmed().isEmpty())
        m_defines->addItem(define.trimmed());
}

void SettingsDialog::removeDefine()
{
    delete m_defines->takeItem(m_defines->currentRow());
}

ProjectSettings SettingsDialog::settings() const
{
    ProjectSettings s = m_settings;

    s.cpu = m_cpu->currentText();

    s.includePaths.clear();
    for (int i = 0; i < m_includePaths->count(); ++i)
        s.includePaths.append(m_includePaths->item(i)->text());

    s.defines.clear();
    for (int i = 0; i < m_defines->count(); ++i)
        s.defines.append(m_defines->item(i)->text());

    s.extraBuildArgs = linesToArgs(m_extraBuildArgs->toPlainText());

    s.machine = static_cast<Machine>(m_machine->currentData().toInt());
    s.tosPath = m_rom->currentData().toString();
    s.monitor = m_monitor->currentText();
    s.memSizeMiB = m_ram->value();
    s.hardDiskImage = m_hardDisk->text().trimmed();

    s.extraEmulatorArgs = linesToArgs(m_extraEmuArgs->toPlainText());

    return s;
}

} // namespace pist
