#pragma once

#include <QMainWindow>

class QLabel;
class QStackedWidget;
class StepNavigator;
class StatusBadge;
class CapturePage;
class DatasetPage;
class PreprocessPage;
class RecognitionPage;
class TrainPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void buildUi();
    void updateCurrentStep(int index);
    void updateClock();

    StepNavigator *m_stepNavigator = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_currentStepLabel = nullptr;
    QLabel *m_clockLabel = nullptr;
    QLabel *m_dataPathLabel = nullptr;
    StatusBadge *m_deviceBadge = nullptr;
    StatusBadge *m_taskBadge = nullptr;
};
