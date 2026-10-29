// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "project/ProjectSettings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace pist {

class ImageDocument;

/// Edit one declared sprite export. `asset->source` empty asks for a `.pim`
/// first. Returns false when the user cancels or the document cannot be read.
bool editAssetExport(QWidget *parent, AssetExport *asset);

} // namespace pist
