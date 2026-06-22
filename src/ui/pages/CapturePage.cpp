#include "ui/pages/CapturePage.h"

#include "app/ProjectPaths.h"
#include "ui/widgets/MetricCard.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QVBoxLayout>

#include <limits>
#include <cmath>
#include <utility>

namespace
{
constexpr int kUsrpConfigIndex = 0;
constexpr int kReserved3943BConfigIndex = 1;

QTableWidgetItem *readonlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

void refreshWidgetStyle(QWidget *widget)
{
    if (!widget) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QComboBox *editableCombo(const QString &objectName, const QStringList &values, const QString &current, QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setObjectName(objectName);
    combo->setEditable(true);
    combo->addItems(values);
    combo->setCurrentText(current);
    combo->setMinimumHeight(36);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return combo;
}

QWidget *fieldBlock(const QString &label, QWidget *editor, QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *caption = new QLabel(label, container);
    caption->setProperty("muted", true);
    layout->addWidget(caption);
    layout->addWidget(editor);
    return container;
}

bool parsePositiveDouble(const QComboBox *combo, const QString &fieldName, QString *error)
{
    bool ok = false;
    const double value = combo ? combo->currentText().trimmed().toDouble(&ok) : 0.0;
    if (!ok || value <= 0.0 || !std::isfinite(value)) {
        if (error) {
            *error = QStringLiteral("%1 必须是大于 0 的数字。").arg(fieldName);
        }
        return false;
    }
    return true;
}

bool parseNonNegativeDouble(const QComboBox *combo, const QString &fieldName, QString *error)
{
    bool ok = false;
    const double value = combo ? combo->currentText().trimmed().toDouble(&ok) : 0.0;
    if (!ok || value < 0.0 || !std::isfinite(value)) {
        if (error) {
            *error = QStringLiteral("%1 必须是非负数字。").arg(fieldName);
        }
        return false;
    }
    return true;
}

bool parseNonNegativeInt(const QComboBox *combo, const QString &fieldName, QString *error)
{
    bool ok = false;
    const int value = combo ? combo->currentText().trimmed().toInt(&ok) : -1;
    if (!ok || value < 0) {
        if (error) {
            *error = QStringLiteral("%1 必须是非负整数。").arg(fieldName);
        }
        return false;
    }
    return true;
}

} // namespace

CapturePage::CapturePage(QWidget *parent)
    : CapturePage(std::make_unique<UsrpDeviceService>(), parent)
{
}

CapturePage::CapturePage(std::unique_ptr<IUsrpDeviceService> usrpService, QWidget *parent)
    : WorkflowPage(QStringLiteral("数据采集"), QStringLiteral("选择采集设备，配置射频参数，并写入 CAP 原始数据文件。"), parent, true)
    , m_usrpService(std::move(usrpService))
{
    buildUi();
    updateDeviceTypeUi();
}

CapturePage::~CapturePage()
{
    if (m_captureThread) {
        m_captureThread->quit();
        m_captureThread->wait();
    }
}

void CapturePage::buildUi()
{
    if (auto *scrollContent = findChild<QWidget *>(QStringLiteral("WorkflowPageScrollContent"))) {
        scrollContent->setMinimumHeight(720);
    }

    auto *workbench = new QHBoxLayout();
    workbench->setSpacing(14);

    auto *deviceColumn = new QGroupBox(QStringLiteral("设备"), this);
    deviceColumn->setObjectName(QStringLiteral("CaptureDeviceColumn"));
    deviceColumn->setMinimumWidth(260);
    auto *deviceLayout = new QVBoxLayout(deviceColumn);
    deviceLayout->setContentsMargins(16, 20, 16, 16);
    deviceLayout->setSpacing(12);

    m_deviceTypeCombo = new QComboBox(deviceColumn);
    m_deviceTypeCombo->setObjectName(QStringLiteral("CaptureDeviceTypeCombo"));
    m_deviceTypeCombo->addItems({QStringLiteral("USRP B210"), QStringLiteral("3943B（预留）")});
    m_deviceTypeCombo->setVisible(false);

    auto *sourceLabel = new QLabel(QStringLiteral("采集源"), deviceColumn);
    sourceLabel->setProperty("muted", true);
    deviceLayout->addWidget(sourceLabel);

    m_usrpSourceButton = new QPushButton(QStringLiteral("USRP B210"), deviceColumn);
    m_usrpSourceButton->setObjectName(QStringLiteral("UsrpSourceButton"));
    m_usrpSourceButton->setCheckable(true);
    m_usrpSourceButton->setChecked(true);
    m_usrpSourceButton->setProperty("primary", true);
    m_3943bSourceButton = new QPushButton(QStringLiteral("3943B 预留"), deviceColumn);
    m_3943bSourceButton->setObjectName(QStringLiteral("Device3943BSourceButton"));
    m_3943bSourceButton->setCheckable(true);
    deviceLayout->addWidget(m_usrpSourceButton);
    deviceLayout->addWidget(m_3943bSourceButton);
    deviceLayout->addWidget(m_deviceTypeCombo);

    m_discoverButton = new QPushButton(QStringLiteral("检查设备"), deviceColumn);
    m_discoverButton->setObjectName(QStringLiteral("DiscoverUsrpButton"));
    m_discoverButton->setMinimumHeight(38);
    deviceLayout->addWidget(m_discoverButton);

    m_connectionSummary = new QLabel(deviceColumn);
    m_connectionSummary->setObjectName(QStringLiteral("UsrpConnectionSummary"));
    m_connectionSummary->setWordWrap(true);
    m_connectionSummary->setProperty("summary", true);
    deviceLayout->addWidget(m_connectionSummary);

    m_deviceMetric = new MetricCard(QStringLiteral("设备状态"), deviceColumn);
    m_deviceMetric->setValue(QStringLiteral("未检查"));
    m_fileMetric = new MetricCard(QStringLiteral("采集文件"), deviceColumn);
    m_fileMetric->setValue(QStringLiteral("0"));
    deviceLayout->addWidget(m_deviceMetric);
    deviceLayout->addWidget(m_fileMetric);

    m_deviceConfigStack = new QStackedWidget(deviceColumn);
    m_deviceConfigStack->setObjectName(QStringLiteral("DeviceConfigStack"));
    m_deviceConfigStack->addWidget(buildUsrpConfigPanel());
    m_deviceConfigStack->addWidget(buildReservedDevicePanel());
    m_deviceConfigStack->setMaximumHeight(130);
    deviceLayout->addWidget(m_deviceConfigStack);
    deviceLayout->addStretch();
    workbench->addWidget(deviceColumn, 2);

    auto *parametersColumn = new QGroupBox(QStringLiteral("USRP 参数"), this);
    parametersColumn->setObjectName(QStringLiteral("CaptureParametersColumn"));
    auto *parametersLayout = new QVBoxLayout(parametersColumn);
    parametersLayout->setContentsMargins(16, 20, 16, 16);
    parametersLayout->setSpacing(12);

    m_editableParamsHint = new QLabel(QStringLiteral("下拉项是常用预设，也可以直接输入 UHD 支持的数值。你的天线接在 RF-A TX/RX，默认使用通道 0 + TX/RX。"), parametersColumn);
    m_editableParamsHint->setObjectName(QStringLiteral("UsrpEditableParametersHint"));
    m_editableParamsHint->setWordWrap(true);
    m_editableParamsHint->setProperty("muted", true);
    parametersLayout->addWidget(m_editableParamsHint);

    m_deviceArgsEdit = new QLineEdit(parametersColumn);
    m_deviceArgsEdit->setObjectName(QStringLiteral("UsrpDeviceArgsEdit"));
    m_deviceArgsEdit->setPlaceholderText(QStringLiteral("留空为自动发现，例如 serial=30FB59C"));
    m_deviceArgsEdit->setMinimumHeight(36);

    m_channelCombo = editableCombo(QStringLiteral("UsrpChannelCombo"), {QStringLiteral("0"), QStringLiteral("1")}, QStringLiteral("0"), parametersColumn);
    m_antennaCombo = editableCombo(QStringLiteral("UsrpAntennaCombo"), {QStringLiteral("TX/RX"), QStringLiteral("RX2")}, QStringLiteral("TX/RX"), parametersColumn);
    m_centerFreqCombo = editableCombo(QStringLiteral("UsrpCenterFrequencyCombo"), {QStringLiteral("2400000000"), QStringLiteral("1090000000"), QStringLiteral("915000000"), QStringLiteral("433000000")}, QStringLiteral("2400000000"), parametersColumn);
    m_sampleRateCombo = editableCombo(QStringLiteral("UsrpSampleRateCombo"), {QStringLiteral("12800000"), QStringLiteral("8000000"), QStringLiteral("4000000"), QStringLiteral("2000000")}, QStringLiteral("12800000"), parametersColumn);
    m_bandwidthCombo = editableCombo(QStringLiteral("UsrpBandwidthCombo"), {QStringLiteral("10000000"), QStringLiteral("6000000"), QStringLiteral("3000000"), QStringLiteral("1500000")}, QStringLiteral("10000000"), parametersColumn);
    m_gainCombo = editableCombo(QStringLiteral("UsrpGainCombo"), {QStringLiteral("30"), QStringLiteral("20"), QStringLiteral("10"), QStringLiteral("0")}, QStringLiteral("30"), parametersColumn);
    m_durationCombo = editableCombo(QStringLiteral("UsrpDurationCombo"), {QStringLiteral("0.1"), QStringLiteral("0.25"), QStringLiteral("0.5"), QStringLiteral("1.0"), QStringLiteral("3.0")}, QStringLiteral("0.1"), parametersColumn);
    m_cpuFormatCombo = editableCombo(QStringLiteral("UsrpCpuFormatCombo"), {QStringLiteral("sc16"), QStringLiteral("fc32")}, QStringLiteral("sc16"), parametersColumn);
    m_wireFormatCombo = editableCombo(QStringLiteral("UsrpWireFormatCombo"), {QStringLiteral("sc16"), QStringLiteral("sc8")}, QStringLiteral("sc16"), parametersColumn);

    auto *basicGroup = new QGroupBox(QStringLiteral("基础参数"), parametersColumn);
    basicGroup->setObjectName(QStringLiteral("UsrpBasicParametersPanel"));
    auto *basicGrid = new QGridLayout(basicGroup);
    basicGrid->setContentsMargins(14, 18, 14, 14);
    basicGrid->setHorizontalSpacing(14);
    basicGrid->setVerticalSpacing(12);
    basicGrid->addWidget(fieldBlock(QStringLiteral("中心频率 Hz"), m_centerFreqCombo, basicGroup), 0, 0);
    basicGrid->addWidget(fieldBlock(QStringLiteral("采样率 Sps"), m_sampleRateCombo, basicGroup), 0, 1);
    basicGrid->addWidget(fieldBlock(QStringLiteral("带宽 Hz"), m_bandwidthCombo, basicGroup), 1, 0);
    basicGrid->addWidget(fieldBlock(QStringLiteral("增益 dB"), m_gainCombo, basicGroup), 1, 1);
    basicGrid->addWidget(fieldBlock(QStringLiteral("采集时长 s"), m_durationCombo, basicGroup), 2, 0);
    basicGrid->addWidget(fieldBlock(QStringLiteral("天线"), m_antennaCombo, basicGroup), 2, 1);
    basicGrid->setColumnStretch(0, 1);
    basicGrid->setColumnStretch(1, 1);
    parametersLayout->addWidget(basicGroup);

    m_advancedParamsToggle = new QPushButton(QStringLiteral("显示高级参数"), parametersColumn);
    m_advancedParamsToggle->setObjectName(QStringLiteral("UsrpAdvancedParametersToggle"));
    m_advancedParamsToggle->setCheckable(true);
    m_advancedParamsToggle->setProperty("secondary", true);
    parametersLayout->addWidget(m_advancedParamsToggle, 0, Qt::AlignLeft);

    m_advancedParamsPanel = new QWidget(parametersColumn);
    m_advancedParamsPanel->setObjectName(QStringLiteral("UsrpAdvancedParametersPanel"));
    auto *advancedGrid = new QGridLayout(m_advancedParamsPanel);
    advancedGrid->setContentsMargins(0, 0, 0, 0);
    advancedGrid->setHorizontalSpacing(14);
    advancedGrid->setVerticalSpacing(12);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("设备参数"), m_deviceArgsEdit, m_advancedParamsPanel), 0, 0);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("通道"), m_channelCombo, m_advancedParamsPanel), 0, 1);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("CPU 格式"), m_cpuFormatCombo, m_advancedParamsPanel), 1, 0);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("线缆格式"), m_wireFormatCombo, m_advancedParamsPanel), 1, 1);
    advancedGrid->setColumnStretch(0, 1);
    advancedGrid->setColumnStretch(1, 1);
    m_advancedParamsPanel->setVisible(false);
    parametersLayout->addWidget(m_advancedParamsPanel);

    m_validationMessage = new QLabel(QStringLiteral("参数就绪。"), parametersColumn);
    m_validationMessage->setObjectName(QStringLiteral("UsrpCaptureValidationMessage"));
    m_validationMessage->setWordWrap(true);
    m_validationMessage->setProperty("kind", QStringLiteral("neutral"));
    parametersLayout->addWidget(m_validationMessage);
    parametersLayout->addStretch();
    workbench->addWidget(parametersColumn, 4);

    auto *runColumn = new QGroupBox(QStringLiteral("运行"), this);
    runColumn->setObjectName(QStringLiteral("CaptureRunColumn"));
    runColumn->setMinimumWidth(340);
    auto *runLayout = new QVBoxLayout(runColumn);
    runLayout->setContentsMargins(16, 20, 16, 16);
    runLayout->setSpacing(12);

    m_taskMetric = new MetricCard(QStringLiteral("采集任务"), runColumn);
    m_taskMetric->setValue(QStringLiteral("待执行"));
    runLayout->addWidget(m_taskMetric);

    m_taskStateLabel = new QLabel(QStringLiteral("待执行"), runColumn);
    m_taskStateLabel->setObjectName(QStringLiteral("CaptureTaskStateLabel"));
    m_taskStateLabel->setProperty("state", QStringLiteral("idle"));
    runLayout->addWidget(m_taskStateLabel);

    m_fileStateLabel = new QLabel(QStringLiteral("输出目录：%1").arg(QDir::toNativeSeparators(ProjectPaths::dataDirectory())), runColumn);
    m_fileStateLabel->setObjectName(QStringLiteral("CaptureFileStateLabel"));
    m_fileStateLabel->setWordWrap(true);
    m_fileStateLabel->setProperty("muted", true);
    runLayout->addWidget(m_fileStateLabel);

    m_outputDirEdit = new QLineEdit(ProjectPaths::dataDirectory(), runColumn);
    m_outputDirEdit->setObjectName(QStringLiteral("UsrpOutputDirEdit"));
    m_outputDirEdit->setReadOnly(true);
    runLayout->addWidget(m_outputDirEdit);

    m_captureButton = new QPushButton(QStringLiteral("开始采集"), runColumn);
    m_captureButton->setObjectName(QStringLiteral("StartUsrpCaptureButton"));
    m_captureButton->setProperty("primary", true);
    m_captureButton->setMinimumHeight(40);
    m_captureButton->setEnabled(false);
    runLayout->addWidget(m_captureButton);

    m_logOutput = new QPlainTextEdit(runColumn);
    m_logOutput->setObjectName(QStringLiteral("UsrpCaptureLog"));
    m_logOutput->setReadOnly(true);
    m_logOutput->setMinimumHeight(260);
    m_logOutput->setPlainText(QStringLiteral("选择采集源并检查连接。"));
    runLayout->addWidget(m_logOutput, 1);
    workbench->addWidget(runColumn, 3);

    contentLayout()->addLayout(workbench, 4);

    auto *resultsGroup = new QGroupBox(QStringLiteral("结果"), this);
    resultsGroup->setObjectName(QStringLiteral("CaptureResultsPanel"));
    auto *resultsLayout = new QVBoxLayout(resultsGroup);
    resultsLayout->setContentsMargins(16, 20, 16, 16);
    resultsLayout->setSpacing(10);

    m_resultsTabs = new QTabWidget(resultsGroup);
    m_resultsTabs->setObjectName(QStringLiteral("CaptureResultsTabs"));

    m_deviceTable = new QTableWidget(0, 4, m_resultsTabs);
    m_deviceTable->setObjectName(QStringLiteral("UsrpDeviceTable"));
    m_deviceTable->setAlternatingRowColors(true);
    m_deviceTable->setHorizontalHeaderLabels({QStringLiteral("设备"), QStringLiteral("类型"), QStringLiteral("序列号"), QStringLiteral("状态")});
    m_deviceTable->horizontalHeader()->setStretchLastSection(true);
    m_deviceTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_deviceTable, QStringLiteral("设备列表"));

    m_captureTable = new QTableWidget(0, 5, m_resultsTabs);
    m_captureTable->setObjectName(QStringLiteral("UsrpCaptureTable"));
    m_captureTable->setAlternatingRowColors(true);
    m_captureTable->setHorizontalHeaderLabels({QStringLiteral("文件名"), QStringLiteral("设备"), QStringLiteral("中心频率"), QStringLiteral("样本数"), QStringLiteral("状态")});
    m_captureTable->horizontalHeader()->setStretchLastSection(true);
    m_captureTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_captureTable, QStringLiteral("采集记录"));
    resultsLayout->addWidget(m_resultsTabs, 1);
    contentLayout()->addWidget(resultsGroup, 2);

    connect(m_usrpSourceButton, &QPushButton::clicked, this, [this]() {
        m_deviceTypeCombo->setCurrentText(QStringLiteral("USRP B210"));
    });
    connect(m_3943bSourceButton, &QPushButton::clicked, this, [this]() {
        m_deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    });
    connect(m_deviceTypeCombo, &QComboBox::currentTextChanged, this, &CapturePage::updateDeviceTypeUi);
    connect(m_discoverButton, &QPushButton::clicked, this, &CapturePage::discoverDevices);
    connect(m_captureButton, &QPushButton::clicked, this, &CapturePage::startCapture);
    connect(m_advancedParamsToggle, &QPushButton::toggled, this, [this](bool checked) {
        if (m_advancedParamsPanel) {
            m_advancedParamsPanel->setVisible(checked);
        }
        if (m_advancedParamsToggle) {
            m_advancedParamsToggle->setText(checked ? QStringLiteral("隐藏高级参数") : QStringLiteral("显示高级参数"));
        }
    });
}

