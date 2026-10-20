#pragma once

#include "services/SignalCleaningProcessRunner.h"
#include "ui/pages/WorkflowPage.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QShowEvent;
class MetricCard;
class SpectrogramReviewWidget;

class SignalCleaningPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit SignalCleaningPage(QWidget *parent = nullptr);
    ~SignalCleaningPage() override;

    bool hasResults() const;
    bool isRunning() const;
    bool hasConfiguredModel() const;
    void setInputRootPath(const QString &path);
    void setWorkflowExecutionAllowed(bool allowed, const QString &reason = QString());

signals:
    void requestOpenDataset();
    void workflowStateChanged();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void loadDefaults();
    void loadModelMetadata();
    void browseModel();
    void browseBackend();
    void browseInputRoot();
    void startCleaning();
    void cancelCleaning();
    void openOutputDirectory();
    void updateProgress(int percent, const QString &message);
    void showResult(const SignalCleaningResult &result);
    void rebuildPredictionTable();
    void updateSelectedPrediction();
    void savePredictionsToState(const SignalCleaningResult &result);
    void setBusy(bool busy);
    void updateStartAvailability();
    void setPageMessage(const QString &message, const QString &kind = QString());
    void appendLog(const QString &message);

    QLineEdit *m_modelPathEdit = nullptr;
    QLineEdit *m_backendPathEdit = nullptr;
    QLineEdit *m_inputRootEdit = nullptr;
    QLineEdit *m_outputRootEdit = nullptr;
    QLineEdit *m_thresholdEdit = nullptr;
    QLineEdit *m_batchSizeEdit = nullptr;
    QComboBox *m_predictionFilter = nullptr;
    QCheckBox *m_cpuCheck = nullptr;
    QLabel *m_modelSummaryLabel = nullptr;
    QLabel *m_workflowGateLabel = nullptr;
    QLabel *m_runStateLabel = nullptr;
    QLabel *m_progressDetailLabel = nullptr;
    QLabel *m_pageMessage = nullptr;
    QLabel *m_predictionDetailLabel = nullptr;
    MetricCard *m_totalMetric = nullptr;
    MetricCard *m_signalMetric = nullptr;
    MetricCard *m_noiseMetric = nullptr;
    MetricCard *m_modelledMetric = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTableWidget *m_predictionTable = nullptr;
    QPlainTextEdit *m_logOutput = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_nextDatasetButton = nullptr;
    SpectrogramReviewWidget *m_spectrogramReviewWidget = nullptr;
    SignalCleaningProcessRunner *m_processRunner = nullptr;
    SignalCleaningResult m_latestResult;
    QVector<SignalCleaningPrediction> m_visiblePredictions;
    bool m_defaultsLoaded = false;
    bool m_workflowExecutionAllowed = false;
    bool m_busy = false;
    QString m_workflowBlockingReason;
};
