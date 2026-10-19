#pragma once

#include "core/Records.h"
#include "services/RecognitionProcessRunner.h"
#include "ui/pages/WorkflowPage.h"

#include <QElapsedTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QShowEvent;
class QTableWidget;
class MetricCard;

class RecognitionPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit RecognitionPage(QWidget *parent = nullptr);
    ~RecognitionPage() override;
    void setModelForRecognition(const QString &modelPath, const QString &datasetVersionId = QString());
    bool hasUsableModel() const;

signals:
    void recognitionCompleted();
    void recognitionInputChanged();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void loadModels();
    void loadSamples();
    void selectModelFromCombo();
    void selectSampleFromCombo();
    void chooseModel();
    void chooseSample();
    void chooseBatchSamples();
    void chooseBatchSampleDirectory();
    void runRecognition();
    void runAllDatasetSamples();
    void retryFailedBatchSamples();
    void exportRecognitionResults();
    void beginBatchRecognition(const QVector<RecognitionRequest> &requests, bool retryingFailures);
    void startNextBatchRecognition();
    void finishBatchRecognition();
    void cancelRecognition();
    void updateBatchProgress();
    void appendRecognitionResult(const RecognitionResult &result);
    void appendRecognitionFailure(const QString &samplePath, const QString &message);
    void handleRecognitionFinished(const RecognitionResult &result);
    void handleRecognitionFailed(const QString &message);
    void setBusy(bool busy);
    void updateRunAvailability();
    void updateProvenance();
    void updateCaptureSegmentSummary();
    QVector<RecognitionRequest> availableBatchRequests() const;

    QComboBox *m_modelCombo = nullptr;
    QComboBox *m_sampleCombo = nullptr;
    QLineEdit *m_samplePathEdit = nullptr;
    QLabel *m_modelSummary = nullptr;
    QLabel *m_sampleSummary = nullptr;
    QLabel *m_batchSampleSummary = nullptr;
    QLabel *m_segmentSummary = nullptr;
    QLabel *m_resultDetail = nullptr;
    QLabel *m_provenanceLabel = nullptr;
    QLabel *m_topCandidates = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_chooseModelButton = nullptr;
    QPushButton *m_chooseSampleButton = nullptr;
    QPushButton *m_chooseBatchSamplesButton = nullptr;
    QPushButton *m_chooseBatchDirectoryButton = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_batchRunButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_retryFailedButton = nullptr;
    QPushButton *m_exportResultsButton = nullptr;
    QProgressBar *m_batchProgress = nullptr;
    MetricCard *m_modelMetric = nullptr;
    MetricCard *m_confidenceMetric = nullptr;
    MetricCard *m_resultMetric = nullptr;
    QTableWidget *m_resultTable = nullptr;
    RecognitionProcessRunner *m_runner = nullptr;
    QVector<TrainingRunRecord> m_trainingRuns;
    QVector<SampleRecord> m_samples;
    int m_selectedRunIndex = -1;
    int m_selectedSampleIndex = -1;
    int m_selectedSampleNpzIndex = 0;
    QString m_selectedModelPath;
    QStringList m_externalBatchSamplePaths;
    QString m_batchSegmentDirectory;
    QVector<RecognitionRequest> m_batchRequests;
    QVector<RecognitionRequest> m_failedBatchRequests;
    QVector<RecognitionResult> m_captureSegmentResults;
    int m_batchCurrentIndex = -1;
    int m_batchSuccessCount = 0;
    int m_batchFailureCount = 0;
    double m_batchHighestConfidence = 0.0;
    int m_captureSegmentExpectedWindows = 0;
    QString m_batchCurrentFailureMessage;
    QElapsedTimer m_batchElapsedTimer;
    bool m_batchActive = false;
    bool m_batchCancelled = false;
};
