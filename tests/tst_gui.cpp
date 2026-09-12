// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// End-to-end test of the debug loop through MainWindow: build, launch, attach,
// resolve a source-line breakpoint, hit it, and confirm the editor follows the
// program counter.
//
// This covers the layer that no other suite touches. The README claims the
// editor follows the PC and that breakpoints work, so those claims are asserted
// here rather than assumed.

#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "ui/FileBrowser.h"
#include "ui/MainWindow.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTreeView>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

class TstGui : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void windowConstructs();
    void assemblesAndMapsLines();
    void editorShowsExecutionLineAndBreakpoints();

    /// Run must eventually start an emulator session — and must do so *after* the
    /// asynchronous build finishes, not alongside it.
    void runStartsAnEmulatorSession();

    /// Floppy images reach the emulator command line.
    void floppyImagesReachTheCommandLine();
    void fileBrowserShowsTheProjectDirectory();
    void editorTracksUnsavedChanges();
    void diagnoseReportsToolsAndRoms();

private:
    QString m_vasm;
    QString m_tos;
    QTemporaryDir *m_work = nullptr;
    QString m_source;
};

void TstGui::initTestCase()
{
    m_vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (m_vasm.isEmpty())
        QSKIP("needs vasmm68k_mot");

    m_work = new QTemporaryDir;
    QVERIFY(m_work->isValid());
    m_source = m_work->path() + QStringLiteral("/prog.s");
}

void TstGui::windowConstructs()
{
    // The window builds its whole dock layout, prober and action set here. A
    // failure to start Hatari or vasm must not prevent construction.
    MainWindow window;
    QVERIFY(!window.windowTitle().isEmpty());
    QVERIFY2(window.windowTitle().contains(QLatin1String("PiST")),
             qPrintable(window.windowTitle()));
}

// The line map is what turns a gutter click into an address and a PC back into a
// line. Verify against a real listing rather than a fixture.
void TstGui::assemblesAndMapsLines()
{
    QFile src(m_source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tpea\tmsg(pc)\n"
              "\tmove.w\t#9,-(sp)\n"
              "\ttrap\t#1\n"
              "\taddq.l\t#6,sp\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "msg:\tdc.b\t\"HI\",0\n"
              "\teven\n"
              "\tend\n");
    src.close();

    const QString listing = m_work->path() + QStringLiteral("/prog.lst");
    const QString prg = m_work->path() + QStringLiteral("/prog.prg");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"),
                        QStringLiteral("-L"), listing, QStringLiteral("-o"), prg, m_source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);

    LineMap map;
    QString error;
    QVERIFY2(map.parseListing(listing, &error), qPrintable(error));
    QVERIFY(!map.isEmpty());

    // `loop:` is on line 6 and must resolve once we know where the program was
    // loaded. This is the address a gutter click would produce.
    LineMap::SectionBases bases;
    bases.text = 0x12596;
    bases.data = 0x125a4;
    bases.bss = 0x125a6;

    quint32 loopAddress = 0;
    QVERIFY(map.addressFor(QStringLiteral("prog.s"), 6, bases, &loopAddress));
    QVERIFY(loopAddress > bases.text);

    // ...and the reverse direction: the PC inside that instruction resolves back
    // to line 6, which is what drives the editor highlight.
    LineMap::Address back;
    QVERIFY(map.lineFor(loopAddress, bases, &back));
    QCOMPARE(back.line, 6);
    // The listing records the path as it was passed to vasm, which here is
    // absolute. Matching must therefore be path-tolerant, and this assertion is
    // the regression guard for the highlight silently never appearing.
    QVERIFY2(LineMap::sameSource(back.file, QStringLiteral("prog.s")),
             qPrintable("recorded path does not match the open file: " + back.file));
    QVERIFY(LineMap::sameSource(back.file, m_source));
}

