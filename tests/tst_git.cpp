// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Git is a subprocess. These tests drive a temporary repository: parsing, the
// panel's commit set, blame of a dirty line, and a push to a local bare repo.

#include "editor/CodeEditor.h"
#include "git/GitTypes.h"
#include "ui/GitPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

using namespace pist;

namespace {

bool haveGit()
{
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

bool runGit(const QString &dir, const QStringList &args, QString *out = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.start(QStringLiteral("git"), args);
    if (!process.waitForStarted(5000) || !process.waitForFinished(20000))
        return false;
    if (out)
        *out = QString::fromUtf8(process.readAllStandardOutput());
    if (process.exitCode() != 0) {
        qWarning("%s", process.readAllStandardError().constData());
        return false;
    }
    return true;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

QTreeWidgetItem *findRow(QTreeWidget *tree, const QString &group, const QString &path)
{
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *parent = tree->topLevelItem(g);
        if (parent->text(0) != group)
            continue;
        for (int i = 0; i < parent->childCount(); ++i) {
            if (parent->child(i)->data(0, Qt::UserRole).toString() == path)
                return parent->child(i);
        }
    }
    return nullptr;
}

} // namespace

class TstGit : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qputenv("GIT_AUTHOR_NAME", "Ada Lovelace");
        qputenv("GIT_AUTHOR_EMAIL", "ada@example.com");
        qputenv("GIT_COMMITTER_NAME", "Ada Lovelace");
        qputenv("GIT_COMMITTER_EMAIL", "ada@example.com");
    }

    void porcelainParsesBranchRenamesAndSpaces();
    void logPorcelainKeepsSubjectsAndAnEmptyDecoration();
    void blamePorcelainMarksAZeroHashUncommitted();

    void panelCommitsOnlyTheCheckedFile();
    void uncheckedStagedFileDoesNotRideAlong();
    void blameNamesTheAuthorAndADirtyLine();
    void pullWithoutUpstreamShowsGitsError();
    void pushReachesALocalBareRemote();
    void blameLaneDoesNotToggleBreakpoints();
    void branchSelectorCreatesAndSwitches();
    void switchRefusesToOverwriteLocalEdits();
    void selectedRowShowsStagedUnstagedAndUntrackedDiffs();
    void historyListsCommitsNewestFirstAndShowsTheOneYouPick();
    void emptyHistorySaysThereAreNoCommits();
};

void TstGit::porcelainParsesBranchRenamesAndSpaces()
{
    QByteArray raw = QByteArray("## main...origin/main [ahead 2, behind 1]") + '\0';
    raw += QByteArray("MM tracked") + '\0';
    raw += QByteArray("?? new file") + '\0';
    raw += QByteArray("RM new.s") + '\0';
    raw += QByteArray("old name.s") + '\0';

    const GitStatus status = parsePorcelain(raw);
    QVERIFY(status.ok);
    QCOMPARE(status.branch, QStringLiteral("main"));
    QCOMPARE(status.upstream, QStringLiteral("origin/main"));
    QCOMPARE(status.ahead, 2);
    QCOMPARE(status.behind, 1);
    QCOMPARE(status.entries.size(), 5);

    QCOMPARE(status.entries.at(0).group, GitChange::Staged);
    QCOMPARE(status.entries.at(0).path, QStringLiteral("tracked"));
    QCOMPARE(status.entries.at(1).group, GitChange::Unstaged);
    QCOMPARE(status.entries.at(1).path, QStringLiteral("tracked"));
    QCOMPARE(status.entries.at(2).group, GitChange::Untracked);
    QCOMPARE(status.entries.at(2).path, QStringLiteral("new file"));
    QCOMPARE(status.entries.at(3).group, GitChange::Staged);
    QCOMPARE(status.entries.at(3).path, QStringLiteral("new.s"));
    QCOMPARE(status.entries.at(3).from, QStringLiteral("old name.s"));
    QCOMPARE(status.entries.at(4).group, GitChange::Unstaged);
    QCOMPARE(status.entries.at(4).path, QStringLiteral("new.s"));
}

