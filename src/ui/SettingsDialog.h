// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "project/ProjectSettings.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;

namespace pist {

/// Project settings editor.
///
/// Functional first: every control does something and the values reach the
/// build and the emulator command line. Visual refinement comes later.
///
/// The machine and ROM controls are deliberately linked. Hatari overrides
/// `--machine` to match the ROM and reports it only as an error log line
/// (docs/PLAN.md §5 rule 3), so the dialog shows which ROMs suit the chosen
/// machine and warns on a mismatch rather than letting the user pick a pairing
/// that silently becomes something else.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(const ProjectSettings &settings, QWidget *parent = nullptr);

    ProjectSettings settings() const;

private slots:
    void onMachineChanged();
    void browseRom();
    void browseHardDisk();
    void browseFloppyA();
    void browseFloppyB();
    void addIncludePath();
    void removeIncludePath();
    void addDefine();
    void removeDefine();

private:
    void buildUi();
    void loadValues(const ProjectSettings &settings);
    void refreshRomList();
    void updateCompatibilityNote();

    QComboBox *m_machine = nullptr;
    QComboBox *m_monitor = nullptr;
    QComboBox *m_cpu = nullptr;
    QSpinBox *m_ram = nullptr;
    QComboBox *m_rom = nullptr;
    QLabel *m_romNote = nullptr;
    QLineEdit *m_hardDisk = nullptr;
    QLineEdit *m_floppyA = nullptr;
    QLineEdit *m_floppyB = nullptr;
    QListWidget *m_includePaths = nullptr;
    QListWidget *m_defines = nullptr;
    QPlainTextEdit *m_extraBuildArgs = nullptr;
    QPlainTextEdit *m_extraEmuArgs = nullptr;

    ProjectSettings m_settings;
};

} // namespace pist
