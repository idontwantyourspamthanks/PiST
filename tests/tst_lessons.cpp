// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

// The teaching layer's own tests: the lesson format's parser and the checkpoint
// evaluator against constructed snapshots (no IDE, no emulator), then every
// shipped lesson parsed, and finally the first lesson run end to end against a
// real session — because a lesson that stops being true is the failure mode
// this whole design exists to prevent (docs/tutorial-plan.md §7.2).

#include "editor/DiagnosticRef.h"
#include "lessons/Lesson.h"
#include "lessons/LessonRunner.h"
#include "ui/MainWindow.h"

#include "emu/TosRom.h"

#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;
using lessons::Checkpoint;
using lessons::Lesson;
using lessons::LessonRunner;
using lessons::Snapshot;

namespace {

/// Skip the calling test — or fail it under PIST_REQUIRE_EMULATOR — when the
/// emulator integration cannot run, exactly as tst_gui does: a skip is honest
/// on a developer's machine and a lie in CI, where the variable is set.
QString emulatorMissing()
{
    QStringList missing;
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        missing << QStringLiteral("hatari");
    const TosRom rom = selectPreferredRom(findTosRoms(), Machine::St);
    if (rom.path.isEmpty() || !rom.supportsAutostart())
        missing << QStringLiteral("an autostart-capable TOS ROM for an ST");
    return missing.join(QStringLiteral(", "));
}

const char *kGoodLesson = R"PIL(
id:      sample
title:   A sample
level:   first steps
needs:   assembler
learn:   nothing, it is a fixture
source:  template "GEMDOS hello" as sample.s

step  One
  teach  The first step, which wraps onto
         a second line of prose.
  do     build
  expect problems == none

step  Two
  teach  The second step checks a register.
  do     breakpoint 16
  expect stopped
  expect reg.d0 == $2a
)PIL";

} // namespace

class TstLessons : public QObject
{
    Q_OBJECT

private slots:
    void parsesALesson();
    void refusesMalformedLessons_data();
    void refusesMalformedLessons();
    void evaluatesCheckpoints_data();
    void evaluatesCheckpoints();
    void everyShippedLessonParses();
    void shippedLessonsNameTheirNeeds();
    void keyTokensResolveToTheBoundKey();
    void explainsTheDiagnosticsTheToolchainActuallyPrints();
    void aLessonHoldsAgainstARealSession_data();
    void aLessonHoldsAgainstARealSession();
};

void TstLessons::parsesALesson()
{
    Lesson lesson;
    QString error;
    QVERIFY2(lessons::parseLesson(QString::fromUtf8(kGoodLesson), QStringLiteral("/tmp/sample"),
                                 &lesson, &error),
             qPrintable(error));

    QCOMPARE(lesson.id, QStringLiteral("sample"));
    QCOMPARE(lesson.title, QStringLiteral("A sample"));
    QCOMPARE(lesson.level, QStringLiteral("first steps"));
    QCOMPARE(lesson.needs, QStringList{ QStringLiteral("assembler") });
    QCOMPARE(lesson.steps.size(), 2);

    // A source template is split into its label and the name it is created as.
    QCOMPARE(lesson.source.kind, lessons::SourceSpec::Template);
    QCOMPARE(lesson.source.value, QStringLiteral("GEMDOS hello"));
    QCOMPARE(lesson.source.as, QStringLiteral("sample.s"));

    // Wrapped prose is one paragraph, not two lines.
    QCOMPARE(lesson.steps.at(0).teach,
             QStringLiteral("The first step, which wraps onto a second line of prose."));
    QCOMPARE(lesson.steps.at(0).actions.size(), 1);
    QCOMPARE(lesson.steps.at(0).actions.at(0).verb, QStringLiteral("build"));
    QCOMPARE(lesson.steps.at(0).expects.size(), 1);

    QCOMPARE(lesson.steps.at(1).actions.at(0).verb, QStringLiteral("breakpoint"));
    QCOMPARE(lesson.steps.at(1).actions.at(0).args, QStringList{ QStringLiteral("16") });
    QCOMPARE(lesson.steps.at(1).expects.size(), 2);
    QCOMPARE(lesson.steps.at(1).expects.at(1).kind, Checkpoint::Register);
    QCOMPARE(lesson.steps.at(1).expects.at(1).subject, QStringLiteral("d0"));
    QCOMPARE(lesson.steps.at(1).expects.at(1).op, QStringLiteral("=="));

    // The id defaults to the directory when the file does not name one.
    Lesson unnamed;
    QVERIFY(lessons::parseLesson(QString::fromUtf8(kGoodLesson).replace(QStringLiteral("id:      sample\n"), QString()),
                                QStringLiteral("/tmp/from-dir"), &unnamed, &error));
    QCOMPARE(unnamed.id, QStringLiteral("from-dir"));
}

