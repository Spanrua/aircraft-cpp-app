#pragma once

#include "services/CeyearVisaService.h"
#include "services/UsrpDeviceService.h"
#include "ui/pages/WorkflowPage.h"

#include <QElapsedTimer>

#include <memory>

class QLabel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QThread;
class QTimer;
class MetricCard;

class CapturePage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit CapturePage(QWidget *parent = nullptr);
    explicit CapturePage(std::unique_ptr<IUsrpDeviceService> usrpService, QWidget *parent = nullptr);
    CapturePage(std::unique_ptr<IUsrpDeviceService> usrpService,
                std::unique_ptr<ICeyearVisaService> ceyearService,
                QWidget *parent = nullptr);
    ~CapturePage() override;

    void setPresetCenterFrequencyHz(double frequencyHz);

signals:
    void captureFileReady(const QString &filePath);

private slots:
    void discoverDevices();
    void startCapture();
    void updateDeviceTypeUi();

private:
    void buildUi();
    QWidget *buildUsrpConfigPanel();
    QWidget *buildReservedDevicePanel();
    QWidget *buildCeyearControlPanel();
    QString validateParameters(QString *fieldName = nullptr) const;
    QString validateCeyearControl(CeyearControlConfig *config) const;
    QString validateCeyearCapture(CeyearCaptureConfig *config) const;
    bool confirmCeyearContinuousCapture(const CeyearCaptureConfig &config);
    bool startCeyearCaptureFromUi(bool continuous, bool confirmContinuousStart = false);
    void startCaptureAsync(const UsrpCaptureConfig &config);
    void startCeyearCaptureAsync(const CeyearCaptureConfig &config);
    void scheduleNextCeyearContinuousCapture();
    void cancelCeyearContinuousCapture();
    void stopCeyearCaptureAsync();
    void queryCeyearStatusAsync();
    void appendLog(const QString &line);
    void appendValidationMessage(const QString &line, const QString &kind = QStringLiteral("neutral"));
    void updateConnectionSummary();
    void setBusy(bool busy);
    void beginCaptureEventSession();
    void recordCaptureEvent(const QString &eventName);
    void setCaptureEventFilePath(const QString &filePath);
    void endCaptureEventSession(bool success);
    void updateCaptureEventStatus();
    void finalizeCapture(const UsrpCaptureResult &result, const UsrpCaptureConfig &config);
    void finalizeCeyearStatus(const CeyearStatusResult &result);
    void finalizeCeyearCapture(const CeyearCaptureResult &result, const CeyearCaptureConfig &config);
    bool isUsrpSelected() const;
    double readComboDouble(const QComboBox *combo, double fallback) const;
    int readComboInt(const QComboBox *combo, int fallback) const;
    QString readComboText(const QComboBox *combo, const QString &fallback) const;
    QString nextCapturePath() const;

    std::unique_ptr<IUsrpDeviceService> m_usrpService;
    std::unique_ptr<ICeyearVisaService> m_ceyearService;
    QVector<UsrpDeviceInfo> m_devices;
    MetricCard *m_deviceMetric = nullptr;
    MetricCard *m_fileMetric = nullptr;
    MetricCard *m_taskMetric = nullptr;
    QPushButton *m_discoverButton = nullptr;
    QPushButton *m_captureButton = nullptr;
    QPushButton *m_usrpSourceButton = nullptr;
    QPushButton *m_3943bSourceButton = nullptr;
    QPushButton *m_advancedParamsToggle = nullptr;
    QComboBox *m_deviceTypeCombo = nullptr;
    QStackedWidget *m_deviceConfigStack = nullptr;
    QStackedWidget *m_parameterStack = nullptr;
    QWidget *m_usrpParameterPanel = nullptr;
    QWidget *m_ceyearControlPanel = nullptr;
    QWidget *m_advancedParamsPanel = nullptr;
    QLabel *m_3943bPlaceholder = nullptr;
    QLabel *m_editableParamsHint = nullptr;
    QLabel *m_usrpParameterModeLabel = nullptr;
    QLabel *m_connectionSummary = nullptr;
    QLabel *m_ceyearReadinessLabel = nullptr;
    QLabel *m_validationMessage = nullptr;
    QLabel *m_taskStateLabel = nullptr;
    QLabel *m_fileStateLabel = nullptr;
    QLabel *m_captureElapsedLabel = nullptr;
    QLineEdit *m_outputDirEdit = nullptr;
    QLineEdit *m_deviceArgsEdit = nullptr;
    QLineEdit *m_ceyearResourceEdit = nullptr;
    QComboBox *m_ceyearPresetCombo = nullptr;
    QLineEdit *m_ceyearCenterFrequencyEdit = nullptr;
    QLineEdit *m_ceyearFrequencySpanEdit = nullptr;
    QComboBox *m_ceyearDemodulationCombo = nullptr;
    QComboBox *m_ceyearDetectorCombo = nullptr;
    QComboBox *m_ceyearMeasureModeCombo = nullptr;
    QLineEdit *m_ceyearIqPointCountEdit = nullptr;
    QComboBox *m_ceyearRecordSourceCombo = nullptr;
    QComboBox *m_ceyearReportPathCombo = nullptr;
    QComboBox *m_ceyearPcReceiveProtocolCombo = nullptr;
    QLineEdit *m_ceyearPcReceiveIpEdit = nullptr;
    QLineEdit *m_ceyearPcReceivePortEdit = nullptr;
    QLineEdit *m_ceyearPcOutputFileEdit = nullptr;
    QCheckBox *m_ceyearContinuousCaptureCheck = nullptr;
    QComboBox *m_ceyearTargetModelCombo = nullptr;
    QComboBox *m_ceyearModelIndexCombo = nullptr;
    QComboBox *m_ceyearDistanceCombo = nullptr;
    QComboBox *m_ceyearHeightCombo = nullptr;
    QComboBox *m_ceyearBandCombo = nullptr;
    QLineEdit *m_ceyearSegmentEdit = nullptr;
    QCheckBox *m_ceyearRecordEnableCheck = nullptr;
    QPushButton *m_stopCeyearCaptureButton = nullptr;
    QPushButton *m_copyDiagnosticsButton = nullptr;
    QComboBox *m_channelCombo = nullptr;
    QComboBox *m_antennaCombo = nullptr;
    QComboBox *m_centerFreqCombo = nullptr;
    QComboBox *m_sampleRateCombo = nullptr;
    QComboBox *m_gainCombo = nullptr;
    QComboBox *m_durationCombo = nullptr;
    QComboBox *m_cpuFormatCombo = nullptr;
    QComboBox *m_wireFormatCombo = nullptr;
    QTabWidget *m_resultsTabs = nullptr;
    QTableWidget *m_deviceTable = nullptr;
    QTableWidget *m_captureTable = nullptr;
    QTableWidget *m_ceyearStatusTable = nullptr;
    QPlainTextEdit *m_logOutput = nullptr;
    QThread *m_captureThread = nullptr;
    QThread *m_ceyearThread = nullptr;
    QTimer *m_ceyearContinuousTimer = nullptr;
    QTimer *m_captureEventUiTimer = nullptr;
    QElapsedTimer m_ceyearCurrentCaptureTimer;
    QElapsedTimer m_captureEventTimer;
    int m_ceyearContinuousRound = 0;
    int m_ceyearContinuousRetryCount = 0;
    bool m_ceyearRetryCurrentRound = false;
    bool m_ceyearBusy = false;
    bool m_ceyearContinuousActive = false;
    bool m_captureEventActive = false;
    QString m_captureEventFilePath;
    QString m_ceyearLastStatusText = QStringLiteral("未查询");
    QString m_ceyearLastStatusAt;
};
