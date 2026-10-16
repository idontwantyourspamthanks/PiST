// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "lessons/Lesson.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace pist {
namespace lessons {

namespace {

/// A register value as a lesson writes it: decimal, $hex or 0x hex. Returns
/// false for anything else, so a typo in a lesson is a parse error rather than
/// a checkpoint that compares against zero.
bool parseValue(const QString &text, quint32 *out)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    bool ok = false;
    quint32 value = 0;
    if (trimmed.startsWith(QLatin1Char('$')) || trimmed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        const QString digits = trimmed.startsWith(QLatin1Char('$')) ? trimmed.mid(1) : trimmed.mid(2);
        value = digits.toUInt(&ok, 16);
    } else {
        value = trimmed.toUInt(&ok, 10);
    }
    if (!ok)
        return false;
    *out = value;
    return true;
}

/// The needs/flags a lesson file may name. Unknown ones are refused: a lesson
/// that declares `emultor` would silently never skip and never run.
const QStringList &needsTable()
{
    static const QStringList table = {
        QStringLiteral("assembler"), QStringLiteral("linker"),
        QStringLiteral("emulator"), QStringLiteral("rom"),
    };
    return table;
}

/// Verb -> the argument counts it accepts. -1 means "any number of trailing
/// arguments, including none" (only `disasm` today, whose address is optional).
struct VerbInfo
{
    const char *verb;
    int argc;  ///< exact count, or 0 for none
    bool optional;
};

const QList<VerbInfo> &verbTable()
{
    static const QList<VerbInfo> table = {
        { "open", 1, false },
        { "goto", 1, false },
        { "breakpoint", 1, false },
        { "clearbreakpoint", 1, false },
        { "build", 0, false },
        { "buildrelease", 0, false },
        { "run", 0, false },
        { "stop", 0, false },
        { "continue", 0, false },
        { "step", 0, false },
        { "stepover", 0, false },
        { "readmem", 2, false },
        { "disasm", 1, true },
        { "explain", 1, false },
    };
    return table;
}

bool splitCheckpointBytes(const QString &text, QByteArray *out)
{
    out->clear();
    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        // A byte pair, or one run of them ("deadbeef") so a lesson can write a
        // long expectation on one line.
        if (token.size() % 2 != 0 || token.isEmpty())
            return false;
        for (int i = 0; i < token.size(); i += 2) {
            bool ok = false;
            const int byte = token.mid(i, 2).toInt(&ok, 16);
            if (!ok)
                return false;
            out->append(char(byte));
        }
    }
    return !out->isEmpty();
}

} // namespace

QString Action::text() const
{
    return args.isEmpty() ? verb : verb + QLatin1Char(' ') + args.join(QLatin1Char(' '));
}

QStringList actionVerbs()
{
    QStringList verbs;
    for (const VerbInfo &info : verbTable())
        verbs << QString::fromLatin1(info.verb);
    return verbs;
}

QStringList knownNeeds()
{
    return needsTable();
}

QString checkpointText(const Checkpoint &c)
{
    switch (c.kind) {
    case Checkpoint::Stopped:
        return QStringLiteral("stopped");
    case Checkpoint::Running:
        return QStringLiteral("running");
    case Checkpoint::ProblemsNone:
        return QStringLiteral("problems == none");
    case Checkpoint::Register:
        return QStringLiteral("reg.%1 %2 %3").arg(c.subject, c.op, c.value);
    case Checkpoint::Memory: {
        QStringList bytes;
        for (const char b : c.bytes)
            bytes << QStringLiteral("%1").arg(quint8(b), 2, 16, QLatin1Char('0'));
        return QStringLiteral("mem %1 %2 == %3")
            .arg(c.address)
            .arg(c.length)
            .arg(bytes.join(QLatin1Char(' ')));
    }
    case Checkpoint::AtLine:
        return QStringLiteral("at %1:%2").arg(c.subject).arg(c.line);
    }
    return {};
}