void TstLessons::refusesMalformedLessons_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("errorContains");

    const QString head = QStringLiteral("title: T\nlevel: L\nlearn: x\n\nstep S\n  teach t\n");

    // A lesson that does not parse is an error, not a lesson with fewer steps:
    // a silently dropped step is a lesson that teaches less than it claims.
    QTest::newRow("unknown key") << QStringLiteral("title: T\nlevl: L\nlearn: x\n\nstep S\n  teach t\n  do build\n")
                                 << QStringLiteral("not a lesson key");
    QTest::newRow("unknown verb") << (head + QStringLiteral("  do teleport\n")) << QStringLiteral("not a lesson verb");
    QTest::newRow("bad verb arity") << (head + QStringLiteral("  do goto\n")) << QStringLiteral("exactly 1 arguments");
    QTest::newRow("bad checkpoint") << (head + QStringLiteral("  expect reg.q7 == 1\n")) << QStringLiteral("not a register");
    QTest::newRow("bad operator") << (head + QStringLiteral("  expect reg.d0 =~ 1\n")) << QStringLiteral("not a comparison");
    QTest::newRow("bad need") << QStringLiteral("title: T\nlevel: L\nlearn: x\nneeds: emultor\n\nstep S\n  teach t\n  do build\n")
                              << QStringLiteral("is not a need");
    QTest::newRow("no outcome") << (head + QStringLiteral("  expect stopped\n\nstep T\n  teach t\n  do run\n"))
                                << QStringLiteral("observes something the runner cannot check");
    QTest::newRow("both outcome forms") << (head + QStringLiteral("  expect stopped\n  nominal nothing\n"))
                                        << QStringLiteral("has both expect and nominal");
    QTest::newRow("no teach") << QStringLiteral("title: T\nlevel: L\nlearn: x\n\nstep S\n  do build\n  expect stopped\n")
                              << QStringLiteral("has no teach line");
    QTest::newRow("nothing at all") << (head + QStringLiteral("  do build\n"))
                                    << QStringLiteral("observes something the runner cannot check");
    QTest::newRow("memory length mismatch") << (head + QStringLiteral("  expect mem 0x100 4 == de ad be\n"))
                                            << QStringLiteral("bytes were expected but 3 were written");
    QTest::newRow("missing learn") << QStringLiteral("title: T\nlevel: L\n\nstep S\n  teach t\n  do build\n")
                                   << QStringLiteral("needs a `learn:`");
    QTest::newRow("no steps") << QStringLiteral("title: T\nlevel: L\nlearn: x\n")
                              << QStringLiteral("has no steps");
}

void TstLessons::refusesMalformedLessons()
{
    QFETCH(QString, text);
    QFETCH(QString, errorContains);

    Lesson lesson;
    QString error;
    QVERIFY2(!lessons::parseLesson(text, QStringLiteral("/tmp/bad"), &lesson, &error),
             "this lesson file should have been refused");
    QVERIFY2(error.contains(errorContains),
             qPrintable(QStringLiteral("expected an error mentioning \"%1\", got \"%2\"")
                            .arg(errorContains, error)));
}