QWidget *CapturePage::buildUsrpConfigPanel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("RF-A TX/RX"), panel);
    title->setObjectName(QStringLiteral("UsrpConfigSummaryTitle"));
    auto *body = new QLabel(QStringLiteral("默认通道 0，天线 TX/RX；如需指定设备可在高级参数填写 serial。"), panel);
    body->setWordWrap(true);
    body->setProperty("muted", true);
    layout->addWidget(title);
    layout->addWidget(body);
    return panel;
}

QWidget *CapturePage::buildReservedDevicePanel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    m_3943bPlaceholder = new QLabel(QStringLiteral("3943B 接口待接入，当前仅保留设备选择入口。"), panel);
    m_3943bPlaceholder->setObjectName(QStringLiteral("Device3943BPlaceholder"));
    m_3943bPlaceholder->setWordWrap(true);
    layout->addWidget(m_3943bPlaceholder);
    layout->addStretch();
    return panel;
}

void CapturePage::updateDeviceTypeUi()
{
    const bool usrp = isUsrpSelected();
    if (m_usrpSourceButton) {
        m_usrpSourceButton->setChecked(usrp);
        m_usrpSourceButton->setProperty("primary", usrp);
        refreshWidgetStyle(m_usrpSourceButton);
    }
    if (m_3943bSourceButton) {
        m_3943bSourceButton->setChecked(!usrp);
        m_3943bSourceButton->setProperty("primary", !usrp);
        refreshWidgetStyle(m_3943bSourceButton);
    }
    if (m_deviceConfigStack) {
        m_deviceConfigStack->setCurrentIndex(usrp ? kUsrpConfigIndex : kReserved3943BConfigIndex);
    }
    if (!usrp) {
        m_devices.clear();
        if (m_deviceTable) {
            m_deviceTable->setRowCount(0);
        }
        if (m_deviceMetric) {
            m_deviceMetric->setValue(QStringLiteral("3943B 预留"));
        }
        appendValidationMessage(QStringLiteral("3943B 接口待接入，暂不可采集。"), QStringLiteral("neutral"));
        appendLog(QStringLiteral("3943B 设备类型已选择，接口待接入。"));
    } else {
        if (m_deviceMetric && m_devices.isEmpty()) {
            m_deviceMetric->setValue(QStringLiteral("未检查"));
        }
        appendValidationMessage(QStringLiteral("参数就绪。"), QStringLiteral("neutral"));
    }
    updateConnectionSummary();
    setBusy(false);
}

