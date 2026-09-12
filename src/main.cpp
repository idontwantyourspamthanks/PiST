// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("pist"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("pist"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("An IDE for Atari ST assembly development"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("source"),
                                 QStringLiteral("Assembly source file to open."));
    parser.process(app);

    pist::MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        QMetaObject::invokeMethod(&window, "openFile", Qt::QueuedConnection);

    return app.exec();
}
