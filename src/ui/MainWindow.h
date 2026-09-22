#pragma once

#include <QMainWindow>

#include <memory>

class QLabel;
class QStackedWidget;
class QTimer;
class QWidget;
class StepNavigator;
class StatusBadge;
class CapturePage;
class DatasetPage;
class PreprocessPage;
class RecognitionPage;
class SweepPage;
class TrainPage;
class ICeyearVisaService;
class IUsrpDeviceService;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    MainWindow(std::unique_ptr<ICeyearVisaService> sweepService,
               std::unique_ptr<IUsrpDeviceService> captureUsrpService,
               std::unique_ptr<ICeyearVisaService> captureCeyearService,
               QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi(std::unique_ptr<ICeyearVisaService> sweepService,
                 std::unique_ptr<IUsrpDeviceService> captureUsrpService,
                 std::unique_ptr<ICeyearVisaService> captureCeyearService);
    void updateCurrentStep(int index);
    void updateClock();
    void refreshWorkflowStatuses();

    StepNavigator *m_stepNavigator = nullptr;
    QStackedWidget *m_stack = nullptr;
    SweepPage *m_sweepPage = nullptr;
    CapturePage *m_capturePage = nullptr;
    PreprocessPage *m_preprocessPage = nullptr;
    DatasetPage *m_datasetPage = nullptr;
    TrainPage *m_trainPage = nullptr;
    RecognitionPage *m_recognitionPage = nullptr;
    QLabel *m_currentStepLabel = nullptr;
    QLabel *m_clockLabel = nullptr;
    QLabel *m_dataPathLabel = nullptr;
    StatusBadge *m_deviceBadge = nullptr;
    StatusBadge *m_taskBadge = nullptr;
    QTimer *m_clockTimer = nullptr;
};