void TstLessons::evaluatesCheckpoints_data()
{
    QTest::addColumn<QString>("expectLine");
    QTest::addColumn<int>("problems");
    QTest::addColumn<bool>("passes");
    QTest::addColumn<QString>("detailContains");

    QTest::newRow("stopped") << QStringLiteral("expect stopped") << 0 << true << QString();
    QTest::newRow("running") << QStringLiteral("expect running") << 0 << false << QStringLiteral("stopped");
    QTest::newRow("no problems") << QStringLiteral("expect problems == none") << 0 << true << QString();
    QTest::newRow("a problem") << QStringLiteral("expect problems == none") << 1 << false
                               << QStringLiteral("problem(s)");
    QTest::newRow("register equal") << QStringLiteral("expect reg.d0 == 42") << 0 << true << QString();
    QTest::newRow("register not equal") << QStringLiteral("expect reg.d0 == 43") << 0 << false
                                        << QStringLiteral("$2a");
    QTest::newRow("register greater") << QStringLiteral("expect reg.d0 >= 1") << 0 << true << QString();
    QTest::newRow("register less") << QStringLiteral("expect reg.d0 < 1") << 0 << false << QStringLiteral("$2a");
    QTest::newRow("memory") << QStringLiteral("expect mem 0x1000 4 == de ad be ef") << 0 << true << QString();
    QTest::newRow("memory wrong") << QStringLiteral("expect mem 0x1000 4 == de ad be ee") << 0 << false
                                  << QStringLiteral("de ad be ef");
    QTest::newRow("at the stop") << QStringLiteral("expect at hello.s:16") << 0 << true << QString();
    QTest::newRow("at another line") << QStringLiteral("expect at hello.s:17") << 0 << false
                                     << QStringLiteral("hello.s:16");
}

void TstLessons::evaluatesCheckpoints()
{
    QFETCH(QString, expectLine);
    QFETCH(int, problems);
    QFETCH(bool, passes);
    QFETCH(QString, detailContains);

    // A snapshot with the relevant facts filled in: the values are the ones the
    // "passes" rows compare against, and the ones the failing rows report.
    Snapshot snapshot;
    snapshot.stopped = true;
    snapshot.running = false;
    snapshot.registers.insert(QStringLiteral("d0"), 42);
    snapshot.problems = problems;
    snapshot.memory = QByteArray::fromHex("deadbeef");
    snapshot.memoryValid = true;
    snapshot.sourceFile = QStringLiteral("/somewhere/hello.s");
    snapshot.sourceLine = 16;

    // Parsed through the lesson file, so the test exercises the real format
    // rather than a hand-built struct the parser might never produce.
    const QString text = QStringLiteral("title: T\nlevel: L\nlearn: x\n\nstep S\n  teach t\n  do build\n  %1\n")
                             .arg(expectLine);
    Lesson lesson;
    QString error;
    QVERIFY2(lessons::parseLesson(text, QStringLiteral("/tmp/cp"), &lesson, &error), qPrintable(error));
    QCOMPARE(lesson.steps.first().expects.size(), 1);

    QString detail;
    const bool ok = lessons::evaluateCheckpoint(lesson.steps.first().expects.first(), snapshot, &detail);
    QCOMPARE(ok, passes);
    if (passes) {
        QVERIFY(detail.isEmpty());
    } else {
        QVERIFY2(detail.contains(detailContains),
                 qPrintable(QStringLiteral("expected the detail to mention \"%1\", got \"%2\"")
                                .arg(detailContains, detail)));
    }
}

