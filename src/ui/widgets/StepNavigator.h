#pragma once

#include <QWidget>

#include <QStringList>
#include <QVector>

#include "ui/WorkflowState.h"

class QPushButton;

class StepNavigator : public QWidget
{
    Q_OBJECT

public:
    explicit StepNavigator(QWidget *parent = nullptr);
    void setSteps(const QStringList &steps);
    void setCurrentIndex(int index);
    void setCompletedThrough(int index);
    void setStepStatus(int index, WorkflowStepState state, const QString &summary = QString(),
                       const QString &blockingReason = QString());
    WorkflowStepState stepState(int index) const;
    QString stepSummary(int index) const;
    QString stepBlockingReason(int index) const;

signals:
    void stepSelected(int index);

private:
    void refreshButtonStates();

    QStringList m_steps;
    QVector<QPushButton *> m_buttons;
    QVector<WorkflowStepState> m_states;
    QVector<QString> m_summaries;
    QVector<QString> m_blockingReasons;
    int m_currentIndex = 0;
    int m_completedThrough = -1;
};