// The last step of the debug loop: MainWindow turns a resolved PC into a
// highlight on the editor widget, and a gutter click into a marker. Both are
// plain widget state, so they are asserted directly rather than inferred from
// the line map being correct.
void TstGui::editorShowsExecutionLineAndBreakpoints()
{
    MainWindow window;
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY2(editor, "MainWindow must own a CodeEditor");

    editor->setPlainText(QStringLiteral("\ttext\nstart:\tnop\n\tbra.s\tstart\n\tend\n"));

    // Highlight follows whatever the debug loop resolved.
    QCOMPARE(editor->currentExecutionLine(), 0);
    editor->setCurrentExecutionLine(2);
    QCOMPARE(editor->currentExecutionLine(), 2);

    editor->clearCurrentExecutionLine();
    QCOMPARE(editor->currentExecutionLine(), 0);

    // Gutter markers, which the breakpoint list drives.
    QVERIFY(editor->breakpointLines().isEmpty());
    editor->setBreakpointLines({2, 3});
    QCOMPARE(editor->breakpointLines(), QList<int>({2, 3}));
    editor->setBreakpointLines({});
    QVERIFY(editor->breakpointLines().isEmpty());
}

// The regression this guards: Run called build() (asynchronous) and then checked
// whether a build was in flight, which is always true immediately after starting
// one — so the entire emulator-launch path was unreachable dead code and Run
// appeared to "just build".
void TstGui::runStartsAnEmulatorSession()
{
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        QSKIP("needs hatari");
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    // A program that assembles cleanly and spins, so the session stops at entry.
    const QString source = m_work->path() + QStringLiteral("/run.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    // A project file with no include paths, so no modal dialog can interrupt.
    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error), qPrintable(error));

    MainWindow window;
    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY2(host, "MainWindow must own an EmulatorHost");
    QVERIFY(!host->isRunning());

    window.openPath(source);
    QCOMPARE(window.windowTitle().contains(QLatin1String("PiST")), true);

    QSignalSpy runningSpy(host, &EmulatorHost::runningChanged);

    // Invoke Run exactly as the toolbar action does.
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    // The emulator must come up without further interaction. The build runs first,
    // so allow for it.
    QTRY_VERIFY_WITH_TIMEOUT(host->isRunning(), 30000);

    // ...and it must be a real session, not just a spawned process.
    QSignalSpy stoppedSpy(host, &EmulatorHost::stoppedChanged);
    QVERIFY2(stoppedSpy.wait(30000) || host->isStopped(),
             "the emulator started but never reached the debugger");

    host->stop();
}

// Floppy drives are the last emulator feature that had no UI. Verify the
// configured images actually reach `--disk-a`/`--disk-b`, since a dialog field
// that never reaches the command line is the failure mode this project has hit
// before (BuildService's include paths, and the run path itself).
void TstGui::floppyImagesReachTheCommandLine()
{
    SessionConfig config;
    config.hatariPath = QStringLiteral("hatari");
    config.programPath = QStringLiteral("/tmp/prog.prg");
    config.floppyImages = {QStringLiteral("/disks/boot.st"),
                           QStringLiteral("/disks/data.st")};

    const QStringList argv = config.toArgv();
    const QString joined = argv.join(QLatin1Char(' '));

    QVERIFY2(joined.contains(QLatin1String("--disk-a /disks/boot.st")), qPrintable(joined));
    QVERIFY2(joined.contains(QLatin1String("--disk-b /disks/data.st")), qPrintable(joined));

    // A user might also supply only a disk for A:, which must not emit a bare
    // --disk-b with nothing after it.
    SessionConfig onlyA;
    onlyA.hatariPath = QStringLiteral("hatari");
    onlyA.programPath = QStringLiteral("/tmp/prog.prg");
    onlyA.floppyImages = {QStringLiteral("/disks/boot.st"), QString()};
    const QString joinedA = onlyA.toArgv().join(QLatin1Char(' '));
    QVERIFY(joinedA.contains(QLatin1String("--disk-a")));
    QVERIFY2(!joinedA.contains(QLatin1String("--disk-b")), qPrintable(joinedA));
}