namespace {

bool parseCheckpoint(const QString &text, Checkpoint *out, QString *error)
{
    const QString trimmed = text.trimmed();
    const QStringList head = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (head.isEmpty()) {
        *error = QStringLiteral("an expect needs a form (stopped, running, problems == none, "
                                "reg.d0 == 1, mem 0x1000 4 == deadbeef, at file.s:12)");
        return false;
    }

    if (head.first() == QLatin1String("stopped")) {
        out->kind = Checkpoint::Stopped;
        return true;
    }
    if (head.first() == QLatin1String("running")) {
        out->kind = Checkpoint::Running;
        return true;
    }

    if (head.first() == QLatin1String("problems")) {
        if (head.size() != 3 || head.at(1) != QLatin1String("==") || head.at(2) != QLatin1String("none")) {
            *error = QStringLiteral("the only problems form is `problems == none`");
            return false;
        }
        out->kind = Checkpoint::ProblemsNone;
        return true;
    }

    if (head.first() == QLatin1String("reg.")) {
        *error = QStringLiteral("name the register after the dot: reg.d0, reg.a7, reg.pc, reg.sr");
        return false;
    }
    if (head.first().startsWith(QLatin1String("reg."))) {
        // reg.<name> <op> <value>
        if (head.size() != 3) {
            *error = QStringLiteral("a register checkpoint is `reg.<name> <op> <value>`");
            return false;
        }
        static const QSet<QString> ops = { QStringLiteral("=="), QStringLiteral("!="),
                                           QStringLiteral(">="), QStringLiteral("<="),
                                           QStringLiteral(">"), QStringLiteral("<") };
        out->kind = Checkpoint::Register;
        out->subject = head.at(0).mid(4).toLower();
        out->op = head.at(1);
        out->value = head.at(2);
        if (!ops.contains(out->op)) {
            *error = QStringLiteral("`%1` is not a comparison (== != >= <= > <)").arg(out->op);
            return false;
        }
        quint32 ignored = 0;
        if (!parseValue(out->value, &ignored)) {
            *error = QStringLiteral("`%1` is not a number (decimal, $hex or 0x hex)").arg(out->value);
            return false;
        }
        static const QSet<QString> registers = {
            QStringLiteral("d0"), QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"),
            QStringLiteral("d4"), QStringLiteral("d5"), QStringLiteral("d6"), QStringLiteral("d7"),
            QStringLiteral("a0"), QStringLiteral("a1"), QStringLiteral("a2"), QStringLiteral("a3"),
            QStringLiteral("a4"), QStringLiteral("a5"), QStringLiteral("a6"), QStringLiteral("a7"),
            QStringLiteral("pc"), QStringLiteral("sr"),
        };
        if (!registers.contains(out->subject)) {
            *error = QStringLiteral("`%1` is not a register the debugger reads").arg(out->subject);
            return false;
        }
        return true;
    }

    if (head.first() == QLatin1String("mem")) {
        // mem <address> <length> == <bytes...>
        const int eq = head.indexOf(QLatin1String("=="));
        if (head.size() < 5 || eq != 3) {
            *error = QStringLiteral("a memory checkpoint is `mem <address> <length> == <hex bytes>`");
            return false;
        }
        quint32 address = 0;
        if (!parseValue(head.at(1), &address)) {
            *error = QStringLiteral("`%1` is not an address").arg(head.at(1));
            return false;
        }
        bool ok = false;
        const int length = head.at(2).toInt(&ok, 0);
        if (!ok || length <= 0) {
            *error = QStringLiteral("`%1` is not a byte count").arg(head.at(2));
            return false;
        }
        QByteArray bytes;
        if (!splitCheckpointBytes(head.mid(4).join(QLatin1Char(' ')), &bytes)) {
            *error = QStringLiteral("the expected bytes must be hex pairs");
            return false;
        }
        if (bytes.size() != length) {
            *error = QStringLiteral("%1 bytes were expected but %2 were written")
                         .arg(length)
                         .arg(bytes.size());
            return false;
        }
        out->kind = Checkpoint::Memory;
        out->address = address;
        out->length = length;
        out->bytes = bytes;
        return true;
    }

    if (head.first() == QLatin1String("at")) {
        // at <file>:<line>  — the machine is stopped on that source line
        if (head.size() != 2) {
            *error = QStringLiteral("an at checkpoint is `at <file>:<line>`");
            return false;
        }
        const QString tail = head.at(1);
        const int colon = tail.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0) {
            *error = QStringLiteral("`%1` is not file:line").arg(tail);
            return false;
        }
        bool ok = false;
        const int line = tail.mid(colon + 1).toInt(&ok);
        if (!ok || line <= 0) {
            *error = QStringLiteral("`%1` is not a line number").arg(tail.mid(colon + 1));
            return false;
        }
        out->kind = Checkpoint::AtLine;
        out->subject = tail.left(colon);
        out->line = line;
        return true;
    }

