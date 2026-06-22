#pragma once

#include "services/UsrpDeviceService.h"
#include "ui/pages/WorkflowPage.h"

#include <memory>

class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QThread;
class MetricCard;

class CapturePage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit CapturePage(QWidget *parent = nullptr);
    explicit CapturePage(std::unique_ptr<IUsrpDeviceService> usrpService, QWidget *parent = nullptr);
    ~CapturePage() override;

private slots:
    void discoverDevices();
    void startCapture();
    void updateDeviceTypeUi();

private:
    void buildUi();
    QWidget *buildUsrpConfigPanel();
    QWidget *buildReservedDevicePanel();
    QString validateParameters(QString *fieldName = nullptr) const;
    void startCaptureAsync(const UsrpCaptureConfig &config);
    void appendLog(const QString &line);
    void appendValidationMessage(const QString &line, const QString &kind = QStringLiteral("neutral"));
    void updateConnectionSummary();
    void setBusy(bool busy);
    void finalizeCapture(const UsrpCaptureResult &result, const UsrpCaptureConfig &config);
    bool isUsrpSelected() const;
    double readComboDouble(const QComboBox *combo, double fallback) const;
    int readComboInt(const QComboBox *combo, int fallback) const;
    QString readComboText(const QComboBox *combo, const QString &fallback) const;
    QString nextCapturePath() const;

    std::unique_ptr<IUsrpDeviceService> m_usrpService;
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
    QWidget *m_advancedParamsPanel = nullptr;
    QLabel *m_3943bPlaceholder = nullptr;
    QLabel *m_editableParamsHint = nullptr;
    QLabel *m_connectionSummary = nullptr;
    QLabel *m_validationMessage = nullptr;
    QLabel *m_taskStateLabel = nullptr;
    QLabel *m_fileStateLabel = nullptr;
    QLineEdit *m_outputDirEdit = nullptr;
    QLineEdit *m_deviceArgsEdit = nullptr;
    QComboBox *m_channelCombo = nullptr;
    QComboBox *m_antennaCombo = nullptr;
    QComboBox *m_centerFreqCombo = nullptr;
    QComboBox *m_sampleRateCombo = nullptr;
    QComboBox *m_bandwidthCombo = nullptr;
    QComboBox *m_gainCombo = nullptr;
    QComboBox *m_durationCombo = nullptr;
    QComboBox *m_cpuFormatCombo = nullptr;
    QComboBox *m_wireFormatCombo = nullptr;
    QTabWidget *m_resultsTabs = nullptr;
    QTableWidget *m_deviceTable = nullptr;
    QTableWidget *m_captureTable = nullptr;
    QPlainTextEdit *m_logOutput = nullptr;
    QThread *m_captureThread = nullptr;
};