void CapturePage::discoverDevices()
{
    if (!isUsrpSelected()) {
        appendLog(QStringLiteral("3943B 接口待接入，无法检查设备。"));
        return;
    }

    setBusy(true);
    appendLog(QStringLiteral("开始发现 USRP 设备..."));
    QString error;
    m_devices = m_usrpService ? m_usrpService->discover(&error) : QVector<UsrpDeviceInfo>{};
    m_deviceTable->setRowCount(0);

    for (const auto &device : m_devices) {
        const int row = m_deviceTable->rowCount();
        m_deviceTable->insertRow(row);
        m_deviceTable->setItem(row, 0, readonlyItem(device.displayName()));
        m_deviceTable->setItem(row, 1, readonlyItem(device.type));
        m_deviceTable->setItem(row, 2, readonlyItem(device.serial));
        m_deviceTable->setItem(row, 3, readonlyItem(QStringLiteral("可用")));
    }

    if (m_devices.isEmpty()) {
        m_deviceMetric->setValue(QStringLiteral("未发现"));
        appendLog(error.isEmpty() ? QStringLiteral("未发现 USRP 设备。") : error);
    } else {
        m_deviceMetric->setValue(QStringLiteral("%1 台").arg(m_devices.size()));
        appendLog(QStringLiteral("发现 %1 台 USRP：%2").arg(m_devices.size()).arg(m_devices.first().displayName()));
    }
    updateConnectionSummary();
    setBusy(false);
}