void TstGit::logPorcelainKeepsSubjectsAndAnEmptyDecoration()
{
    QByteArray raw;
    const auto field = [&raw](const char *text) {
        raw += text;
        raw += '\0';
    };
    field("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    field("aaaaaaa");
    field("Ada Lovelace");
    field("2020-01-02");
    field("HEAD -> main");
    field("second subject");
    raw += '\n';
    field("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    field("bbbbbbb");
    field("Ada Lovelace");
    field("2020-01-01");
    field("");
    field("first subject");
    raw += '\n';

    const GitLog log = parseLog(raw);
    QCOMPARE(log.size(), 2);
    QCOMPARE(log.at(0).abbrev, QStringLiteral("aaaaaaa"));
    QCOMPARE(log.at(0).refs, QStringLiteral("HEAD -> main"));
    QCOMPARE(log.at(0).subject, QStringLiteral("second subject"));
    QCOMPARE(log.at(1).hash, QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    QVERIFY(log.at(1).refs.isEmpty());
    QCOMPARE(log.at(1).subject, QStringLiteral("first subject"));
}

void TstGit::blamePorcelainMarksAZeroHashUncommitted()
{
    const QByteArray raw =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 2\n"
        "author Ada Lovelace\n"
        "author-time 1600000000\n"
        "summary first\n"
        "\tone\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 2 2\n"
        "\ttwo\n"
        "0000000000000000000000000000000000000000 3 3 1\n"
        "author External file (--contents)\n"
        "author-time 1600000000\n"
        "summary Version from standard input\n"
        "\tTHREE\n";

    const GitBlameMap lines = parseBlame(raw);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.value(1).author, QStringLiteral("Ada Lovelace"));
    QCOMPARE(lines.value(1).summary, QStringLiteral("first"));
    QVERIFY(!lines.value(1).uncommitted);
    QCOMPARE(lines.value(1).date,
             QDateTime::fromSecsSinceEpoch(1600000000).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    QCOMPARE(lines.value(2).author, QStringLiteral("Ada Lovelace"));
    QVERIFY(lines.value(3).uncommitted);
}

void TstGit::panelCommitsOnlyTheCheckedFile()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "move.w d0,d1\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "move.w d0,d2\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("b.s")), "nop\n"));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *files = panel.findChild<QTreeWidget *>(QStringLiteral("gitFiles"));
    QVERIFY(files);
    QTRY_VERIFY(findRow(files, QStringLiteral("Changes"), QStringLiteral("a.s")));
    QTRY_VERIFY(findRow(files, QStringLiteral("Untracked"), QStringLiteral("b.s")));

    auto *commit = panel.findChild<QPushButton *>(QStringLiteral("gitCommit"));
    auto *message = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitMessage"));
    QVERIFY(commit && message);
    QVERIFY(!commit->isEnabled());

    findRow(files, QStringLiteral("Changes"), QStringLiteral("a.s"))->setCheckState(0, Qt::Checked);
    message->setPlainText(QStringLiteral("only a"));
    QVERIFY(commit->isEnabled());
    message->setPlainText(QStringLiteral("   "));
    QVERIFY(!commit->isEnabled());
    message->setPlainText(QStringLiteral("only a"));
    commit->click();

    auto *output = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitOutput"));
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("Commit")), 10000);
    QVERIFY2(output->toPlainText().contains(QStringLiteral("finished")),
             qPrintable(output->toPlainText()));

    QString log;
    QVERIFY(runGit(dir.path(), {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s")}, &log));
    QCOMPARE(log.trimmed(), QStringLiteral("only a"));
    QString names;
    QVERIFY(runGit(dir.path(),
                   {QStringLiteral("show"), QStringLiteral("--name-only"), QStringLiteral("--pretty=format:"),
                    QStringLiteral("HEAD")},
                   &names));
    QVERIFY(names.contains(QStringLiteral("a.s")));
    QVERIFY(!names.contains(QStringLiteral("b.s")));

    QString status;
    QVERIFY(runGit(dir.path(), {QStringLiteral("status"), QStringLiteral("--porcelain")}, &status));
    QVERIFY(status.contains(QStringLiteral("b.s")));
    QVERIFY(!status.contains(QStringLiteral("a.s")));
}

void TstGit::uncheckedStagedFileDoesNotRideAlong()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "nop\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("b.s")), "nop\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s"), QStringLiteral("b.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "moveq #1,d0\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("b.s")), "moveq #2,d0\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s"), QStringLiteral("b.s")}));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *files = panel.findChild<QTreeWidget *>(QStringLiteral("gitFiles"));
    QTRY_VERIFY(findRow(files, QStringLiteral("Staged"), QStringLiteral("a.s")));
    QTRY_VERIFY(findRow(files, QStringLiteral("Staged"), QStringLiteral("b.s")));
    QCOMPARE(findRow(files, QStringLiteral("Staged"), QStringLiteral("a.s"))->checkState(0), Qt::Checked);
    QCOMPARE(findRow(files, QStringLiteral("Staged"), QStringLiteral("b.s"))->checkState(0), Qt::Checked);

    findRow(files, QStringLiteral("Staged"), QStringLiteral("b.s"))->setCheckState(0, Qt::Unchecked);
    auto *message = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitMessage"));
    message->setPlainText(QStringLiteral("just a"));
    panel.findChild<QPushButton *>(QStringLiteral("gitCommit"))->click();

    auto *output = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitOutput"));
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("finished")), 10000);

    QString names;
    QVERIFY(runGit(dir.path(),
                   {QStringLiteral("show"), QStringLiteral("--name-only"), QStringLiteral("--pretty=format:"),
                    QStringLiteral("HEAD")},
                   &names));
    QVERIFY(names.contains(QStringLiteral("a.s")));
    QVERIFY(!names.contains(QStringLiteral("b.s")));

    QString status;
    QVERIFY(runGit(dir.path(), {QStringLiteral("status"), QStringLiteral("--porcelain")}, &status));
    QVERIFY2(status.contains(QStringLiteral("b.s")), qPrintable(status));
    QVERIFY(!status.contains(QStringLiteral("a.s")));
}

