#pragma once

#include "services/PreprocessProcessRunner.h"
#include "ui/pages/WorkflowPage.h"

class QLabel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QThread;
class QProgressBar;
class QScrollArea;
class QPixmap;
class MetricCard;

class PreprocessPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit PreprocessPage(QWidget *parent = nullptr);
    ~PreprocessPage() override;
    void setInputFilePath(const QString &filePath);
    bool isInputConfirmed() const;
    bool isRunning() const;
    bool hasResults() const;

signals:
    void requestOpenDataset();

private:
    void buildUi();
    void previewCap();
    void runPreprocess();
    void cancelPreprocess();
    void showRunResult(const PreprocessRunResult &result);
    void showPartialResult(const PreprocessRunResult &result);
    void updateProgress(const PreprocessProgress &progress);
    void selectResultRow();
    void saveProfile();
    void loadProfile();
    void resetProfile();
    void saveSampleRecords(const QVector<SampleRecord> &records);
    void savePreprocessBatch(const PreprocessRunResult &result);
    void setBusy(bool busy);
    void setValidationMessage(const QString &message, const QString &kind = QStringLiteral("neutral"));
    void setRunState(const QString &message, const QString &state = QStringLiteral("idle"));
    void appendLog(const QString &line);
    void updateCapSummary(const CapProbeResult &result);
    bool buildRequest(const QString &capPath, PreprocessRequest *request, QString *error) const;

    QLineEdit *m_capPathEdit = nullptr;
    QLineEdit *m_outputDirEdit = nullptr;
    QLineEdit *m_windowSamplesEdit = nullptr;
    QLineEdit *m_chunkSamplesEdit = nullptr;
    QLineEdit *m_frequencySpanEdit = nullptr;
    QLineEdit *m_energyOffsetEdit = nullptr;
    QLineEdit *m_targetBandwidthEdit = nullptr;
    QLineEdit *m_minBandwidthEdit = nullptr;
    QLineEdit *m_minWidebandFillEdit = nullptr;
    QLineEdit *m_minWidebandSnrEdit = nullptr;
    QLineEdit *m_morphFrequencyCloseEdit = nullptr;
    QLineEdit *m_shapeSplitQuantileEdit = nullptr;
    QLineEdit *m_shapeSplitMergeGapEdit = nullptr;
    QLineEdit *m_maxSegmentDurationEdit = nullptr;
    QLineEdit *m_bandWindowEdit = nullptr;
    QLineEdit *m_bandWindowOverlapEdit = nullptr;
    QLineEdit *m_minBandScoreEdit = nullptr;
    QLineEdit *m_minDurationEdit = nullptr;
    QLineEdit *m_cvColumnQuantileEdit = nullptr;
    QLineEdit *m_cvOccupancyEdit = nullptr;
    QLineEdit *m_cvMorphFrequencyEdit = nullptr;
    QLineEdit *m_cvMorphTimeEdit = nullptr;
    QLineEdit *m_mergeGapEdit = nullptr;
    QLineEdit *m_npersegEdit = nullptr;
    QLineEdit *m_noverlapEdit = nullptr;
    QLineEdit *m_spectrogramNpersegEdit = nullptr;
    QLineEdit *m_spectrogramNoverlapEdit = nullptr;
    QComboBox *m_detectionModeCombo = nullptr;
    QComboBox *m_payloadEndianCombo = nullptr;
    QCheckBox *m_exportSpectrogramCheck = nullptr;
    QLabel *m_bandwidthLabel = nullptr;
    QLabel *m_sampleRateLabel = nullptr;
    QLabel *m_centerFrequencyLabel = nullptr;
    QLabel *m_versionLabel = nullptr;
    QLabel *m_headerLengthLabel = nullptr;
    QLabel *m_sampleCountLabel = nullptr;
    QLabel *m_validationMessage = nullptr;
    QLabel *m_runStateLabel = nullptr;
    QLabel *m_outputSummaryLabel = nullptr;
    QLabel *m_inputFileSummaryLabel = nullptr;
    QLabel *m_computeBackendLabel = nullptr;
    QLabel *m_progressDetailLabel = nullptr;
    QLabel *m_qualitySummaryLabel = nullptr;
    QLabel *m_detailTitleLabel = nullptr;
    QLabel *m_detailMetricsLabel = nullptr;
    QLabel *m_detailPathsLabel = nullptr;
    QLabel *m_spectrogramPreviewLabel = nullptr;
    QLabel *m_emptyState = nullptr;
    MetricCard *m_capMetric = nullptr;
    MetricCard *m_segmentMetric = nullptr;
    MetricCard *m_sampleMetric = nullptr;
    QPlainTextEdit *m_logOutput = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTabWidget *m_resultsTabs = nullptr;
    QTableWidget *m_segmentTable = nullptr;
    QTableWidget *m_sampleTable = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_nextDatasetButton = nullptr;
    QPushButton *m_saveProfileButton = nullptr;
    QPushButton *m_loadProfileButton = nullptr;
    QPushButton *m_resetProfileButton = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_advancedParamsToggle = nullptr;
    QWidget *m_advancedParamsPanel = nullptr;
    PreprocessProcessRunner *m_processRunner = nullptr;
    PreprocessRunResult m_latestResult;
    bool m_inputConfirmed = false;
};