void CapturePage::startCapture()
{
    if (!isUsrpSelected()) {
        appendLog(QStringLiteral("3943B 接口待接入，当前无法采集。"));
        return;
    }
    if (m_devices.isEmpty()) {
        appendValidationMessage(QStringLiteral("请先检查 USRP 连接。"), QStringLiteral("error"));
        appendLog(QStringLiteral("请先检查 USRP 连接。"));
        return;
    }

    QString fieldName;
    const QString validationError = validateParameters(&fieldName);
    if (!validationError.isEmpty()) {
        appendValidationMessage(validationError, QStringLiteral("error"));
        appendLog(validationError);
        return;
    }

    UsrpCaptureConfig config;
    config.deviceArgs = m_deviceArgsEdit ? m_deviceArgsEdit->text().trimmed() : QString();
    config.channel = readComboInt(m_channelCombo, config.channel);
    config.antenna = readComboText(m_antennaCombo, config.antenna);
    config.centerFrequencyHz = readComboDouble(m_centerFreqCombo, config.centerFrequencyHz);
    config.sampleRateHz = readComboDouble(m_sampleRateCombo, config.sampleRateHz);
    config.bandwidthHz = readComboDouble(m_bandwidthCombo, config.bandwidthHz);
    config.gainDb = readComboDouble(m_gainCombo, config.gainDb);
    config.durationSeconds = readComboDouble(m_durationCombo, config.durationSeconds);
    config.cpuFormat = readComboText(m_cpuFormatCombo, config.cpuFormat);
    config.wireFormat = readComboText(m_wireFormatCombo, config.wireFormat);
    config.outputFilePath = nextCapturePath();

    appendValidationMessage(QStringLiteral("参数已通过校验。"), QStringLiteral("good"));
    appendLog(QStringLiteral("设备类型：USRP B210"));
    appendLog(QStringLiteral("采集参数：CH%1 %2, %3 MHz, %4 MSps, BW %5 MHz, Gain %6 dB, %7/%8")
                  .arg(config.channel)
                  .arg(config.antenna)
                  .arg(config.centerFrequencyHz / 1000000.0, 0, 'f', 3)
                  .arg(config.sampleRateHz / 1000000.0, 0, 'f', 3)
                  .arg(config.bandwidthHz / 1000000.0, 0, 'f', 3)
                  .arg(config.gainDb, 0, 'f', 1)
                  .arg(config.cpuFormat, config.wireFormat));
    appendLog(QStringLiteral("开始采集：%1 s，输出 %2").arg(config.durationSeconds, 0, 'f', 3).arg(config.outputFilePath));
    startCaptureAsync(config);
}