void TstGit::blameNamesTheAuthorAndADirtyLine()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    const QString path = dir.filePath(QStringLiteral("f.s"));
    QVERIFY(writeFile(path, "one\ntwo\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("f.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("first")}));

    GitPanel panel;
    panel.setDirectory(dir.path());
    QTRY_VERIFY(panel.inRepository());

    QSignalSpy blamed(&panel, &GitPanel::blameReady);
    panel.blame(path, 1, 2, "one\nTWO\n");
    QTRY_VERIFY(blamed.count() >= 1);
    const auto lines = blamed.at(0).at(1).value<GitBlameMap>();
    QCOMPARE(lines.value(1).author, QStringLiteral("Ada Lovelace"));
    QVERIFY(!lines.value(1).uncommitted);
    QVERIFY2(lines.value(2).uncommitted, qPrintable(lines.value(2).author));
}

void TstGit::pullWithoutUpstreamShowsGitsError()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "nop\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *pull = panel.findChild<QPushButton *>(QStringLiteral("gitPull"));
    QTRY_VERIFY(pull->isEnabled());
    pull->click();

    auto *output = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitOutput"));
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("Pull")), 10000);
    QVERIFY2(output->toPlainText().contains(QStringLiteral("failed")),
             qPrintable(output->toPlainText()));
    QVERIFY2(!output->toPlainText().trimmed().isEmpty(), qPrintable(output->toPlainText()));
}

void TstGit::pushReachesALocalBareRemote()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir bare;
    QTemporaryDir dir;
    QVERIFY(bare.isValid() && dir.isValid());
    QVERIFY(runGit(bare.path(), {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-q")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "nop\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), bare.path()}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("HEAD")}));

    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "rts\n"));
    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *files = panel.findChild<QTreeWidget *>(QStringLiteral("gitFiles"));
    QTRY_VERIFY(findRow(files, QStringLiteral("Changes"), QStringLiteral("a.s")));
    findRow(files, QStringLiteral("Changes"), QStringLiteral("a.s"))->setCheckState(0, Qt::Checked);
    panel.findChild<QPlainTextEdit *>(QStringLiteral("gitMessage"))->setPlainText(QStringLiteral("send it"));
    panel.findChild<QPushButton *>(QStringLiteral("gitCommit"))->click();
    auto *output = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitOutput"));
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("Commit finished")), 10000);

    auto *push = panel.findChild<QPushButton *>(QStringLiteral("gitPush"));
    QTRY_VERIFY(push->isEnabled());
    push->click();
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("Push finished")), 15000);

    QString local;
    QString remote;
    QVERIFY(runGit(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &local));
    QVERIFY(runGit(bare.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, &remote));
    QCOMPARE(remote.trimmed(), local.trimmed());
}

