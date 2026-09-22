#pragma once

#include "core/Records.h"
#include "services/RecognitionProcessRunner.h"
#include "ui/pages/WorkflowPage.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class MetricCard;

class RecognitionPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit RecognitionPage(QWidget *parent = nullptr);
    void setModelForRecognition(const QString &modelPath, const QString &datasetVersionId = QString());
    bool hasUsableModel() const;

protected:
    void showEvent(QShowEvent *event) override;

private:
    void loadModels();
    void loadSamples();
    void selectModelFromCombo();
    void selectSampleFromCombo();
    void chooseSample();
    void runRecognition();
    void handleRecognitionFinished(const RecognitionResult &result);
    void handleRecognitionFailed(const QString &message);
    void setBusy(bool busy);
    void updateRunAvailability();

    QComboBox *m_modelCombo = nullptr;
    QComboBox *m_sampleCombo = nullptr;
    QLineEdit *m_samplePathEdit = nullptr;
    QLabel *m_modelSummary = nullptr;
    QLabel *m_sampleSummary = nullptr;
    QLabel *m_resultDetail = nullptr;
    QLabel *m_topCandidates = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    MetricCard *m_modelMetric = nullptr;
    MetricCard *m_confidenceMetric = nullptr;
    MetricCard *m_resultMetric = nullptr;
    QTableWidget *m_resultTable = nullptr;
    RecognitionProcessRunner *m_runner = nullptr;
    QVector<TrainingRunRecord> m_trainingRuns;
    QVector<SampleRecord> m_samples;
    int m_selectedRunIndex = -1;
    int m_selectedSampleIndex = -1;
    QString m_selectedModelPath;
};