QString CapturePage::validateParameters(QString *fieldName) const
{
    QString error;
    if (!parseNonNegativeInt(m_channelCombo, QStringLiteral("通道"), &error)) {
        if (fieldName) {
            *fieldName = QStringLiteral("通道");
        }
        return error;
    }
    if (!parsePositiveDouble(m_centerFreqCombo, QStringLiteral("中心频率"), &error)) {
        if (fieldName) {
            *fieldName = QStringLiteral("中心频率");
        }
        return error;
    }
    if (!parsePositiveDouble(m_sampleRateCombo, QStringLiteral("采样率"), &error)) {
        if (fieldName) {
            *fieldName = QStringLiteral("采样率");
        }
        return error;
    }
    if (!parseNonNegativeDouble(m_bandwidthCombo, QStringLiteral("带宽"), &error)) {
        if (fieldName) {
            *fieldName = QStringLiteral("带宽");
        }
        return error;
    }
    if (!parseNonNegativeDouble(m_gainCombo, QStringLiteral("增益"), &error)) {
        if (fieldName) {
            *fieldName = QStringLiteral("增益");
        }
        return error;
    }
    if (!parsePositiveDouble(m_durationCombo, QStringLiteral("采集时长"), &error)) {
        if (fieldName) {
            *fieldName = QStringLiteral("采集时长");
        }
        return error;
    }
    if (readComboText(m_antennaCombo, QString()).isEmpty()) {
        if (fieldName) {
            *fieldName = QStringLiteral("天线");
        }
        return QStringLiteral("天线不能为空。");
    }
    if (readComboText(m_cpuFormatCombo, QString()).isEmpty()) {
        if (fieldName) {
            *fieldName = QStringLiteral("CPU 格式");
        }
        return QStringLiteral("CPU 格式不能为空。");
    }
    if (readComboText(m_wireFormatCombo, QString()).isEmpty()) {
        if (fieldName) {
            *fieldName = QStringLiteral("线缆格式");
        }
        return QStringLiteral("线缆格式不能为空。");
    }
    return {};
}

