#pragma once

#include "services/PreprocessProcessRunner.h"
#include "ui/pages/WorkflowPage.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QThread;
class MetricCard;

class PreprocessPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit PreprocessPage(QWidget *parent = nullptr);
    ~PreprocessPage() override;

private:
    void buildUi();
    void previewCap();
    void runPreprocess();
    void runDemoPreprocess();
    void showRunResult(const PreprocessRunResult &result);
    void saveSampleRecords(const QVector<SampleRecord> &records);
    void setBusy(bool busy);
    void setValidationMessage(const QString &message, const QString &kind = QStringLiteral("neutral"));
    void setRunState(const QString &message, const QString &state = QStringLiteral("idle"));
    void appendLog(const QString &line);
    void updateCapSummary(const CapProbeResult &result);
    PreprocessRequest buildRequest(const QString &capPath) const;

    QLineEdit *m_capPathEdit = nullptr;
    QLineEdit *m_outputDirEdit = nullptr;
    QLineEdit *m_modelPathEdit = nullptr;
    QLineEdit *m_sliceLengthEdit = nullptr;
    QLineEdit *m_energyThresholdEdit = nullptr;
    QLineEdit *m_noiseFloorEdit = nullptr;
    QLineEdit *m_minBandwidthEdit = nullptr;
    QLineEdit *m_minDurationEdit = nullptr;
    QLineEdit *m_aiConfidenceEdit = nullptr;
    QLabel *m_bandwidthLabel = nullptr;
    QLabel *m_sampleRateLabel = nullptr;
    QLabel *m_centerFrequencyLabel = nullptr;
    QLabel *m_versionLabel = nullptr;
    QLabel *m_headerLengthLabel = nullptr;
    QLabel *m_sampleCountLabel = nullptr;
    QLabel *m_validationMessage = nullptr;
    QLabel *m_runStateLabel = nullptr;
    QLabel *m_outputSummaryLabel = nullptr;
    QLabel *m_emptyState = nullptr;
    MetricCard *m_capMetric = nullptr;
    MetricCard *m_segmentMetric = nullptr;
    MetricCard *m_sampleMetric = nullptr;
    QPlainTextEdit *m_logOutput = nullptr;
    QTabWidget *m_resultsTabs = nullptr;
    QTableWidget *m_segmentTable = nullptr;
    QTableWidget *m_sampleTable = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_demoButton = nullptr;
    QPushButton *m_advancedParamsToggle = nullptr;
    QWidget *m_advancedParamsPanel = nullptr;
    QThread *m_preprocessThread = nullptr;
};