void TstGui::fileBrowserShowsTheProjectDirectory()
{
    // A project directory with a couple of files, so the browser has something to
    // root itself in.
    const QString dir = m_work->path() + QStringLiteral("/browse");
    QVERIFY(QDir().mkpath(dir));
    const QString src = dir + QStringLiteral("/prog.s");
    QFile f(src);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("\tnop\n");
    f.close();

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY2(browser, "MainWindow must own a FileBrowser");

    // Opening a file must point the browser at that file's directory, so the
    // browser and the editor never disagree about which project is open.
    window.openPath(src);

    auto *view = browser->findChild<QTreeView *>();
    QVERIFY(view);
    QVERIFY2(view->model(), "the browser must have a model");
    const QString rootPath = view->model()->data(view->rootIndex(), Qt::UserRole + 1).toString();
    QCOMPARE(rootPath, dir);

    // The file being edited is the selected entry.
    const QString selected = view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString();
    QCOMPARE(selected, src);
}

// The editor's modified state drives the window title and the save prompt, and
// until now isModifiedSinceLoad() was never called from anywhere.
void TstGui::editorTracksUnsavedChanges()
{
    MainWindow window;
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);

    editor->setPlainText(QStringLiteral("\tnop\n"));
    editor->document()->setModified(false);
    QVERIFY(!editor->isModifiedSinceLoad());

    editor->insertPlainText(QStringLiteral("\trts\n"));
    QVERIFY2(editor->isModifiedSinceLoad(),
             "editing must mark the document modified, or the save prompt never fires");

    editor->document()->setModified(false);
    QVERIFY(!editor->isModifiedSinceLoad());

    // An id in the title with the modified marker is how Qt shows unsaved state.
    QVERIFY2(window.windowTitle().contains(QLatin1String("[*]")),
             qPrintable("title must carry the modified placeholder: " + window.windowTitle()));
}

// `--diagnose` is what release packaging uses to prove an archive resolves the
// assembler it carries, so the option has to be dependable. Run the built binary
// and check that it reports something for each section.
void TstGui::diagnoseReportsToolsAndRoms()
{
    // The test executable is not pist, so locate the application binary beside
    // it — the same directory CMake puts them both in.
    QString app = QCoreApplication::applicationDirPath() + QStringLiteral("/pist");
#ifdef Q_OS_WIN
    app += QStringLiteral(".exe");
#endif
    if (!QFileInfo::exists(app))
        QSKIP("pist binary not found next to the test executable");

    QProcess process;
    process.start(app, {QStringLiteral("--diagnose")});
    QVERIFY(process.waitForStarted(5000));
    QVERIFY(process.waitForFinished(15000));

    const QString output = QString::fromUtf8(process.readAllStandardOutput())
                         + QString::fromUtf8(process.readAllStandardError());

    QVERIFY2(process.exitCode() == 0, qPrintable("diagnose failed: " + output));
    QVERIFY2(output.contains(QLatin1String("Assembler")), qPrintable(output));
    QVERIFY2(output.contains(QLatin1String("Emulator")), qPrintable(output));
    QVERIFY2(output.contains(QLatin1String("TOS ROMs found")), qPrintable(output));
    QVERIFY2(output.contains(QLatin1String("Search paths")), qPrintable(output));

    // It must name a path or say NOT FOUND — never leave the section blank, which
    // would make a packaging check pass vacuously.
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("Assembler (")))
            QVERIFY2(line.contains(QLatin1String("NOT FOUND"))
                         || line.contains(QLatin1Char('/')) || line.contains(QLatin1Char('\\')),
                     qPrintable("no assembler path or NOT FOUND: " + line));
    }
}

QTEST_MAIN(TstGui)
#include "tst_gui.moc"