void CapturePage::startCaptureAsync(const UsrpCaptureConfig &config)
{
    if (!m_usrpService || m_captureThread) {
        return;
    }

    setBusy(true);
    if (m_taskMetric) {
        m_taskMetric->setValue(QStringLiteral("采集中"));
    }
    if (m_taskStateLabel) {
        m_taskStateLabel->setText(QStringLiteral("采集中"));
        m_taskStateLabel->setProperty("state", QStringLiteral("running"));
        refreshWidgetStyle(m_taskStateLabel);
    }
    if (m_fileStateLabel) {
        m_fileStateLabel->setText(QStringLiteral("输出文件：%1").arg(QDir::toNativeSeparators(config.outputFilePath)));
    }

    auto *service = m_usrpService.get();
    m_captureThread = QThread::create([this, service, config]() {
        const UsrpCaptureResult result = service->captureToCap(config);
        QMetaObject::invokeMethod(this, [this, result, config]() {
            m_captureThread = nullptr;
            finalizeCapture(result, config);
        }, Qt::QueuedConnection);
    });
    connect(m_captureThread, &QThread::finished, m_captureThread, &QObject::deleteLater);
    m_captureThread->start();
}

void CapturePage::appendLog(const QString &line)
{
    if (m_logOutput) {
        m_logOutput->appendPlainText(line);
    }
}