void TstGit::blameLaneDoesNotToggleBreakpoints()
{
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("    move.w d0,d1\n"));
    const int plain = editor.lineNumberAreaWidth();
    editor.setBlameShown(true);
    QVERIFY(editor.blameLaneWidth() > 40);
    QVERIFY(editor.lineNumberAreaWidth() > plain);

    GitBlameLine info;
    info.hash = QString(40, QLatin1Char('a'));
    info.author = QStringLiteral("Ada Lovelace");
    info.date = QStringLiteral("2020-01-01 00:00");
    info.summary = QStringLiteral("first line");
    GitBlameMap lines;
    lines.insert(1, info);
    editor.setBlame(lines);

    editor.resize(700, 300);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    auto *gutter = editor.findChild<QWidget *>(QStringLiteral("lineNumberArea"));
    QVERIFY(gutter);
    const int lane = editor.blameLaneWidth();

    QSignalSpy clicks(&editor, &CodeEditor::gutterClicked);
    QTest::mouseClick(gutter, Qt::LeftButton, Qt::NoModifier, QPoint(lane / 2, 8));
    QCOMPARE(clicks.count(), 0);

    QTest::mouseMove(gutter, QPoint(4, 8));
    QVERIFY(gutter->toolTip().contains(QStringLiteral("Ada Lovelace")));
    QVERIFY(gutter->toolTip().contains(info.hash));
    QVERIFY(gutter->toolTip().contains(QStringLiteral("first line")));

    QTest::mouseClick(gutter, Qt::LeftButton, Qt::NoModifier, QPoint(lane + 4, 8));
    QCOMPARE(clicks.count(), 1);
    QCOMPARE(clicks.at(0).at(0).toInt(), 1);

    editor.setBlameShown(false);
    QCOMPARE(editor.lineNumberAreaWidth(), plain);
    QCOMPARE(editor.blameLaneWidth(), 0);
}

void TstGit::branchSelectorCreatesAndSwitches()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "nop\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));
    QString original;
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}, &original));
    original = original.trimmed();
    QVERIFY(!original.isEmpty());

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *combo = panel.findChild<QComboBox *>(QStringLiteral("gitBranches"));
    auto *add = panel.findChild<QPushButton *>(QStringLiteral("gitNewBranch"));
    QVERIFY(combo && add);
    QTRY_VERIFY(add->isEnabled());
    QTRY_COMPARE(combo->currentText(), original);

    QTimer::singleShot(0, [] {
        auto *dialog = QApplication::activeModalWidget();
        QVERIFY(dialog);
        auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("gitBranchName"));
        auto *box = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(edit && box);
        auto *ok = box->button(QDialogButtonBox::Ok);
        QVERIFY(ok);
        QVERIFY(!ok->isEnabled());
        edit->setText(QStringLiteral("feature"));
        QVERIFY(ok->isEnabled());
        ok->click();
    });
    add->click();

    auto *output = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitOutput"));
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("New branch")), 10000);
    QVERIFY2(output->toPlainText().contains(QStringLiteral("finished")),
             qPrintable(output->toPlainText()));
    QTRY_COMPARE(combo->currentText(), QStringLiteral("feature"));

    QString current;
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}, &current));
    QCOMPARE(current.trimmed(), QStringLiteral("feature"));

    combo->setCurrentText(original);
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("Switch finished")), 10000);
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}, &current));
    QCOMPARE(current.trimmed(), original);
}

void TstGit::switchRefusesToOverwriteLocalEdits()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "base\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));
    QString original;
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}, &original));
    original = original.trimmed();

    QVERIFY(runGit(dir.path(), {QStringLiteral("switch"), QStringLiteral("-c"), QStringLiteral("feature")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "feature\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("feature")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("switch"), original}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "dirty\n"));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *combo = panel.findChild<QComboBox *>(QStringLiteral("gitBranches"));
    QVERIFY(combo);
    QTRY_VERIFY(combo->findText(QStringLiteral("feature")) >= 0);
    QCOMPARE(combo->currentText(), original);

    combo->setCurrentText(QStringLiteral("feature"));
    auto *output = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitOutput"));
    QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("Switch failed")), 10000);

    QString current;
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}, &current));
    QCOMPARE(current.trimmed(), original);
    QTRY_COMPARE(combo->currentText(), original);
}