void TstLessons::everyShippedLessonParses()
{
    const QStringList roots = lessons::lessonRoots(QStringLiteral(PIST_SOURCE_DIR));
    QVERIFY2(!roots.isEmpty(), "the source tree's lessons/ directory was not found");

    QStringList warnings;
    const QVector<Lesson> loaded = lessons::loadLessons(roots, &warnings);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QStringLiteral("; "))));
    QVERIFY(!loaded.isEmpty());

    for (const Lesson &lesson : loaded) {
        QVERIFY2(lesson.usable(), qPrintable(QStringLiteral("%1: %2").arg(lesson.id, lesson.error)));
        QVERIFY(!lesson.title.isEmpty());
        QVERIFY(!lesson.steps.isEmpty());
        for (const lessons::Step &step : lesson.steps) {
            QVERIFY2(step.declaresOutcome(),
                     qPrintable(QStringLiteral("%1: step \"%2\" says nothing").arg(lesson.id, step.title)));
        }
    }
    // The lesson a fresh install opens with is one of them.
    QVERIFY(std::any_of(loaded.cbegin(), loaded.cend(), [](const Lesson &lesson) {
        return lesson.id == lessons::defaultLessonId();
    }));
}

void TstLessons::shippedLessonsNameTheirNeeds()
{
    const QStringList roots = lessons::lessonRoots(QStringLiteral(PIST_SOURCE_DIR));
    const QVector<Lesson> loaded = lessons::loadLessons(roots, nullptr);
    const QStringList known = lessons::knownNeeds();
    for (const Lesson &lesson : loaded) {
        for (const QString &need : lesson.needs)
            QVERIFY2(known.contains(need), qPrintable(QStringLiteral("%1: %2").arg(lesson.id, need)));
    }

    // A lesson that needs a tool says so, so the panel can skip rather than
    // fail it (§9) — every shipped lesson with steps that run a session must
    // declare the emulator.
    for (const Lesson &lesson : loaded) {
        bool runs = false;
        for (const lessons::Step &step : lesson.steps)
            for (const lessons::Action &action : step.actions)
                if (action.verb == QLatin1String("run"))
                    runs = true;
        if (runs) {
            QVERIFY2(lesson.needs.contains(QStringLiteral("emulator")),
                     qPrintable(QStringLiteral("%1 runs a session but does not need an emulator")
                                    .arg(lesson.id)));
        }
    }
}

void TstLessons::keyTokensResolveToTheBoundKey()
{
    MainWindow window;
    LessonRunner runner(&window);
    // The scheme decides what a token means; the lesson names the action, so
    // the same file reads correctly under either (docs/tutorial-plan.md §5
    // decision 9).
    runner.setKeyResolver([](const QString &action) {
        return action == QLatin1String("step") ? QStringLiteral("F11") : QString();
    });

    Lesson lesson;
    QString error;
    QVERIFY(lessons::parseLesson(
        QStringLiteral("title: T\nlevel: L\nlearn: x\n\nstep S\n  teach  Press {key:step} to advance, "
                       "{key:build} to build.\n  do build\n  expect stopped\n"),
        QStringLiteral("/tmp/tok"), &lesson, &error));
    runner.setLesson(lesson);

    const QString text = runner.teachText(0);
    QVERIFY2(text.contains(QStringLiteral("Press F11 to advance")), qPrintable(text));
    // An unbound action falls back to its name rather than leaving a hole.
    QVERIFY2(text.contains(QStringLiteral("Build to build")), qPrintable(text));
}

void TstLessons::explainsTheDiagnosticsTheToolchainActuallyPrints()
{
    // Every message here is one the toolchain really prints — the first four are
    // pinned verbatim in tests/tst_parsers.cpp, and `unknown mnemonic` is what
    // vasm answered when an indented equate read as an instruction. An
    // explanation table written against invented wording would teach nothing
    // (docs/tutorial-plan.md §11), so the table is anchored to these.
    const QStringList real = {
        QStringLiteral("error 10 in line 2 of \"bad.s\": number or identifier expected"),
        QStringLiteral("error 3004: section attributes <r> not supported"),
        QStringLiteral("fatal error 3008: output module doesn't allow multiple sections "
                       "of the same type ()"),
        QStringLiteral("Error 36: main.o (CODE+0x4): Reference to undefined symbol helper."),
        QStringLiteral("error 1 in line 1 of \"bad.s\": unknown mnemonic <kMax>"),
    };
    for (const QString &message : real) {
        const DiagnosticInfo *info = diagnosticRef(message);
        QVERIFY2(info != nullptr,
                 qPrintable(QStringLiteral("no explanation for: %1").arg(message)));
        QVERIFY(!info->meaning.isEmpty());
    }

    // Anything the table does not know says nothing at all: a wrong explanation
    // is worse than none.
    QVERIFY(diagnosticRef(QStringLiteral("this is not a diagnostic")) == nullptr);
    QVERIFY(diagnosticRef(QString()) == nullptr);
}