    *error = QStringLiteral("`%1` is not a checkpoint form").arg(head.first());
    return false;
}

} // namespace

bool parseLesson(const QString &text, const QString &dir, Lesson *out, QString *error)
{
    Lesson lesson;
    lesson.dir = dir;
    lesson.id = QFileInfo(dir).fileName();

    bool inSteps = false;
    bool sourceSeen = false;
    Step current;
    bool haveStep = false;
    QString pendingKeyword;  ///< the directive a continuation would extend

    auto fail = [&](int line, const QString &what) {
        // line 0 is a lesson-level problem (a missing key, a step that checks
        // nothing), not a position in the file.
        *error = line > 0 ? QStringLiteral("line %1: %2").arg(line).arg(what) : what;
        return false;
    };

    auto finishStep = [&]() {
        lesson.steps.append(current);
        current = Step();
        pendingKeyword.clear();
    };

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const int lineNumber = i + 1;
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;
        if (line.trimmed().startsWith(QLatin1Char('#')))
            continue;

        const int indent = line.size() - line.trimmed().size();
        const QString trimmed = line.trimmed();

        // A continuation of the directive above: indented further than a
        // directive, so prose can wrap without a marker.
        if (indent >= 4) {
            if (pendingKeyword.isEmpty())
                return fail(lineNumber, QStringLiteral("this continues nothing — indent a "
                                                       "teach/see/nominal line above it"));
            if (pendingKeyword == QLatin1String("teach"))
                current.teach += QLatin1Char(' ') + trimmed;
            else if (pendingKeyword == QLatin1String("nominal"))
                current.nominal += QLatin1Char(' ') + trimmed;
            else
                current.see.last() += QLatin1Char(' ') + trimmed;
            continue;
        }

        pendingKeyword.clear();

        if (indent == 0) {
            if (trimmed.startsWith(QLatin1String("step ")) || trimmed == QLatin1String("step")) {
                if (haveStep)
                    finishStep();
                const QString title = trimmed.mid(4).trimmed();
                if (title.isEmpty())
                    return fail(lineNumber, QStringLiteral("a step needs a title"));
                current.title = title;
                haveStep = true;
                inSteps = true;
                continue;
            }
            if (inSteps) {
                // A col-0 line after the first step can only be another step.
                return fail(lineNumber, QStringLiteral("a lesson's steps are all at the start; "
                                                       "this line is neither `step ...` nor indented"));
            }
            // Front matter: key: value
            const int colon = trimmed.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                return fail(lineNumber, QStringLiteral("expected `key: value` before the first step"));
            const QString key = trimmed.left(colon).trimmed().toLower();
            const QString value = trimmed.mid(colon + 1).trimmed();
            if (value.isEmpty())
                return fail(lineNumber, QStringLiteral("`%1` has no value").arg(key));
            if (key == QLatin1String("id")) {
                lesson.id = value;
            } else if (key == QLatin1String("title")) {
                lesson.title = value;
            } else if (key == QLatin1String("level")) {
                lesson.level = value;
            } else if (key == QLatin1String("learn")) {
                lesson.learn = value;
            } else if (key == QLatin1String("needs")) {
                const QStringList parts = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
                for (const QString &part : parts) {
                    const QString need = part.trimmed().toLower();
                    if (!needsTable().contains(need))
                        return fail(lineNumber, QStringLiteral("`%1` is not a need (%2)")
                                                    .arg(need, needsTable().join(QStringLiteral(", "))));
                    lesson.needs << need;
                }
            } else if (key == QLatin1String("source")) {
                if (sourceSeen)
                    return fail(lineNumber, QStringLiteral("a lesson has one source"));
                sourceSeen = true;
                static const QRegularExpression templateRe(
                    QStringLiteral("^template\\s+\"([^\"]+)\"\\s+as\\s+(\\S+)$"));
                const QRegularExpressionMatch match = templateRe.match(value);
                if (value.startsWith(QLatin1String("template"))) {
                    if (!match.hasMatch())
                        return fail(lineNumber, QStringLiteral("a template source is "
                                                              "`template \"<label>\" as <name.s>`"));
                    lesson.source.kind = SourceSpec::Template;
                    lesson.source.value = match.captured(1);
                    lesson.source.as = match.captured(2);
                } else if (value.startsWith(QLatin1String("file "))) {
                    lesson.source.kind = SourceSpec::LessonFile;
                    lesson.source.value = value.mid(5).trimmed();
                } else if (value.startsWith(QLatin1String("repo "))) {
                    lesson.source.kind = SourceSpec::RepoFile;
                    lesson.source.value = value.mid(5).trimmed();
                } else {
                    return fail(lineNumber, QStringLiteral("a source is `template \"...\" as x.s`, "
                                                           "`file <path>` or `repo <path>`"));
                }
            } else {
                return fail(lineNumber, QStringLiteral("`%1` is not a lesson key (id, title, level, "
                                                       "needs, learn, source)").arg(key));
            }
            continue;
        }

        // Indented: a directive inside a step. The keyword is matched whole, so
        // a teach line that begins with "see" or "do" is still a teach line.
        if (!haveStep)
            return fail(lineNumber, QStringLiteral("a directive needs a step above it"));
        const QString keyword = trimmed.section(QLatin1Char(' '), 0, 0);
        const QString rest = trimmed.mid(keyword.size()).trimmed();
        if (keyword == QLatin1String("teach")) {
            if (!current.teach.isEmpty())
                return fail(lineNumber, QStringLiteral("one teach per step"));
            current.teach = rest;
            pendingKeyword = QStringLiteral("teach");
        } else if (keyword == QLatin1String("see")) {
            current.see << rest;
            pendingKeyword = QStringLiteral("see");
        } else if (keyword == QLatin1String("nominal")) {
            if (!current.nominal.isEmpty())
                return fail(lineNumber, QStringLiteral("one nominal per step"));
            current.nominal = rest;
            pendingKeyword = QStringLiteral("nominal");
        } else if (keyword == QLatin1String("do")) {
            const QStringList words = rest.split(QRegularExpression(QStringLiteral("\\s+")),
                                                 Qt::SkipEmptyParts);
            if (words.isEmpty())
                return fail(lineNumber, QStringLiteral("do needs a verb"));
            const QString verb = words.first();
            Action action;
            action.verb = verb;
            action.args = words.mid(1);
            bool known = false;
            for (const VerbInfo &info : verbTable()) {
                if (verb != QLatin1String(info.verb))
                    continue;
                known = true;
                if (info.optional ? action.args.size() > info.argc : action.args.size() != info.argc) {
                    return fail(lineNumber, QStringLiteral("`%1` takes %2, %3 given")
                                                .arg(verb)
                                                .arg(info.optional
                                                         ? QStringLiteral("at most %1 argument").arg(info.argc)
                                                         : QStringLiteral("exactly %1 arguments").arg(info.argc))
                                                .arg(action.args.size()));
                }
                break;
            }
            if (!known)
                return fail(lineNumber, QStringLiteral("`%1` is not a lesson verb (%2)")
                                            .arg(verb, actionVerbs().join(QStringLiteral(", "))));
            current.actions << action;
        } else if (keyword == QLatin1String("expect")) {
            Checkpoint checkpoint;
            QString why;
            if (!parseCheckpoint(rest, &checkpoint, &why))
                return fail(lineNumber, why);
            current.expects << checkpoint;
        } else {
            return fail(lineNumber, QStringLiteral("`%1` is not a directive (teach, do, see, "
                                                   "expect, nominal)").arg(keyword));
        }
    }
    if (haveStep)
        finishStep();

    if (lesson.title.isEmpty())
        return fail(0, QStringLiteral("the lesson needs a `title:`"));
    if (lesson.level.isEmpty())
        return fail(0, QStringLiteral("the lesson needs a `level:`"));
    if (lesson.learn.isEmpty())
        return fail(0, QStringLiteral("the lesson needs a `learn:` line saying what it teaches"));
    if (lesson.steps.isEmpty())
        return fail(0, QStringLiteral("the lesson has no steps"));
    if (lesson.id.isEmpty())
        return fail(0, QStringLiteral("the lesson needs an id (or a named directory)"));

    for (int s = 0; s < lesson.steps.size(); ++s) {
        const Step &step = lesson.steps.at(s);
        if (step.teach.isEmpty())
            return fail(0, QStringLiteral("step %1 (`%2`) has no teach line").arg(s + 1).arg(step.title));
        if (step.actions.isEmpty() && step.expects.isEmpty() && step.nominal.isEmpty())
            return fail(0, QStringLiteral("step %1 (`%2`) does nothing and checks nothing").arg(s + 1).arg(step.title));
        if (!step.expects.isEmpty() && !step.nominal.isEmpty())
            return fail(0, QStringLiteral("step %1 (`%2`) has both expect and nominal — say which it is")
                                    .arg(s + 1).arg(step.title));
        if (step.nominal.isEmpty() && step.expects.isEmpty())
            return fail(0, QStringLiteral("step %1 (`%2`) observes something the runner cannot check; "
                                          "add an expect, or a nominal line saying why not")
                                    .arg(s + 1).arg(step.title));
    }

    *out = lesson;
    return true;
}

