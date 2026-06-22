#pragma once

#include <QWidget>

#include <QStringList>
#include <QVector>

class QPushButton;

class StepNavigator : public QWidget
{
    Q_OBJECT

public:
    explicit StepNavigator(QWidget *parent = nullptr);
    void setSteps(const QStringList &steps);
    void setCurrentIndex(int index);
    void setCompletedThrough(int index);

signals:
    void stepSelected(int index);

private:
    void refreshButtonStates();

    QStringList m_steps;
    QVector<QPushButton *> m_buttons;
    int m_currentIndex = 0;
    int m_completedThrough = -1;
};
