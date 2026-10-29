// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SettingsDialog.h"

#include "ui/AssetExportDialog.h"

#include "emu/TosRom.h"
#include "support/CrashReport.h"
#include "ui/Appearance.h"
#include "ui/FileBrowser.h"
#include "ui/GalleryCard.h"
#include "ui/SetupDialog.h"

#include "ui/EmuAudio.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QMediaDevices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

/// Data role holding a ROM's absolute path on a combo entry.
constexpr int kRomPathRole = Qt::UserRole + 1;

/// Height ceiling for the Build tab's three lists (additional sources, include
/// paths, defines). Each list scrolls on its own, so a fixed slice keeps the
/// page's size hint — and with it the dialog's preferred height — inside a
/// 620x520 window instead of growing with the entries.
constexpr int kBuildListMaxHeight = 100;

/// `page` as a tab: its content wrapped in a frameless, transparent scroll
/// area, so the dialog's minimum height comes from its own chrome (tab bar,
/// setup button, OK/Cancel) rather than from the tallest page. Unbounded, the
/// Build page's word-wrapped notes and lists pushed the floor to 611 px — a
/// 768 px screen at 150% scaling cannot show that, and Qt then re-applied the
/// over-tall minimum on every relayout, moving the window and dropping the
/// button box below its bottom edge. The word-wrapped labels still wrap:
/// their heightForWidth now feeds the scroll area's content, not the dialog.
QWidget *scrollableTab(QWidget *page)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setAttribute(Qt::WA_TranslucentBackground);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(page);
    return scroll;
}

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

/// One "arguments field + Reset to defaults" row of `form`: the field takes the
/// width, a Reset button sits at its top right and restores @p defaults, one
/// argument per line. The two assembler-argument rows differ only in their
/// defaults.
void addArgsRow(QFormLayout *form, QWidget *tab, const QString &label, QPlainTextEdit *field,
                const QStringList &defaults)
{
    field->setPlaceholderText(SettingsDialog::tr("One argument per line"));
    field->setMaximumHeight(70);
    auto *row = new QWidget(tab);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(field, 1);
    auto *reset = new QPushButton(SettingsDialog::tr("Reset"), row);
    reset->setToolTip(SettingsDialog::tr("Restore these to PiST's default arguments"));
    rowLayout->addWidget(reset, 0, Qt::AlignTop);
    QObject::connect(reset, &QPushButton::clicked, field, [field, defaults] {
        field->setPlainText(defaults.join(QLatin1Char('\n')));
    });
    form->addRow(label, row);
}

/// A quiet wrapped sentence under a row. Palette ink, so a local stylesheet
/// does not throw away the rest of the chrome on this one label.
void quietNote(QLabel *label)
{
    label->setWordWrap(true);
    QPalette ink = label->palette();
    ink.setColor(QPalette::WindowText, appearance::colors().muted);
    label->setPalette(ink);
}

QGroupBox *formSection(const QString &title, QWidget *page, QFormLayout **formOut)
{
    auto *box = new QGroupBox(title, page);
    auto *form = new QFormLayout(box);
    form->setContentsMargins(16, 18, 16, 14);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    *formOut = form;
    return box;
}

/// Cards in combo order. The combo stays the value the dialog saves; the cards
/// are how it is chosen.
void wireChoiceCards(QComboBox *combo, const QList<GalleryCard *> &cards)
{
    auto sync = [combo, cards] {
        const int current = combo->currentIndex();
        for (int i = 0; i < cards.size(); ++i)
            cards.at(i)->setSelected(i == current);
    };
    for (int i = 0; i < cards.size(); ++i) {
        QObject::connect(cards.at(i), &GalleryCard::activated, combo, [combo, i] {
            combo->setCurrentIndex(i);
        });
    }
    QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [sync](int) { sync(); });
    sync();
}

} // namespace

SettingsDialog::SettingsDialog(const ProjectSettings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Settings"));
    buildUi();
    loadValues(settings);

    // Preferred 800x640 — but never more window than the screen has room for.
    // A fixed request on a 768 px screen at 125–150% scaling asks for more
    // height than the work area holds, so the window manager clamps the frame
    // while the layout keeps demanding its minimum: whatever is asked for must
    // be what the dialog actually is. The clamp never drops below the layout's
    // bounded minimum, and a normal desktop keeps the larger window.
    QSize preferred(800, 640);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        preferred.setWidth(qMin(preferred.width(), available.width() * 9 / 10));
        preferred.setHeight(qMin(preferred.height(), available.height() * 85 / 100));
    }
    const QSize needed = minimumSizeHint();
    preferred.setWidth(qMax(preferred.width(), needed.width()));
    preferred.setHeight(qMax(preferred.height(), needed.height()));
    resize(preferred);
}

void SettingsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(14);

    auto *heading = new QLabel(tr("Settings"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto *intro = new QLabel(
        tr("The project, the machine it runs on, and how PiST looks."), this);
    quietNote(intro);
    layout->addWidget(intro);

    auto *tabs = new QTabWidget(this);

    auto pageOf = [](QWidget *tab) {
        auto *page = new QVBoxLayout(tab);
        page->setContentsMargins(8, 8, 8, 8);
        page->setSpacing(12);
        return page;
    };

    // ---------------------------------------------------------------- build
    auto *buildTab = new QWidget(this);
    auto *buildPage = pageOf(buildTab);

    QFormLayout *assemblerForm = nullptr;
    buildPage->addWidget(formSection(tr("Assembler"), buildTab, &assemblerForm));

    // Explicit tool paths. Placed first because a missing assembler blocks
    // everything else, and the hint in the error dialog points here.
    m_assemblerPath = new QLineEdit;
    m_assemblerPath->setPlaceholderText(tr("Detected automatically"));
    connect(addPathRow(assemblerForm, buildTab, tr("vasmm68k_mot:"), m_assemblerPath),
            &QPushButton::clicked, this, &SettingsDialog::browseAssembler);

    m_cpu = new QComboBox(buildTab);
    // Taken from vasm's documented -m values. The ST is a plain 68000; the rest
    // are here because vasm accepts them and some projects target accelerators.
    m_cpu->addItems({QStringLiteral("68000"), QStringLiteral("68010"),
                     QStringLiteral("68020"), QStringLiteral("68030"),
                     QStringLiteral("68040"), QStringLiteral("68060")});
    assemblerForm->addRow(tr("CPU:"), m_cpu);

    QFormLayout *modulesForm = nullptr;
    buildPage->addWidget(formSection(tr("Modules"), buildTab, &modulesForm));

    // Additional sources. More than one switches the build to separate
    // compilation, so the note explains the ordering rule that TOS imposes.
    m_sources = new QListWidget(buildTab);
    m_sources->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sources->setMaximumHeight(kBuildListMaxHeight);

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
    quietNote(srcNote);
    srcLayout->addWidget(srcNote);
    modulesForm->addRow(tr("Additional sources:"), srcBox);

    // Include paths
    m_includePaths = new QListWidget(buildTab);
    m_includePaths->setSelectionMode(QAbstractItemView::SingleSelection);
    m_includePaths->setMaximumHeight(kBuildListMaxHeight);
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
    modulesForm->addRow(tr("Include paths:"), incBox);

    // Defines
    m_defines = new QListWidget(buildTab);
    m_defines->setMaximumHeight(kBuildListMaxHeight);
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
    modulesForm->addRow(tr("Defines:"), defBox);

    QFormLayout *assetForm = nullptr;
    buildPage->addWidget(formSection(tr("Sprite assets"), buildTab, &assetForm));
    m_assets = new QListWidget(buildTab);
    m_assets->setObjectName(QStringLiteral("spriteAssets"));
    m_assets->setSelectionMode(QAbstractItemView::SingleSelection);
    m_assets->setMaximumHeight(kBuildListMaxHeight);
    auto *assetButtons = new QHBoxLayout;
    auto *addAsset = new QPushButton(tr("Add…"), buildTab);
    auto *editAssetButton = new QPushButton(tr("Edit…"), buildTab);
    auto *removeAsset = new QPushButton(tr("Remove"), buildTab);
    assetButtons->addWidget(addAsset);
    assetButtons->addWidget(editAssetButton);
    assetButtons->addWidget(removeAsset);
    assetButtons->addStretch(1);
    connect(addAsset, &QPushButton::clicked, this, &SettingsDialog::addAsset);
    connect(editAssetButton, &QPushButton::clicked, this, &SettingsDialog::editAsset);
    connect(removeAsset, &QPushButton::clicked, this, &SettingsDialog::removeAsset);
    connect(m_assets, &QListWidget::itemDoubleClicked, this, [this] { editAsset(); });
    auto *assetBox = new QWidget(buildTab);
    auto *assetLayout = new QVBoxLayout(assetBox);
    assetLayout->setContentsMargins(0, 0, 0, 0);
    assetLayout->addWidget(m_assets);
    assetLayout->addLayout(assetButtons);
    auto *assetNote = new QLabel(
        tr("Each row is exported into the project's build directory before assemble, "
           "as a .dat and an include of the same name. Add include \"name.i\" to a "
           "source. That directory is on the include path for the build only, and a "
           "failed export keeps the previous files."),
        buildTab);
    quietNote(assetNote);
    assetLayout->addWidget(assetNote);
    assetForm->addRow(tr("On build:"), assetBox);

    QFormLayout *argsForm = nullptr;
    buildPage->addWidget(formSection(tr("Arguments"), buildTab, &argsForm));

    // Assembler arguments, one list per build mode, each editable with its own
    // reset. PiST adds the flags that describe *this* build — the output format,
    // the include paths and defines, the listing, -o and the source file —
    // after these. Both defaults use -devpac; the two lists exist because
    // -devpac drops the symbol table the debugger reads, so the debug list adds
    // -opt-x+ to put it back.
    m_debugBuildArgs = new QPlainTextEdit(buildTab);
    addArgsRow(argsForm, buildTab, tr("Debug assembler args:"), m_debugBuildArgs,
               defaultDebugAssemblerArgs());

    m_releaseBuildArgs = new QPlainTextEdit(buildTab);
    addArgsRow(argsForm, buildTab, tr("Release assembler args:"), m_releaseBuildArgs,
               defaultReleaseAssemblerArgs());

    auto *argsNote = new QLabel(
        tr("PiST adds the output format (-Ftos, or -Fvobj per module for a linked "
           "build), the include paths, defines, listing, -o and the source file "
           "after these. Both defaults use -devpac (Devpac compatibility, "
           "optimisations off) so one source assembles the same either way; the "
           "debug list adds -opt-x+ to keep the symbol table the debugger reads, "
           "and the release list strips it (-nosym) for a clean PRG."),
        buildTab);
    quietNote(argsNote);
    argsForm->addRow(QString(), argsNote);
    buildPage->addStretch(1);

    tabs->addTab(scrollableTab(buildTab), tr("Build"));

    // ------------------------------------------------------------- emulator
    auto *emuTab = new QWidget(this);
    auto *emuPage = pageOf(emuTab);

    QFormLayout *programForm = nullptr;
    emuPage->addWidget(formSection(tr("Program"), emuTab, &programForm));

    m_emulatorPath = new QLineEdit;
    m_emulatorPath->setPlaceholderText(tr("Detected automatically"));
    connect(addPathRow(programForm, emuTab, tr("hatari:"), m_emulatorPath),
            &QPushButton::clicked, this, &SettingsDialog::browseEmulator);

    m_debugBackend = new QComboBox(emuTab);
    m_debugBackend->addItem(tr("Auto (follow the emulator's capability)"),
                            QStringLiteral("auto"));
    m_debugBackend->addItem(tr("Native (stock Hatari)"), QStringLiteral("native"));
    m_debugBackend->addItem(tr("HRDB (TCP remote debugging)"), QStringLiteral("hrdb"));
    programForm->addRow(tr("Debug transport:"), m_debugBackend);

    QFormLayout *machineForm = nullptr;
    emuPage->addWidget(formSection(tr("Machine"), emuTab, &machineForm));

    m_machine = new QComboBox(emuTab);
    for (Machine machine : allMachines())
        m_machine->addItem(machineDisplayName(machine), static_cast<int>(machine));
    connect(m_machine, &QComboBox::currentIndexChanged, this, &SettingsDialog::onMachineChanged);
    machineForm->addRow(tr("Machine:"), m_machine);

    // ROM: a combo plus a browse button, because a user's ROMs may live anywhere.
    m_rom = new QComboBox;
    m_rom->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(addPathRow(machineForm, emuTab, tr("TOS ROM:"), m_rom),
            &QPushButton::clicked, this, &SettingsDialog::browseRom);

    m_romNote = new QLabel(emuTab);
    quietNote(m_romNote);
    machineForm->addRow(QString(), m_romNote);

    m_monitor = new QComboBox(emuTab);
    m_monitor->addItems({QStringLiteral("mono"), QStringLiteral("rgb"),
                         QStringLiteral("vga"), QStringLiteral("tv")});
    machineForm->addRow(tr("Monitor:"), m_monitor);

    m_ram = new QSpinBox(emuTab);
    m_ram->setRange(0, 14);
    m_ram->setSuffix(tr(" MiB"));
    m_ram->setSpecialValueText(tr("512 KiB (default)"));
    machineForm->addRow(tr("ST RAM:"), m_ram);

    QFormLayout *diskForm = nullptr;
    emuPage->addWidget(formSection(tr("Disks"), emuTab, &diskForm));

    m_hardDisk = new QLineEdit;
    m_hardDisk->setPlaceholderText(tr("Optional disk image for ACSI/IDE"));
    connect(addPathRow(diskForm, emuTab, tr("Hard disk:"), m_hardDisk),
            &QPushButton::clicked, this, &SettingsDialog::browseHardDisk);

    // Floppy drives. Both are optional and independent of the GEMDOS hard disk:
    // a program launched from the HD can still read a floppy, and a disk image is
    // the only way to ship data a program expects on A: or B:.
    m_floppyA = new QLineEdit;
    m_floppyA->setPlaceholderText(tr("Optional disk image for drive A:"));
    connect(addPathRow(diskForm, emuTab, tr("Floppy A:"), m_floppyA), &QPushButton::clicked,
            this, [this] { browseFloppy(m_floppyA, tr("Select a floppy image for drive A:")); });

    m_floppyB = new QLineEdit;
    m_floppyB->setPlaceholderText(tr("Optional disk image for drive B:"));
    connect(addPathRow(diskForm, emuTab, tr("Floppy B:"), m_floppyB), &QPushButton::clicked,
            this, [this] { browseFloppy(m_floppyB, tr("Select a floppy image for drive B:")); });

    m_extraEmuArgs = new QPlainTextEdit(emuTab);
    m_extraEmuArgs->setPlaceholderText(tr("One argument per line"));
    m_extraEmuArgs->setMaximumHeight(70);
    diskForm->addRow(tr("Extra emulator args:"), m_extraEmuArgs);
    emuPage->addStretch(1);

    tabs->addTab(scrollableTab(emuTab), tr("Emulator"));

    // ------------------------------------------------------------ appearance
    // Application-wide preferences (QSettings), deliberately not part of the
    // project file: a theme, a font and a size are the user's taste, not the
    // project's.
    auto *appearanceTab = new QWidget(this);
    auto *appearancePage = pageOf(appearanceTab);

    auto *scopeNote = new QLabel(
        tr("These are application-wide preferences and are not stored in the project file."),
        appearanceTab);
    quietNote(scopeNote);
    appearancePage->addWidget(scopeNote);

    m_theme = new QComboBox(appearanceTab);
    m_theme->addItem(tr("System"), QStringLiteral("system"));
    m_theme->addItem(tr("Light"), QStringLiteral("light"));
    m_theme->addItem(tr("Dark"), QStringLiteral("dark"));
    const int themeIndex = m_theme->findData(appearance::theme());
    if (themeIndex >= 0)
        m_theme->setCurrentIndex(themeIndex);
    m_theme->hide();

    auto *themeLabel = new QLabel(tr("Theme"), appearanceTab);
    QFont sectionFont = themeLabel->font();
    sectionFont.setBold(true);
    themeLabel->setFont(sectionFont);
    appearancePage->addWidget(themeLabel);

    auto *systemCard = new GalleryCard(appearanceTab);
    systemCard->setTitle(tr("System"));
    systemCard->setDetail(tr("Follow the desktop."));
    systemCard->setMinimumHeight(104);
    auto *lightCard = new GalleryCard(appearanceTab);
    lightCard->setTitle(tr("Light"));
    lightCard->setDetail(tr("Paper, with the GEM green."));
    lightCard->setMinimumHeight(104);
    auto *darkCard = new GalleryCard(appearanceTab);
    darkCard->setTitle(tr("Dark"));
    darkCard->setDetail(tr("The default. A dark desk."));
    darkCard->setMinimumHeight(104);
    auto *themeRow = new QHBoxLayout;
    themeRow->setSpacing(10);
    themeRow->addWidget(systemCard);
    themeRow->addWidget(lightCard);
    themeRow->addWidget(darkCard);
    appearancePage->addLayout(themeRow);
    wireChoiceCards(m_theme, {systemCard, lightCard, darkCard});

    QFormLayout *appearanceLayout = nullptr;
    appearancePage->addWidget(formSection(tr("Editor"), appearanceTab, &appearanceLayout));

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
    m_shortcutScheme->hide();

    auto *keysLabel = new QLabel(tr("Shortcuts"), appearanceTab);
    keysLabel->setFont(sectionFont);
    appearancePage->addWidget(keysLabel);
    auto *pistCard = new GalleryCard(appearanceTab);
    pistCard->setTitle(tr("PiST"));
    pistCard->setDetail(tr("F8 toggles a breakpoint, F9 continues, F10 steps into."));
    pistCard->setMinimumHeight(104);
    auto *commonCard = new GalleryCard(appearanceTab);
    commonCard->setTitle(tr("Common"));
    commonCard->setDetail(tr("F5 continues, F9 toggles a breakpoint, F10 steps over."));
    commonCard->setMinimumHeight(104);
    auto *keysRow = new QHBoxLayout;
    keysRow->setSpacing(10);
    keysRow->addWidget(pistCard);
    keysRow->addWidget(commonCard);
    appearancePage->addLayout(keysRow);
    wireChoiceCards(m_shortcutScheme, {pistCard, commonCard});

    QFormLayout *soundForm = nullptr;
    appearancePage->addWidget(formSection(tr("Sound"), appearanceTab, &soundForm));

    m_audioOutput = new QComboBox(appearanceTab);
    m_audioOutput->setObjectName(QStringLiteral("audioOutput"));
    m_audioOutput->addItem(tr("System default"), QByteArray());
    {
        const QByteArray current =
            QSettings().value(QLatin1String(pist::kAudioOutputDeviceKey)).toByteArray();
        int index = 0;
        const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
        for (int i = 0; i < devices.size(); ++i) {
            m_audioOutput->addItem(devices[i].description(), devices[i].id());
            if (devices[i].id() == current)
                index = i + 1;
        }
        m_audioOutput->setCurrentIndex(index);
    }
    soundForm->addRow(tr("Audio output:"), m_audioOutput);

    QFormLayout *reportForm = nullptr;
    appearancePage->addWidget(formSection(tr("Crash reports"), appearanceTab, &reportForm));
    m_crashReports = new QCheckBox(
        tr("Send a stack and the session phase when PiST crashes"), appearanceTab);
    m_crashReports->setObjectName(QStringLiteral("crashReports"));
    m_crashReports->setChecked(crash::reportsEnabled());
    reportForm->addRow(m_crashReports);
    appearancePage->addStretch(1);

    tabs->addTab(scrollableTab(appearanceTab), tr("Appearance"));

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

    for (QLineEdit *edit : findChildren<QLineEdit *>())
        edit->setMinimumHeight(30);
    for (QComboBox *box : findChildren<QComboBox *>()) {
        if (!box->isHidden())
            box->setMinimumHeight(30);
    }
    for (QSpinBox *spin : findChildren<QSpinBox *>())
        spin->setMinimumHeight(30);
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
    QSettings().setValue(QLatin1String(pist::kAudioOutputDeviceKey),
                         m_audioOutput->currentData().toByteArray());
    crash::setReportsEnabled(m_crashReports->isChecked());
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
    m_assetExports = settings.assets;
    refreshAssetList();

    m_debugBuildArgs->setPlainText(settings.debugAssemblerArgs.join(QLatin1Char('\n')));
    m_releaseBuildArgs->setPlainText(settings.releaseAssemblerArgs.join(QLatin1Char('\n')));

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

void SettingsDialog::refreshAssetList()
{
    m_assets->clear();
    for (const AssetExport &asset : m_assetExports) {
        const QString file = QFileInfo(asset.source).fileName();
        m_assets->addItem(tr("%1  %2  →  %3").arg(file, asset.phase, asset.output));
    }
}

void SettingsDialog::addAsset()
{
    AssetExport asset;
    if (!editAssetExport(this, &asset))
        return;
    m_assetExports.append(asset);
    refreshAssetList();
    m_assets->setCurrentRow(m_assets->count() - 1);
}

void SettingsDialog::editAsset()
{
    const int row = m_assets->currentRow();
    if (row < 0 || row >= m_assetExports.size())
        return;
    AssetExport asset = m_assetExports.at(row);
    if (!editAssetExport(this, &asset))
        return;
    m_assetExports[row] = asset;
    refreshAssetList();
    m_assets->setCurrentRow(row);
}

void SettingsDialog::removeAsset()
{
    const int row = m_assets->currentRow();
    if (row < 0 || row >= m_assetExports.size())
        return;
    m_assetExports.removeAt(row);
    refreshAssetList();
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
    s.assets = m_assetExports;

    s.debugAssemblerArgs = linesToArgs(m_debugBuildArgs->toPlainText());
    s.releaseAssemblerArgs = linesToArgs(m_releaseBuildArgs->toPlainText());

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
