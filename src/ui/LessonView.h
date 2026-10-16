// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QVector>
#include <QWidget>

#include "lessons/Lesson.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace pist {

namespace lessons {
class LessonRunner;
}

/// The Lesson dock: the step list, the current step's prose, the two step
/// actions and the checkpoint verdict (docs/tutorial-plan.md §8).
///
/// It renders a LessonRunner and nothing else — every button is a slot on the
/// runner, so what the panel does and what the headless test does are the same
/// code path.
class LessonView : public QWidget
{
    Q_OBJECT

public:
    explicit LessonView(lessons::LessonRunner *runner, QWidget *parent = nullptr);

    /// Fill the lesson chooser. A lesson with a parse error is listed disabled,
    /// naming the error, rather than being dropped from the list.
    void setLessons(const QVector<lessons::Lesson> &lessons);

    /// Choose a lesson by id (no-op when it is not loaded) and start it.
    void selectLesson(const QString &id);

    /// The id of the lesson on show, empty when none is.
    QString currentLessonId() const;

signals:
    /// The user asked why the current step matters. The window owns the
    /// reference dock, so it decides what to raise (the step's own `explain`
    /// topic when it has one).
    void whyRequested();

private:
    void render();
    void renderSteps();

    lessons::LessonRunner *m_runner = nullptr;
    QVector<lessons::Lesson> m_lessons;
    QComboBox *m_chooser = nullptr;
    QListWidget *m_steps = nullptr;
    QLabel *m_teach = nullptr;
    QLabel *m_see = nullptr;
    QLabel *m_verdict = nullptr;
    QPushButton *m_show = nullptr;
    QPushButton *m_why = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_restart = nullptr;
};

} // namespace pist
