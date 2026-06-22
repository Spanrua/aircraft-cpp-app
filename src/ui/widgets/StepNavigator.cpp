#include "ui/widgets/StepNavigator.h"

#include <QHBoxLayout>
#include <QLayoutItem>
#include <QPushButton>
#include <QStyle>

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
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (int index = 0; index < steps.size(); ++index) {
        auto *button = new QPushButton(QStringLiteral("%1  %2").arg(index + 1).arg(steps.at(index)), this);
        button->setCheckable(true);
        button->setMinimumHeight(44);
        button->setProperty("stepButton", true);
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

void StepNavigator::refreshButtonStates()
{
    for (int i = 0; i < m_buttons.size(); ++i) {
        auto *button = m_buttons.at(i);
        const bool isCurrent = i == m_currentIndex;
        const bool isCompleted = i <= m_completedThrough && !isCurrent;
        button->setChecked(isCurrent);
        button->setProperty("completed", isCompleted);
        button->setProperty("pending", i > m_currentIndex);
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
}