void CapturePage::appendValidationMessage(const QString &line, const QString &kind)
{
    if (!m_validationMessage) {
        return;
    }
    m_validationMessage->setText(line);
    m_validationMessage->setProperty("kind", kind);
    refreshWidgetStyle(m_validationMessage);
}

void CapturePage::updateConnectionSummary()
{
    if (!m_connectionSummary) {
        return;
    }
    if (!isUsrpSelected()) {
        m_connectionSummary->setText(QStringLiteral("当前选择 3943B，接口待接入。"));
        return;
    }
    if (m_devices.isEmpty()) {
        m_connectionSummary->setText(QStringLiteral("当前选择 USRP B210。请先检查设备；默认 RF-A TX/RX，通道 0。"));
        return;
    }
    m_connectionSummary->setText(QStringLiteral("已发现 %1 台 USRP。当前默认使用 %2。")
                                     .arg(m_devices.size())
                                     .arg(m_devices.first().displayName()));
}

void CapturePage::setBusy(bool busy)
{
    const bool usrp = isUsrpSelected();
    if (m_discoverButton) {
        m_discoverButton->setEnabled(!busy && usrp);
    }
    if (m_captureButton) {
        m_captureButton->setEnabled(!busy && usrp && !m_devices.isEmpty());
    }
    if (m_deviceTypeCombo) {
        m_deviceTypeCombo->setEnabled(!busy);
    }
    if (m_usrpSourceButton) {
        m_usrpSourceButton->setEnabled(!busy);
    }
    if (m_3943bSourceButton) {
        m_3943bSourceButton->setEnabled(!busy);
    }
}

