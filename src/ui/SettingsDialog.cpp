// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SettingsDialog.h"

#include "emu/TosRom.h"
#include "ui/Appearance.h"
#include "ui/FileBrowser.h"
#include "ui/SetupDialog.h"


#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
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

/// One "value field + Browse… button" row of `form`: `field` (a QLineEdit or a
/// QComboBox) takes the space on the left, a Browse… button sits on the right.
/// The six path rows — assembler, emulator, TOS ROM, hard disk, floppy A/B —
/// had this construction written out by hand. The row takes ownership of
/// `field`; the button is returned so the caller can connect it.
QPushButton *addPathRow(QFormLayout *form, QWidget *tab, const QString &label, QWidget *field)
{
    auto *row = new QWidget(tab);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(field, 1);
    auto *browse = new QPushButton(SettingsDialog::tr("Browse…"), row);
    rowLayout->addWidget(browse);
    form->addRow(label, row);
    return browse;
}

} // namespace

SettingsDialog::SettingsDialog(const ProjectSettings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Settings"));
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

    // Explicit tool paths. Placed first because a missing assembler blocks
    // everything else, and the hint in the error dialog points here.
    m_assemblerPath = new QLineEdit;
    m_assemblerPath->setPlaceholderText(tr("Detected automatically"));
    connect(addPathRow(buildLayout, buildTab, tr("vasmm68k_mot:"), m_assemblerPath),
            &QPushButton::clicked, this, &SettingsDialog::browseAssembler);

    m_cpu = new QComboBox(buildTab);
    // Taken from vasm's documented -m values. The ST is a plain 68000; the rest
    // are here because vasm accepts them and some projects target accelerators.
    m_cpu->addItems({QStringLiteral("68000"), QStringLiteral("68010"),
                     QStringLiteral("68020"), QStringLiteral("68030"),
                     QStringLiteral("68040"), QStringLiteral("68060")});
    buildLayout->addRow(tr("CPU:"), m_cpu);

    // Additional sources. More than one switches the build to separate
    // compilation, so the note explains the ordering rule that TOS imposes.
    m_sources = new QListWidget(buildTab);
    m_sources->setSelectionMode(QAbstractItemView::SingleSelection);

    auto *srcButtons = new QHBoxLayout;
    auto *addSrc = new QPushButton(tr("Add file…"), buildTab);
    auto *removeSrc = new QPushButton(tr("Remove"), buildTab);
    srcButtons->addWidget(addSrc);
    srcButtons->addWidget(removeSrc);
    srcButtons->addStretch(1);
    connect(addSrc, &QPushButton::clicked, this, &SettingsDialog::addSource);
    connect(removeSrc, &QPushButton::clicked, this, &SettingsDialog::removeSource);

    auto *srcBox = new QWidget(buildTab);
    auto *srcLayout = new QVBoxLayout(srcBox);
    srcLayout->setContentsMargins(0, 0, 0, 0);
    srcLayout->addWidget(m_sources);
    srcLayout->addLayout(srcButtons);
    auto *srcNote = new QLabel(
        tr("Linked into the program after the file being edited, which is the entry "
           "module — TOS starts executing at the beginning of text, so that must come "
           "first. Adding any file here switches the build to separate compilation "
           "and needs vlink."),
        buildTab);
    srcNote->setWordWrap(true);
    srcNote->setStyleSheet(QStringLiteral("color: palette(mid);"));
    srcLayout->addWidget(srcNote);
    buildLayout->addRow(tr("Additional sources:"), srcBox);

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

    m_emulatorPath = new QLineEdit;
    m_emulatorPath->setPlaceholderText(tr("Detected automatically"));
    connect(addPathRow(emuLayout, emuTab, tr("hatari:"), m_emulatorPath),
            &QPushButton::clicked, this, &SettingsDialog::browseEmulator);

    m_debugBackend = new QComboBox(emuTab);
    m_debugBackend->addItem(tr("Auto (follow the emulator's capability)"),
                            QStringLiteral("auto"));
    m_debugBackend->addItem(tr("Native (stock Hatari)"), QStringLiteral("native"));
    m_debugBackend->addItem(tr("HRDB (TCP remote debugging)"), QStringLiteral("hrdb"));
    emuLayout->addRow(tr("Debug transport:"), m_debugBackend);

    m_machine = new QComboBox(emuTab);
    for (Machine machine : allMachines())
        m_machine->addItem(machineDisplayName(machine), static_cast<int>(machine));
    connect(m_machine, &QComboBox::currentIndexChanged, this, &SettingsDialog::onMachineChanged);
    emuLayout->addRow(tr("Machine:"), m_machine);

    // ROM: a combo plus a browse button, because a user's ROMs may live anywhere.
    m_rom = new QComboBox;
    m_rom->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(addPathRow(emuLayout, emuTab, tr("TOS ROM:"), m_rom),
            &QPushButton::clicked, this, &SettingsDialog::browseRom);

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

    m_hardDisk = new QLineEdit;
    m_hardDisk->setPlaceholderText(tr("Optional disk image for ACSI/IDE"));
    connect(addPathRow(emuLayout, emuTab, tr("Hard disk:"), m_hardDisk),
            &QPushButton::clicked, this, &SettingsDialog::browseHardDisk);

    // Floppy drives. Both are optional and independent of the GEMDOS hard disk:
    // a program launched from the HD can still read a floppy, and a disk image is
    // the only way to ship data a program expects on A: or B:.
    m_floppyA = new QLineEdit;
    m_floppyA->setPlaceholderText(tr("Optional disk image for drive A:"));
    connect(addPathRow(emuLayout, emuTab, tr("Floppy A:"), m_floppyA), &QPushButton::clicked,
            this, [this] { browseFloppy(m_floppyA, tr("Select a floppy image for drive A:")); });

    m_floppyB = new QLineEdit;
    m_floppyB->setPlaceholderText(tr("Optional disk image for drive B:"));
    connect(addPathRow(emuLayout, emuTab, tr("Floppy B:"), m_floppyB), &QPushButton::clicked,
            this, [this] { browseFloppy(m_floppyB, tr("Select a floppy image for drive B:")); });

    m_extraEmuArgs = new QPlainTextEdit(emuTab);
    m_extraEmuArgs->setPlaceholderText(tr("One argument per line"));
    m_extraEmuArgs->setMaximumHeight(70);
    emuLayout->addRow(tr("Extra emulator args:"), m_extraEmuArgs);

    tabs->addTab(emuTab, tr("Emulator"));

    // ------------------------------------------------------------ appearance
    // Application-wide preferences (QSettings), deliberately not part of the
    // project file: a theme, a font and a size are the user's taste, not the
    // project's.
    auto *appearanceTab = new QWidget(this);
    auto *appearanceLayout = new QFormLayout(appearanceTab);

    auto *scopeNote = new QLabel(
        tr("These are application-wide preferences and are not stored in the project file."),
        appearanceTab);
    scopeNote->setWordWrap(true);
    appearanceLayout->addRow(scopeNote);

    m_theme = new QComboBox(appearanceTab);
    m_theme->addItem(tr("System"), QStringLiteral("system"));
    m_theme->addItem(tr("Light"), QStringLiteral("light"));
    m_theme->addItem(tr("Dark"), QStringLiteral("dark"));
    const int themeIndex = m_theme->findData(appearance::theme());
    if (themeIndex >= 0)
        m_theme->setCurrentIndex(themeIndex);
    appearanceLayout->addRow(tr("Theme:"), m_theme);

    m_fontFamily = new QComboBox(appearanceTab);
    m_fontFamily->addItem(tr("Default"), QString());
    {
        const QStringList choices = appearance::editorFontChoices();
        for (const QString &family : choices)
            m_fontFamily->addItem(family, family);
        const QString current = appearance::editorFontFamily();
        int index = m_fontFamily->findData(current);
        if (index < 0 && !current.isEmpty()) {
            m_fontFamily->addItem(current, current);
            index = m_fontFamily->count() - 1;
        }
        if (index >= 0)
            m_fontFamily->setCurrentIndex(index);
    }
    appearanceLayout->addRow(tr("Editor font:"), m_fontFamily);

    m_fontSize = new QSpinBox(appearanceTab);
    m_fontSize->setRange(0, 48);
    m_fontSize->setSpecialValueText(tr("Default"));
    m_fontSize->setValue(appearance::editorPointSize());
    appearanceLayout->addRow(tr("Editor font size:"), m_fontSize);

    auto *sample = new QLabel(QStringLiteral("move.w #9,-(a7) ; Cconws"), appearanceTab);
    sample->setObjectName(QStringLiteral("editorFontSample"));
    auto refreshSample = [this, sample] {
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        const QString family = m_fontFamily->currentData().toString();
        if (!family.isEmpty()) {
            font = QFont(family);
            font.setStyleHint(QFont::TypeWriter);
            font.setFixedPitch(true);
        }
        const int size = m_fontSize->value();
        if (size > 0)
            font.setPointSize(size);
        sample->setFont(font);
    };
    connect(m_fontFamily, &QComboBox::currentIndexChanged, this, refreshSample);
    connect(m_fontSize, qOverload<int>(&QSpinBox::valueChanged), this, refreshSample);
    refreshSample();
    appearanceLayout->addRow(tr("Sample:"), sample);

    auto *tabWidth = new QLabel(QStringLiteral("8"), appearanceTab);
    tabWidth->setObjectName(QStringLiteral("tabWidthValue"));
    appearanceLayout->addRow(tr("Tab width:"), tabWidth);

    m_shortcutScheme = new QComboBox(appearanceTab);
    m_shortcutScheme->setObjectName(QStringLiteral("shortcutScheme"));
    m_shortcutScheme->addItem(tr("PiST (F10 step into, F9 continue)"), QStringLiteral("pist"));
    m_shortcutScheme->addItem(tr("Common (F10 step over, F11 step into)"), QStringLiteral("common"));
    m_shortcutScheme->setToolTip(
        tr("PiST is the default: F8 toggles a breakpoint, F9 continues, F10 steps "
           "into, F11 steps over. Common matches other IDEs and uses F5 to "
           "continue while stopped."));
    const int schemeIndex = m_shortcutScheme->findData(appearance::shortcutScheme());
    if (schemeIndex >= 0)
        m_shortcutScheme->setCurrentIndex(schemeIndex);
    appearanceLayout->addRow(tr("Shortcut scheme:"), m_shortcutScheme);

    tabs->addTab(appearanceTab, tr("Appearance"));

    layout->addWidget(tabs);

    auto *setup = new QPushButton(tr("Set up tools and ROMs…"), this);
    setup->setObjectName(QStringLiteral("setupToolsButton"));
    connect(setup, &QPushButton::clicked, this, [this] {
        SetupDialog dialog(this);
        dialog.exec();
        emit toolsSetupClosed();
    });
    layout->addWidget(setup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::accept()
{
    // Application-wide preferences persist on OK, independently of the
    // project settings the rest of the dialog edits. The keys come from
    // Appearance — the reader of each one — rather than being spelled again
    // here (MIN-53).
    QSettings().setValue(appearance::themeKey(), m_theme->currentData().toString());
    QSettings().setValue(appearance::fontFamilyKey(), m_fontFamily->currentData().toString());
    QSettings().setValue(appearance::fontSizeKey(), m_fontSize->value());
    QSettings().setValue(appearance::shortcutSchemeKey(),
                         m_shortcutScheme->currentData().toString());
    QDialog::accept();
}

void SettingsDialog::loadValues(const ProjectSettings &settings)
{
    m_assemblerPath->setText(settings.assemblerPath);
    m_emulatorPath->setText(settings.hatariPath);

    const int cpuIndex = m_cpu->findText(settings.cpu);
    if (cpuIndex >= 0)
        m_cpu->setCurrentIndex(cpuIndex);

    for (const QString &path : settings.additionalSources)
        m_sources->addItem(path);
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

    const int backendIndex = m_debugBackend->findData(settings.debugBackend);
    m_debugBackend->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);

    m_ram->setValue(settings.memSizeMiB);
    m_hardDisk->setText(settings.hardDiskImage);
    if (settings.floppyImages.size() > 0)
        m_floppyA->setText(settings.floppyImages.at(0));
    if (settings.floppyImages.size() > 1)
        m_floppyB->setText(settings.floppyImages.at(1));
    m_extraEmuArgs->setPlainText(settings.extraEmulatorArgs.join(QLatin1Char('\n')));

    refreshRomList();
}

void SettingsDialog::refreshRomList()
{
    const Machine machine = static_cast<Machine>(m_machine->currentData().toInt());

    // The combo is the live selection, and the stored settings only say what the
    // dialog was opened with: rebuilding the list from `m_settings.tosPath` threw
    // away whatever the user had picked (including a ROM chosen through Browse…),
    // and settings() then persisted the ROM from the file rather than the one on
    // screen. A valid current index means the user has a selection to keep — even
    // the empty data of "Automatic", which is a choice — while -1 only happens
    // before the list has been built once.
    const bool hadSelection = m_rom->currentIndex() >= 0;
    const QString picked = m_rom->currentData().toString();

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

    // Restore the selection if it is still present. A ROM that is not in the
    // discovered list (one the user browsed to) keeps its own entry, so the
    // choice survives the machine change that re-ran this.
    const QString keep = hadSelection ? picked : m_settings.tosPath;
    if (!keep.isEmpty()) {
        const int index = m_rom->findData(keep);
        if (index >= 0)
            m_rom->setCurrentIndex(index);
        else {
            m_rom->addItem(tr("Configured: %1").arg(keep), keep);
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

void SettingsDialog::browseAssembler()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select the vasm m68k Motorola-syntax assembler"),
        m_assemblerPath->text(), tr("All files (*)"));
    if (!path.isEmpty())
        m_assemblerPath->setText(path);
}

void SettingsDialog::browseEmulator()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select the Hatari executable"), m_emulatorPath->text(),
        tr("All files (*)"));
    if (!path.isEmpty())
        m_emulatorPath->setText(path);
}