void TstLessons::aLessonHoldsAgainstARealSession_data()
{
    QTest::addColumn<QString>("lessonId");
    // hello-tos is the first lesson and the one a newcomer runs; fault is the
    // one whose whole point is a wrong answer, so its checkpoints have to hold
    // against a machine that really counts to eleven.
    QTest::newRow("hello-tos") << QStringLiteral("hello-tos");
    QTest::newRow("fault") << QStringLiteral("fault");
}

void TstLessons::aLessonHoldsAgainstARealSession()
{
    const QString missing = emulatorMissing();
    if (!missing.isEmpty()) {
        if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR")) {
            QFAIL(qPrintable(QStringLiteral("PIST_REQUIRE_EMULATOR is set but the emulator integration "
                                            "cannot run: missing %1").arg(missing)));
        }
        QSKIP(qPrintable(QStringLiteral("needs %1 (set $PIST_TOS_DIR if needed)").arg(missing)));
    }

    // The lesson's starter file belongs in a scratch directory, not the user's
    // own data directory.
    QTemporaryDir work;
    QVERIFY(work.isValid());
    qputenv("PIST_LESSON_DIR", work.path().toUtf8());

    QFETCH(QString, lessonId);
    const QVector<Lesson> loaded =
        lessons::loadLessons(lessons::lessonRoots(QStringLiteral(PIST_SOURCE_DIR)), nullptr);
    const auto it = std::find_if(loaded.cbegin(), loaded.cend(), [&lessonId](const Lesson &lesson) {
        return lesson.usable() && lesson.id == lessonId;
    });
    QVERIFY2(it != loaded.cend(),
             qPrintable(QStringLiteral("%1 is not among the shipped lessons").arg(lessonId)));

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    LessonRunner runner(&window);
    runner.setKeyResolver([](const QString &action) { return QStringLiteral("F%1").arg(action.size()); });
    runner.setLesson(*it);
    QVERIFY2(runner.start(), qPrintable(runner.verdictText()));
    QVERIFY(runner.count() > 0);

    // Every step, in order, with every checkpoint required to hold. This is the
    // lesson's own test run: the same expectations the panel shows as advice are
    // fatal here, so a lesson that stops matching the machine turns CI red
    // rather than teaching something untrue.
    for (int index = 0; index < runner.count(); ++index) {
        QSignalSpy checked(&runner, &LessonRunner::stepChecked);
        QSignalSpy failed(&runner, &LessonRunner::failed);
        runner.goToStep(index);
        runner.runStep();
        QTRY_VERIFY_WITH_TIMEOUT(checked.count() + failed.count() > 0, 240000);
        QVERIFY2(failed.isEmpty(),
                 qPrintable(QStringLiteral("step %1 could not run: %2")
                                .arg(index + 1)
                                .arg(runner.verdictText())));
        const QList<QVariant> args = checked.takeFirst();
        QVERIFY2(args.at(1).toBool(),
                 qPrintable(QStringLiteral("step %1 (%2): %3")
                                .arg(index + 1)
                                .arg(runner.lesson().steps.at(index).title, args.at(2).toString())));
    }

    qunsetenv("PIST_LESSON_DIR");
}

QTEST_MAIN(TstLessons)
#include "tst_lessons.moc"
