#pragma once

#include "core/Records.h"
#include "services/TrainingProcessRunner.h"
#include "ui/pages/WorkflowPage.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QShowEvent;
class QTableWidget;
class QGroupBox;
class MetricCard;

class TrainPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit TrainPage(QWidget *parent = nullptr);

signals:
    void modelReadyForRecognition(const QString &modelPath, const QString &datasetVersionId);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void loadDatasetVersions();
    void loadTrainingRuns();
    void updateSelectedVersion();
    void startCachePrecompute();
    void startTraining();
    void cancelTraining();
    void handleTrainingProgress(const TrainingProgress &progress);
    void handleTrainingFinished(const TrainingRunResult &result);
    void handleTrainingFailed(const QString &message);
    void setTrainingBusy(bool busy);
    QString createTrainingOutputDirectory(const QString &runId) const;
    QString cacheDirectoryForSelection() const;
    bool cacheReadyForSelection(QString *reason = nullptr) const;
    void updateFeatureDescription();
    void updateCacheStatus();
    void persistTrainingRun(const TrainingRunResult &result);

    MetricCard *m_datasetMetric = nullptr;
    MetricCard *m_sampleMetric = nullptr;
    MetricCard *m_trainStateMetric = nullptr;
    QLabel *m_emptyState = nullptr;
    QLabel *m_selectedSummary = nullptr;
    QLabel *m_progressDetail = nullptr;
    QLabel *m_outputSummary = nullptr;
    QLabel *m_cacheStatusLabel = nullptr;
    QLabel *m_featureDescriptionLabel = nullptr;
    QTableWidget *m_versionTable = nullptr;
    QTableWidget *m_trainingTable = nullptr;
    QPlainTextEdit *m_logs = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_precomputeCacheButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_openOutputButton = nullptr;
    QPushButton *m_useModelButton = nullptr;
    QPushButton *m_advancedToggle = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QSpinBox *m_epochsSpin = nullptr;
    QSpinBox *m_batchSizeSpin = nullptr;
    QComboBox *m_trainDataModeCombo = nullptr;
    QComboBox *m_modelSizeCombo = nullptr;
    QComboBox *m_featureModeCombo = nullptr;
    QComboBox *m_sampleLengthCombo = nullptr;
    QComboBox *m_modelModeCombo = nullptr;
    QCheckBox *m_deterministicCheck = nullptr;
    QCheckBox *m_overwriteCacheCheck = nullptr;
    QSpinBox *m_precomputeBatchSpin = nullptr;
    QLabel *m_gpuStatusLabel = nullptr;
    QWidget *m_advancedTrainingPanel = nullptr;
    TrainingProcessRunner *m_trainingRunner = nullptr;
    QVector<DatasetVersionRecord> m_versions;
    QVector<TrainingRunRecord> m_trainingRuns;
    int m_selectedVersionIndex = -1;
    QString m_latestModelPath;
    QString m_latestModelDatasetVersionId;
};
