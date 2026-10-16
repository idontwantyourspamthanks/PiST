// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pist {
namespace lessons {

/// Where a lesson's program lives before a step runs it.
///
/// `Template` creates the file from a New File template when the lesson starts
/// (so an installed build needs nothing from the source tree); `LessonFile` is
/// a source the lesson owns, relative to its own directory; `RepoFile` is a
/// demo the repository ships, relative to the repository root, and the lesson
/// is skipped when the tree is absent (docs/tutorial-plan.md §9).
struct SourceSpec
{
    enum Kind { None, Template, LessonFile, RepoFile };

    Kind kind = None;
    QString value;  ///< template label, or a path
    QString as;     ///< the file name a template is created as
};

/// One `do`: an action a step performs, as a verb and its arguments. The verbs
/// are the remote-control vocabulary wherever one exists
/// (docs/tutorial-plan.md §6), so a lesson step is a thing the IDE can already
/// be told to do.
struct Action
{
    QString verb;
    QStringList args;

    /// The action as written, for a step's detail line.
    QString text() const;
};

/// One `expect`: a claim about the machine, which the runner evaluates.
/// Parsed here, evaluated in evaluateCheckpoint() against a Snapshot.
struct Checkpoint
{
    enum Kind { Stopped, Running, ProblemsNone, Register, Memory, AtLine };

    Kind kind = Stopped;
    QString subject;    ///< register name, or the file name of an AtLine
    QString op;         ///< == != >= <= > <    (Register only)
    QString value;      ///< the operand as written, for the verdict text
    quint32 address = 0;
    int length = 0;
    QByteArray bytes;   ///< Memory: the bytes expected at `address`
    int line = 0;       ///< AtLine
};

struct Step
{
    QString title;
    QString teach;      ///< the prose the step shows
    QString nominal;    ///< non-empty: this step is not machine-checkable, and why
    QStringList see;    ///< where the result appears; a hint, not an action
    QVector<Action> actions;
    QVector<Checkpoint> expects;

    /// Whether this step says anything about the machine — a step with neither
    /// an expectation nor a `nominal` is a bug in the lesson, not a lenient step.
    bool declaresOutcome() const { return !expects.isEmpty() || !nominal.isEmpty(); }
};

struct Lesson
{
    QString id;         ///< the directory name unless the file names one
    QString title;
    QString level;
    QStringList needs;  ///< any of: assembler, linker, emulator, rom
    QString learn;      ///< what the lesson teaches, one line
    SourceSpec source;
    QVector<Step> steps;
    QString dir;        ///< the lesson's directory, absolute
    QString error;      ///< why the lesson could not be used (empty when usable)

    bool usable() const { return error.isEmpty() && !steps.isEmpty(); }
};

/// What a checkpoint is evaluated against: a snapshot of the machine, the build
/// and the editor, assembled by the runner from the control surface. Pure data,
/// so the evaluation is unit-testable with no IDE.
struct Snapshot
{
    bool running = false;
    bool stopped = false;
    QHash<QString, quint32> registers;  ///< d0..d7, a0..a7, pc, sr
    int problems = 0;
    QByteArray memory;                  ///< the bytes a Memory checkpoint asked for
    bool memoryValid = false;
    QString sourceFile;                 ///< the file the program counter maps to
    int sourceLine = 0;                 ///< the line it maps to
};

/// Parse one lesson file. `dir` is the directory the file lives in: it supplies
/// the default id and resolves a `file` source. On failure returns false and
/// sets *error to "line N: ..." — a lesson that does not parse is an error, not
/// a lesson with fewer steps, because a silently dropped step is a lesson that
/// teaches less than it claims.
bool parseLesson(const QString &text, const QString &dir, Lesson *out, QString *error);

/// Evaluate one checkpoint. When it fails, *detail says what was found, in the
/// form the panel shows ("expected d0 == 16, found 15").
bool evaluateCheckpoint(const Checkpoint &checkpoint, const Snapshot &snapshot, QString *detail);

/// The value text of a checkpoint, for a lesson author reading the file back.
QString checkpointText(const Checkpoint &checkpoint);

/// The action verbs a lesson may use (validated while parsing).
QStringList actionVerbs();

/// The needs a lesson may declare (validated while parsing).
QStringList knownNeeds();

/// The directories that may hold lessons, nearest first: a `lessons/` under
/// `startDir`, then the same walking up to the repository root (how run.sh and
/// ctest start, and how WelcomeView::demoProjects() finds demo/), then the
/// installed share directory. Empty when none exists.
QStringList lessonRoots(const QString &startDir);

/// Load every `*/lesson.pil` under the roots. A lesson that fails to parse is
/// kept, with `error` set, so the panel can say which file is wrong rather than
/// quietly listing four lessons out of five.
QVector<Lesson> loadLessons(const QStringList &roots, QStringList *warnings);

/// The lesson a fresh install should offer first (docs/tutorial-plan.md §9).
QString defaultLessonId();

} // namespace lessons
} // namespace pist