bool evaluateCheckpoint(const Checkpoint &c, const Snapshot &snapshot, QString *detail)
{
    auto fail = [&](const QString &found) {
        *detail = QStringLiteral("expected %1, found %2").arg(checkpointText(c), found);
        return false;
    };

    switch (c.kind) {
    case Checkpoint::Stopped:
        return snapshot.stopped ? true
                                : fail(snapshot.running ? QStringLiteral("the machine running")
                                                        : QStringLiteral("no session"));
    case Checkpoint::Running:
        return snapshot.running ? true : fail(QStringLiteral("the machine stopped"));
    case Checkpoint::ProblemsNone:
        return snapshot.problems == 0
                   ? true
                   : fail(QStringLiteral("%1 problem(s)").arg(snapshot.problems));
    case Checkpoint::Register: {
        if (!snapshot.registers.contains(c.subject))
            return fail(QStringLiteral("no register %1 (is the machine stopped?)").arg(c.subject));
        quint32 expected = 0;
        parseValue(c.value, &expected);
        const quint32 found = snapshot.registers.value(c.subject);
        bool ok = false;
        if (c.op == QLatin1String("=="))
            ok = found == expected;
        else if (c.op == QLatin1String("!="))
            ok = found != expected;
        else if (c.op == QLatin1String(">="))
            ok = found >= expected;
        else if (c.op == QLatin1String("<="))
            ok = found <= expected;
        else if (c.op == QLatin1String(">"))
            ok = found > expected;
        else if (c.op == QLatin1String("<"))
            ok = found < expected;
        return ok ? true
                  : fail(QStringLiteral("%1 = $%2").arg(c.subject)
                              .arg(found, 0, 16));
    }
    case Checkpoint::Memory: {
        if (!snapshot.memoryValid)
            return fail(QStringLiteral("no memory read (is the machine stopped?)"));
        if (snapshot.memory.size() < c.length)
            return fail(QStringLiteral("%1 byte(s) read").arg(snapshot.memory.size()));
        const QByteArray found = snapshot.memory.left(c.length);
        return found == c.bytes ? true : fail(QStringLiteral("%1").arg(QString::fromLatin1(found.toHex(' '))));
    }
    case Checkpoint::AtLine: {
        if (snapshot.sourceLine <= 0)
            return fail(QStringLiteral("no source line (is the program mapped?)"));
        const QString found = QFileInfo(snapshot.sourceFile).fileName();
        if (found.compare(c.subject, Qt::CaseInsensitive) != 0 || snapshot.sourceLine != c.line)
            return fail(QStringLiteral("%1:%2").arg(found).arg(snapshot.sourceLine));
        return true;
    }
    }
    return false;
}

