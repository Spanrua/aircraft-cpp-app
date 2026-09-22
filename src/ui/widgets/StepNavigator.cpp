#include "ui/widgets/StepNavigator.h"

#include <QHBoxLayout>
#include <QLayoutItem>
#include <QPushButton>
#include <QStyle>
#include <QVariant>

StepNavigator::StepNavigator(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("StepNavigator"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
}

void StepNavigator::setSteps(const QStringList &steps)
{
    auto *layout = qobject_cast<QHBoxLayout *>(this->layout());
    if (!layout) {
        return;
    }

    m_steps = steps;
    m_buttons.clear();
    m_states = QVector<WorkflowStepState>(steps.size(), WorkflowStepState::NotStarted);
    m_summaries = QVector<QString>(steps.size());
    m_blockingReasons = QVector<QString>(steps.size());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    layout->setSpacing(6);

    for (int index = 0; index < steps.size(); ++index) {
        auto *button = new QPushButton(QStringLiteral("%1  %2").arg(index + 1).arg(steps.at(index)), this);
        button->setCheckable(true);
        button->setMinimumSize(0, 40);
        button->setProperty("stepButton", QVariant(true));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(button, &QPushButton::clicked, this, [this, index]() { emit stepSelected(index); });
        layout->addWidget(button);
        m_buttons.append(button);
    }
    refreshButtonStates();
}

void StepNavigator::setCurrentIndex(int index)
{
    m_currentIndex = index;
    m_completedThrough = qMax(m_completedThrough, index - 1);
    refreshButtonStates();
}

void StepNavigator::setCompletedThrough(int index)
{
    m_completedThrough = index;
    refreshButtonStates();
}

void StepNavigator::setStepStatus(int index, WorkflowStepState state, const QString &summary,
                                  const QString &blockingReason)
{
    if (index < 0 || index >= m_buttons.size()) {
        return;
    }
    m_states[index] = state;
    m_summaries[index] = summary;
    m_blockingReasons[index] = blockingReason;
    refreshButtonStates();
}

WorkflowStepState StepNavigator::stepState(int index) const
{
    if (index < 0 || index >= m_states.size()) {
        return WorkflowStepState::NotStarted;
    }
    return m_states.at(index);
}

QString StepNavigator::stepSummary(int index) const
{
    if (index < 0 || index >= m_summaries.size()) {
        return {};
    }
    return m_summaries.at(index);
}

QString StepNavigator::stepBlockingReason(int index) const
{
    if (index < 0 || index >= m_blockingReasons.size()) {
        return {};
    }
    return m_blockingReasons.at(index);
}

void StepNavigator::refreshButtonStates()
{
    for (int i = 0; i < m_buttons.size(); ++i) {
        auto *button = m_buttons.at(i);
        const bool isCurrent = i == m_currentIndex;
        const bool isCompleted = i <= m_completedThrough && !isCurrent;
        button->setChecked(isCurrent);
        button->setProperty("completed", QVariant(isCompleted));
        button->setProperty("pending", QVariant(i > m_currentIndex));
        const WorkflowStepState state = i < m_states.size() ? m_states.at(i) : WorkflowStepState::NotStarted;
        button->setProperty("workflowState", workflowStepStateKey(state));
        QString tooltip = i < m_summaries.size() ? m_summaries.at(i) : QString();
        if (state == WorkflowStepState::Blocked && i < m_blockingReasons.size()
            && !m_blockingReasons.at(i).isEmpty()) {
            tooltip = m_blockingReasons.at(i);
        }
        button->setToolTip(tooltip);
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
}