void SettingsDialog::browseFloppy(QLineEdit *field, const QString &title)
{
    const QString path = QFileDialog::getOpenFileName(this, title, QString(),
                                                      floppyImageFilter());
    if (!path.isEmpty())
        field->setText(path);
}

void SettingsDialog::addSource()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add assembly sources"), QString(),
        tr("Assembly sources (*.s *.S *.asm *.x68);;All files (*)"));
    for (const QString &path : paths) {
        // A duplicate would be assembled twice and then clash at link time on
        // its own global symbols.
        bool already = false;
        for (int i = 0; i < m_sources->count(); ++i) {
            if (m_sources->item(i)->text() == path)
                already = true;
        }
        if (!already)
            m_sources->addItem(path);
    }
}

void SettingsDialog::removeSource()
{
    delete m_sources->takeItem(m_sources->currentRow());
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

    s.assemblerPath = m_assemblerPath->text().trimmed();
    s.hatariPath = m_emulatorPath->text().trimmed();
    s.cpu = m_cpu->currentText();

    s.additionalSources.clear();
    for (int i = 0; i < m_sources->count(); ++i)
        s.additionalSources.append(m_sources->item(i)->text());

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

    s.debugBackend = m_debugBackend->currentData().toString();

    s.floppyImages.clear();
    for (const QLineEdit *edit : {m_floppyA, m_floppyB})
        s.floppyImages.append(edit->text().trimmed());

    s.extraEmulatorArgs = linesToArgs(m_extraEmuArgs->toPlainText());

    return s;
}

} // namespace pist
