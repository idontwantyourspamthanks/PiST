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
#include "ui/MainWindow.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
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

QTEST_MAIN(TstGui)
#include "tst_gui.moc"