QStringList lessonRoots(const QString &startDir)
{
    QStringList roots;
    QStringList bases;
    bases << QDir::currentPath();
    if (!startDir.isEmpty())
        bases.prepend(startDir);

    // The executable walks upwards too: run.sh and ctest both start inside the
    // tree, but the binary sits in build/, and a bundle has no cwd relationship
    // to its data at all (WelcomeView::demoProjects() searches the same way).
    QDir up(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 4; ++depth) {
        bases << up.absolutePath();
        if (!up.cdUp())
            break;
    }

    auto consider = [&](const QString &base) {
        const QString candidate = QDir(base).filePath(QStringLiteral("lessons"));
        if (QFileInfo(candidate).isDir() && !roots.contains(candidate))
            roots << candidate;
    };
    for (const QString &base : bases)
        consider(base);

    // The installed layout: share/pist/lessons beside the binary.
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const QString &relative : { QStringLiteral("lessons"),
                                     QStringLiteral("../share/pist/lessons"),
                                     QStringLiteral("../Resources/lessons") })
        consider(appDir.filePath(relative));
    return roots;
}

QVector<Lesson> loadLessons(const QStringList &roots, QStringList *warnings)
{
    QVector<Lesson> lessons;
    QSet<QString> seen;
    for (const QString &root : roots) {
        QDir dir(root);
        const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &entry : entries) {
            const QString path = dir.filePath(entry + QStringLiteral("/lesson.pil"));
            if (!QFileInfo::exists(path))
                continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                if (warnings)
                    *warnings << QStringLiteral("%1 could not be read").arg(path);
                continue;
            }
            Lesson lesson;
            QString error;
            if (!parseLesson(QString::fromUtf8(file.readAll()), QFileInfo(path).absolutePath(),
                             &lesson, &error)) {
                // Kept, with the error, so the panel names the broken file
                // instead of the lesson quietly disappearing from the list.
                Lesson broken;
                broken.dir = QFileInfo(path).absolutePath();
                broken.id = entry;
                broken.title = entry;
                broken.error = error;
                if (!seen.contains(broken.id)) {
                    seen.insert(broken.id);
                    lessons.append(broken);
                }
                if (warnings)
                    *warnings << QStringLiteral("%1: %2").arg(path, error);
                continue;
            }
            if (seen.contains(lesson.id))
                continue;  // a nearer root already supplied this lesson
            seen.insert(lesson.id);
            lessons.append(lesson);
        }
    }
    return lessons;
}

QString defaultLessonId()
{
    return QStringLiteral("first-run");
}

} // namespace lessons
} // namespace pist