void TstGit::selectedRowShowsStagedUnstagedAndUntrackedDiffs()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "nop\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("base")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "moveq #1,d0\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "rts\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("new.s")), "untracked-line\n"));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *files = panel.findChild<QTreeWidget *>(QStringLiteral("gitFiles"));
    auto *diff = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitDiff"));
    QVERIFY(files && diff);
    QTRY_VERIFY(findRow(files, QStringLiteral("Staged"), QStringLiteral("a.s")));
    QTRY_VERIFY(findRow(files, QStringLiteral("Changes"), QStringLiteral("a.s")));
    QTRY_VERIFY(findRow(files, QStringLiteral("Untracked"), QStringLiteral("new.s")));
    QVERIFY(diff->isHidden());

    files->setCurrentItem(findRow(files, QStringLiteral("Staged"), QStringLiteral("a.s")));
    QVERIFY(!diff->isHidden());
    QTRY_VERIFY_WITH_TIMEOUT(diff->toPlainText().contains(QStringLiteral("+moveq #1,d0")), 10000);
    QVERIFY(diff->toPlainText().contains(QStringLiteral("-nop")));
    QVERIFY(!diff->toPlainText().contains(QStringLiteral("+rts")));

    files->setCurrentItem(findRow(files, QStringLiteral("Changes"), QStringLiteral("a.s")));
    QTRY_VERIFY_WITH_TIMEOUT(diff->toPlainText().contains(QStringLiteral("+rts")), 10000);
    QVERIFY(diff->toPlainText().contains(QStringLiteral("-moveq #1,d0")));
    QVERIFY(!diff->toPlainText().contains(QStringLiteral("-nop")));

    files->setCurrentItem(findRow(files, QStringLiteral("Untracked"), QStringLiteral("new.s")));
    QTRY_VERIFY_WITH_TIMEOUT(diff->toPlainText().contains(QStringLiteral("+untracked-line")), 10000);
    QVERIFY2(!diff->toPlainText().toLower().contains(QStringLiteral("failed")),
             qPrintable(diff->toPlainText()));
}

void TstGit::historyListsCommitsNewestFirstAndShowsTheOneYouPick()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "first-line\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("first subject")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("a.s")), "second-line\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("a.s")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("second subject")}));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *history = panel.findChild<QPushButton *>(QStringLiteral("gitHistory"));
    auto *log = panel.findChild<QListWidget *>(QStringLiteral("gitLog"));
    auto *diff = panel.findChild<QPlainTextEdit *>(QStringLiteral("gitDiff"));
    QVERIFY(history && log && diff);
    QTRY_VERIFY(history->isEnabled());
    history->click();

    QTRY_VERIFY_WITH_TIMEOUT(log->count() >= 2, 10000);
    QVERIFY(log->item(0)->text().contains(QStringLiteral("second subject")));
    QVERIFY(log->item(1)->text().contains(QStringLiteral("first subject")));
    QTRY_VERIFY_WITH_TIMEOUT(diff->toPlainText().contains(QStringLiteral("second-line")), 10000);
    QVERIFY(diff->toPlainText().contains(QStringLiteral("second subject")));

    log->setCurrentItem(log->item(1));
    QTRY_VERIFY_WITH_TIMEOUT(diff->toPlainText().contains(QStringLiteral("first subject")), 10000);
    QVERIFY(diff->toPlainText().contains(QStringLiteral("first-line")));
    QVERIFY(!diff->toPlainText().contains(QStringLiteral("second-line")));

    auto *changes = panel.findChild<QPushButton *>(QStringLiteral("gitChanges"));
    changes->click();
    QVERIFY(!panel.findChild<QPlainTextEdit *>(QStringLiteral("gitMessage"))->isHidden());
    QVERIFY(log->isHidden());
}

void TstGit::emptyHistorySaysThereAreNoCommits()
{
    if (!haveGit())
        QSKIP("git is not on PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));

    GitPanel panel;
    panel.setDirectory(dir.path());
    auto *history = panel.findChild<QPushButton *>(QStringLiteral("gitHistory"));
    QTRY_VERIFY(history->isEnabled());
    history->click();

    auto *log = panel.findChild<QListWidget *>(QStringLiteral("gitLog"));
    QTRY_VERIFY_WITH_TIMEOUT(log->count() == 1, 10000);
    QCOMPARE(log->item(0)->text(), QStringLiteral("No commits yet."));
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    TstGit test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_git.moc"
