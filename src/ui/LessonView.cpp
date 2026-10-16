// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/LessonView.h"

#include "lessons/LessonRunner.h"
#include "ui/Appearance.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace pist {

LessonView::LessonView(lessons::LessonRunner *runner, QWidget *parent)
    : QWidget(parent), m_runner(runner)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_chooser = new QComboBox(this);
    m_chooser->setObjectName(QStringLiteral("lessonChooser"));
    m_chooser->setToolTip(tr("Which lesson to run"));
    layout->addWidget(m_chooser);

    m_steps = new QListWidget(this);
    m_steps->setObjectName(QStringLiteral("lessonSteps"));
    m_steps->setMaximumHeight(140);
    m_steps->setToolTip(tr("The lesson's steps, in order"));
    layout->addWidget(m_steps);

    m_teach = new QLabel(this);
    m_teach->setObjectName(QStringLiteral("lessonTeach"));
    m_teach->setWordWrap(true);
    m_teach->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_teach->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(m_teach, 1);

    m_see = new QLabel(this);
    m_see->setObjectName(QStringLiteral("lessonSee"));
    m_see->setWordWrap(true);
    layout->addWidget(m_see);

    auto *actions = new QHBoxLayout;
    m_show = new QPushButton(tr("Show me"), this);
    m_show->setObjectName(QStringLiteral("lessonShow"));
    m_show->setToolTip(tr("Perform this step's actions"));
    m_why = new QPushButton(tr("Why?"), this);
    m_why->setObjectName(QStringLiteral("lessonWhy"));
    m_why->setToolTip(tr("Open the instruction and OS-call reference on this step's subject"));
    actions->addWidget(m_show);
    actions->addWidget(m_why);
    actions->addStretch(1);
    layout->addLayout(actions);

    m_verdict = new QLabel(this);
    m_verdict->setObjectName(QStringLiteral("lessonVerdict"));
    m_verdict->setWordWrap(true);
    layout->addWidget(m_verdict);

    auto *navigation = new QHBoxLayout;
    m_back = new QPushButton(tr("Back"), this);
    m_back->setObjectName(QStringLiteral("lessonBack"));
    m_next = new QPushButton(tr("Next"), this);
    m_next->setObjectName(QStringLiteral("lessonNext"));
    m_restart = new QPushButton(tr("Restart lesson"), this);
    m_restart->setObjectName(QStringLiteral("lessonRestart"));
    navigation->addWidget(m_back);
    navigation->addWidget(m_next);
    navigation->addStretch(1);
    navigation->addWidget(m_restart);
    layout->addLayout(navigation);

    connect(m_runner, &lessons::LessonRunner::changed, this, &LessonView::render);
    connect(m_runner, &lessons::LessonRunner::failed, this, [this](const QString &) { render(); });
    connect(m_show, &QPushButton::clicked, m_runner, &lessons::LessonRunner::runStep);
    connect(m_why, &QPushButton::clicked, this, &LessonView::whyRequested);
    connect(m_back, &QPushButton::clicked, this,
            [this]() { m_runner->goToStep(m_runner->index() - 1); });
    connect(m_next, &QPushButton::clicked, this,
            [this]() { m_runner->goToStep(m_runner->index() + 1); });
    connect(m_restart, &QPushButton::clicked, m_runner, &lessons::LessonRunner::restart);
    connect(m_chooser, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0)
            return;
        const QString id = m_chooser->itemData(index).toString();
        if (id.isEmpty() || id == m_runner->lesson().id)
            return;
        selectLesson(id);
    });

    render();
}

void LessonView::setLessons(const QVector<lessons::Lesson> &lessons)
{
    m_lessons = lessons;
    QSignalBlocker blocker(m_chooser);
    m_chooser->clear();
    for (const lessons::Lesson &lesson : lessons) {
        m_chooser->addItem(lesson.title, lesson.id);
        const int row = m_chooser->count() - 1;
        if (!lesson.usable()) {
            // Listed, not dropped: the panel names the broken file instead of
            // the lesson quietly disappearing from the list.
            m_chooser->setItemData(row, lesson.error, Qt::ToolTipRole);
            m_chooser->setItemData(row, QVariant(0), Qt::UserRole - 1);  // disabled
        }
    }
    m_chooser->setCurrentIndex(-1);
}

QString LessonView::currentLessonId() const
{
    return m_runner ? m_runner->lesson().id : QString();
}

void LessonView::selectLesson(const QString &id)
{
    for (const lessons::Lesson &lesson : m_lessons) {
        if (lesson.id != id)
            continue;
        const int row = m_chooser->findData(id);
        if (row >= 0)
            m_chooser->setCurrentIndex(row);
        m_runner->setLesson(lesson);
        m_runner->start();
        render();
        return;
    }
}

void LessonView::renderSteps()
{
    QSignalBlocker blocker(m_steps);
    m_steps->clear();
    const lessons::Lesson &lesson = m_runner->lesson();
    for (int i = 0; i < lesson.steps.size(); ++i) {
        const lessons::Step &step = lesson.steps.at(i);
        const QString mark = i < m_runner->index() ? QStringLiteral("✓")
                             : i == m_runner->index() ? QStringLiteral("▸")
                                                      : QStringLiteral(" ");
        m_steps->addItem(QStringLiteral("%1 %2. %3").arg(mark).arg(i + 1).arg(step.title));
        if (i == m_runner->index())
            m_steps->setCurrentRow(i);
    }
}

void LessonView::render()
{
    const lessons::Lesson &lesson = m_runner->lesson();
    renderSteps();

    const lessons::Step *step = m_runner->currentStep();
    m_teach->setText(step ? m_runner->teachText(m_runner->index())
                          : (lesson.usable() ? tr("Choose a lesson above.")
                                             : lesson.error));
    m_see->setText(step && !step->see.isEmpty() ? tr("See: %1").arg(step->see.join(QStringLiteral(" ")))
                                                : QString());

    if (!m_runner->verdictKnown() || m_runner->verdictText().isEmpty())
        m_verdict->clear();
    else if (step && !step->nominal.isEmpty())
        m_verdict->setText(QStringLiteral("— %1").arg(m_runner->verdictText()));
    else
        m_verdict->setText(QStringLiteral("%1 %2").arg(m_runner->verdictPassed() ? QStringLiteral("✓")
                                                                                : QStringLiteral("✗"),
                                                       m_runner->verdictText()));

    const bool have = step != nullptr;
    m_show->setEnabled(have && !m_runner->busy() && step->actions.size() > 0);
    m_why->setEnabled(have);
    m_back->setEnabled(m_runner->index() > 0);
    m_next->setEnabled(have && m_runner->index() + 1 < m_runner->count());
    m_restart->setEnabled(!lesson.id.isEmpty());
    m_steps->setEnabled(have);
    m_chooser->setEnabled(true);
}

} // namespace pist
