// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The crash record is the body the site will store. A field that is not on the
// receiver's allowlist must not be here to be dropped later, and a breadcrumb
// must stay one short line so a path or a source line cannot ride along.

#include "support/CrashReport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QtTest>

using namespace pist;

class tst_crash : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void record_is_the_allowlisted_body();
    void breadcrumb_is_one_line();
    void junk_is_not_a_report();
    void reports_stay_off_until_asked();
};

void tst_crash::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("PiSTCrashTest"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_crash"));
}

void tst_crash::cleanup()
{
    crash::clearNotes();
    QSettings().remove(QLatin1String(crash::kSettingsKey));
}

void tst_crash::record_is_the_allowlisted_body()
{
    crash::note("build");
    crash::note("run");

    QByteArray json;
    QString error;
    QVERIFY2(crash::parseRecord(crash::formatRecord(QStringLiteral("SIGSEGV"),
                                                    "pist::crash::arm\n"),
                                &json, &error),
             qPrintable(error));

    const QJsonObject body = QJsonDocument::fromJson(json).object();
    const QStringList keys = body.keys();
    for (const QString &key : keys) {
        QVERIFY2(key == QLatin1String("version") || key == QLatin1String("os")
                     || key == QLatin1String("arch") || key == QLatin1String("package")
                     || key == QLatin1String("qt") || key == QLatin1String("kind")
                     || key == QLatin1String("signal") || key == QLatin1String("stack")
                     || key == QLatin1String("breadcrumbs") || key == QLatin1String("hatari_version")
                     || key == QLatin1String("last_command") || key == QLatin1String("build_id"),
                 qPrintable(key));
    }
    QCOMPARE(body.value(QStringLiteral("kind")).toString(), QStringLiteral("ide"));
    QCOMPARE(body.value(QStringLiteral("signal")).toString(), QStringLiteral("SIGSEGV"));
    QCOMPARE(body.value(QStringLiteral("version")).toString(), QStringLiteral(PIST_VERSION));
    QVERIFY(body.value(QStringLiteral("stack")).toString().contains(QStringLiteral("pist::crash::arm")));
    QCOMPARE(body.value(QStringLiteral("breadcrumbs")).toArray().size(), 2);
    QCOMPARE(crash::kEndpoint, "https://pistapi.kswapps.com/api/crash-reports");
}

void tst_crash::breadcrumb_is_one_line()
{
    crash::note("build\n/home/someone/secret.s");
    QByteArray json;
    QVERIFY(crash::parseRecord(crash::formatRecord(QString(), QByteArrayLiteral("frame\n")),
                               &json, nullptr));
    const QJsonArray crumbs = QJsonDocument::fromJson(json).object()
                                  .value(QStringLiteral("breadcrumbs")).toArray();
    QCOMPARE(crumbs.size(), 1);
    QCOMPARE(crumbs.at(0).toString(), QStringLiteral("build /home/someone/secret.s"));
    QVERIFY(!json.contains("\"source\""));
}

void tst_crash::junk_is_not_a_report()
{
    QString error;
    QVERIFY(!crash::parseRecord(QByteArrayLiteral("hello\n"), nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void tst_crash::reports_stay_off_until_asked()
{
    QVERIFY(!crash::reportsEnabled());
    crash::setReportsEnabled(true);
    QVERIFY(crash::reportsEnabled());
}

QTEST_GUILESS_MAIN(tst_crash)
#include "tst_crash.moc"