void CapturePage::finalizeCapture(const UsrpCaptureResult &result, const UsrpCaptureConfig &config)
{
    for (const auto &line : result.logs) {
        appendLog(line);
    }
    appendLog(result.message);

    if (result.success) {
        const int row = m_captureTable->rowCount();
        m_captureTable->insertRow(row);
        m_captureTable->setItem(row, 0, readonlyItem(QFileInfo(result.outputFilePath).fileName()));
        m_captureTable->setItem(row, 1, readonlyItem(QStringLiteral("USRP B210")));
        m_captureTable->setItem(row, 2, readonlyItem(QStringLiteral("%1 MHz").arg(config.centerFrequencyHz / 1000000.0, 0, 'f', 3)));
        m_captureTable->setItem(row, 3, readonlyItem(QString::number(result.sampleCount)));
        m_captureTable->setItem(row, 4, readonlyItem(QStringLiteral("完成")));
        if (m_resultsTabs) {
            m_resultsTabs->setCurrentWidget(m_captureTable);
        }
        m_fileMetric->setValue(QString::number(m_captureTable->rowCount()));
        m_taskMetric->setValue(QStringLiteral("完成"));
        if (m_taskStateLabel) {
            m_taskStateLabel->setText(QStringLiteral("完成"));
            m_taskStateLabel->setProperty("state", QStringLiteral("done"));
            refreshWidgetStyle(m_taskStateLabel);
        }
        appendValidationMessage(QStringLiteral("采集完成。"), QStringLiteral("good"));
    } else {
        m_taskMetric->setValue(QStringLiteral("失败"));
        if (m_taskStateLabel) {
            m_taskStateLabel->setText(QStringLiteral("失败"));
            m_taskStateLabel->setProperty("state", QStringLiteral("error"));
            refreshWidgetStyle(m_taskStateLabel);
        }
        appendValidationMessage(result.message, QStringLiteral("error"));
    }

    setBusy(false);
}

bool CapturePage::isUsrpSelected() const
{
    return !m_deviceTypeCombo || m_deviceTypeCombo->currentText().startsWith(QStringLiteral("USRP"));
}

double CapturePage::readComboDouble(const QComboBox *combo, double fallback) const
{
    bool ok = false;
    const double value = combo ? combo->currentText().trimmed().toDouble(&ok) : fallback;
    return ok ? value : fallback;
}

int CapturePage::readComboInt(const QComboBox *combo, int fallback) const
{
    bool ok = false;
    const int value = combo ? combo->currentText().trimmed().toInt(&ok) : fallback;
    return ok ? value : fallback;
}

QString CapturePage::readComboText(const QComboBox *combo, const QString &fallback) const
{
    const QString value = combo ? combo->currentText().trimmed() : QString();
    return value.isEmpty() ? fallback : value;
}

QString CapturePage::nextCapturePath() const
{
    const QString rawDir = QDir(ProjectPaths::dataDirectory()).filePath(QStringLiteral("raw"));
    QDir().mkpath(rawDir);
    return QDir(rawDir).filePath(QStringLiteral("usrp_%1.cap").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
}
