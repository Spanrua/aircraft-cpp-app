#include "ui/pages/CapturePage.h"

#include "app/ProjectPaths.h"
#include "services/CeyearVisaQuery.h"
#include "ui/widgets/MetricCard.h"

#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>
#include <cmath>
#include <exception>
#include <algorithm>
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

QString comboTextToken(const QComboBox *combo, const QString &fallback)
{
    const QString text = combo ? combo->currentText().trimmed() : QString();
    return text.isEmpty() ? fallback : text.section(QLatin1Char(' '), 0, 0).trimmed();
}

QString cleanCaptureToken(QString token, const QString &fallback)
{
    token = token.trimmed();
    QString cleaned;
    cleaned.reserve(token.size());
    for (const QChar ch : token) {
        if (ch.isLetterOrNumber()) {
            cleaned.append(ch);
        }
    }
    return cleaned.isEmpty() ? fallback : cleaned;
}

QString twoDigitCaptureToken(const QComboBox *combo, const QString &fallback)
{
    QString token = cleanCaptureToken(comboTextToken(combo, fallback), fallback);
    bool ok = false;
    const int value = token.toInt(&ok);
    if (ok && value >= 0 && value <= 99) {
        return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
    }
    return token.right(2).rightJustified(2, QLatin1Char('0'));
}

QString prefixedCaptureToken(const QComboBox *combo, QLatin1Char prefix, const QString &fallback)
{
    QString token = cleanCaptureToken(comboTextToken(combo, fallback), fallback).toUpper();
    if (!token.startsWith(prefix)) {
        token.prepend(prefix);
    }
    bool ok = false;
    const int value = token.mid(1).toInt(&ok);
    if (ok && value >= 0 && value <= 99) {
        return QString(prefix) + QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
    }
    return token;
}

QString segmentCaptureToken(const QLineEdit *edit)
{
    QString token = edit ? edit->text().trimmed().toUpper() : QString();
    if (token.startsWith(QLatin1Char('S'))) {
        token.remove(0, 1);
    }

    bool ok = false;
    int value = token.toInt(&ok);
    value = std::max(1, std::min(999, ok ? value : 1));
    return QStringLiteral("S%1").arg(value, 3, 10, QLatin1Char('0'));
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

template <typename CreateEditor>
QWidget *fieldBlockWithEditor(const QString &label, QWidget *parent, CreateEditor createEditor)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *caption = new QLabel(label, container);
    caption->setProperty("muted", true);
    layout->addWidget(caption);
    layout->addWidget(createEditor(container));
    return container;
}

QVBoxLayout *scrollColumnContent(QGroupBox *column, const QString &objectName)
{
    auto *outerLayout = new QVBoxLayout(column);
    outerLayout->setContentsMargins(12, 18, 12, 12);
    outerLayout->setSpacing(0);

    auto *scrollArea = new QScrollArea(column);
    scrollArea->setObjectName(objectName);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget(scrollArea);
    content->setObjectName(objectName + QStringLiteral("Content"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);
    scrollArea->setWidget(content);

    outerLayout->addWidget(scrollArea, 1);
    return contentLayout;
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

void captureDiagLog(const QString &line)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    const QString dirPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs"));
    QDir().mkpath(dirPath);
    QFile file(QDir(dirPath).filePath(QStringLiteral("aircraft_qt_debug.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString threadId = QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16);
    file.write(QStringLiteral("[%1][thread %2][CapturePage] %3\n").arg(stamp, threadId, line).toUtf8());
    file.flush();
}

} // namespace

CapturePage::CapturePage(QWidget *parent)
    : CapturePage(std::make_unique<UsrpDeviceService>(), parent)
{
}

CapturePage::CapturePage(std::unique_ptr<IUsrpDeviceService> usrpService, QWidget *parent)
    : WorkflowPage(QStringLiteral("数据采集"), QStringLiteral("选择采集设备，配置射频参数，并写入 IQ 原始数据文件。"), parent, true)
    , m_usrpService(std::move(usrpService))
    , m_ceyearService(std::make_unique<CeyearVisaService>())
{
    buildUi();
    updateDeviceTypeUi();
}

CapturePage::CapturePage(std::unique_ptr<IUsrpDeviceService> usrpService,
                         std::unique_ptr<ICeyearVisaService> ceyearService,
                         QWidget *parent)
    : WorkflowPage(QStringLiteral("数据采集"), QStringLiteral("选择采集设备，配置射频参数，并写入 IQ 原始数据文件。"), parent, true)
    , m_usrpService(std::move(usrpService))
    , m_ceyearService(std::move(ceyearService))
{
    if (!m_usrpService) {
        m_usrpService = std::make_unique<UsrpDeviceService>();
    }
    if (!m_ceyearService) {
        m_ceyearService = std::make_unique<CeyearVisaService>();
    }
    buildUi();
    updateDeviceTypeUi();
}

CapturePage::~CapturePage()
{
    if (m_ceyearContinuousTimer) {
        m_ceyearContinuousTimer->stop();
    }
    if (m_captureThread) {
        m_captureThread->quit();
        m_captureThread->wait();
    }
    if (m_ceyearThread) {
        m_ceyearThread->quit();
        m_ceyearThread->wait();
    }
}

void CapturePage::buildUi()
{
    if (auto *scrollContent = findChild<QWidget *>(QStringLiteral("WorkflowPageScrollContent"))) {
        scrollContent->setMinimumHeight(620);
    }

    auto *workbench = new QHBoxLayout();
    workbench->setSpacing(14);

    auto *deviceColumn = new QGroupBox(QStringLiteral("设备"), this);
    deviceColumn->setObjectName(QStringLiteral("CaptureDeviceColumn"));
    deviceColumn->setMinimumWidth(240);
    auto *deviceLayout = scrollColumnContent(deviceColumn, QStringLiteral("CaptureDeviceColumnScroll"));

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
    m_3943bSourceButton = new QPushButton(QStringLiteral("3943B"), deviceColumn);
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
    deviceLayout->addWidget(m_deviceConfigStack);
    deviceLayout->addStretch();
    workbench->addWidget(deviceColumn, 23);

    auto *parametersColumn = new QGroupBox(QStringLiteral("参数配置"), this);
    parametersColumn->setObjectName(QStringLiteral("CaptureParametersColumn"));
    parametersColumn->setMinimumWidth(360);
    auto *parametersLayout = scrollColumnContent(parametersColumn, QStringLiteral("CaptureParametersColumnScroll"));

    m_parameterStack = new QStackedWidget(parametersColumn);
    m_parameterStack->setObjectName(QStringLiteral("CaptureParameterStack"));
    m_usrpParameterPanel = new QWidget(m_parameterStack);
    m_usrpParameterPanel->setObjectName(QStringLiteral("UsrpParameterPanel"));
    auto *usrpParameterLayout = new QVBoxLayout(m_usrpParameterPanel);
    usrpParameterLayout->setContentsMargins(0, 0, 0, 0);
    usrpParameterLayout->setSpacing(12);

    m_editableParamsHint = new QLabel(QStringLiteral("下拉项是常用预设，也可以直接输入 UHD 支持的数值。你的天线接在 RF-A TX/RX，默认使用通道 0 + TX/RX。"), m_usrpParameterPanel);
    m_editableParamsHint->setObjectName(QStringLiteral("UsrpEditableParametersHint"));
    m_editableParamsHint->setWordWrap(true);
    m_editableParamsHint->setProperty("muted", true);
    usrpParameterLayout->addWidget(m_editableParamsHint);

    auto *basicGroup = new QGroupBox(QStringLiteral("基础参数"), m_usrpParameterPanel);
    basicGroup->setObjectName(QStringLiteral("UsrpBasicParametersPanel"));
    auto *basicGrid = new QGridLayout(basicGroup);
    basicGrid->setContentsMargins(14, 18, 14, 14);
    basicGrid->setHorizontalSpacing(14);
    basicGrid->setVerticalSpacing(12);
    basicGrid->addWidget(fieldBlockWithEditor(QStringLiteral("中心频率 Hz"), basicGroup, [this](QWidget *parent) {
        m_centerFreqCombo = editableCombo(QStringLiteral("UsrpCenterFrequencyCombo"), {QStringLiteral("2400000000"), QStringLiteral("1090000000"), QStringLiteral("915000000"), QStringLiteral("433000000")}, QStringLiteral("2400000000"), parent);
        return m_centerFreqCombo;
    }), 0, 0);
    basicGrid->addWidget(fieldBlockWithEditor(QStringLiteral("采样率 Sps"), basicGroup, [this](QWidget *parent) {
        m_sampleRateCombo = editableCombo(QStringLiteral("UsrpSampleRateCombo"), {QStringLiteral("12800000"), QStringLiteral("8000000"), QStringLiteral("4000000"), QStringLiteral("2000000")}, QStringLiteral("12800000"), parent);
        return m_sampleRateCombo;
    }), 0, 1);
    basicGrid->addWidget(fieldBlockWithEditor(QStringLiteral("增益 dB"), basicGroup, [this](QWidget *parent) {
        m_gainCombo = editableCombo(QStringLiteral("UsrpGainCombo"), {QStringLiteral("30"), QStringLiteral("20"), QStringLiteral("10"), QStringLiteral("0")}, QStringLiteral("30"), parent);
        return m_gainCombo;
    }), 1, 0);
    basicGrid->addWidget(fieldBlockWithEditor(QStringLiteral("采集时长 s"), basicGroup, [this](QWidget *parent) {
        m_durationCombo = editableCombo(QStringLiteral("UsrpDurationCombo"), {QStringLiteral("0.1"), QStringLiteral("0.25"), QStringLiteral("0.5"), QStringLiteral("1.0"), QStringLiteral("3.0")}, QStringLiteral("0.1"), parent);
        return m_durationCombo;
    }), 1, 1);
    basicGrid->addWidget(fieldBlockWithEditor(QStringLiteral("天线"), basicGroup, [this](QWidget *parent) {
        m_antennaCombo = editableCombo(QStringLiteral("UsrpAntennaCombo"), {QStringLiteral("TX/RX"), QStringLiteral("RX2")}, QStringLiteral("TX/RX"), parent);
        return m_antennaCombo;
    }), 2, 0);
    basicGrid->setColumnStretch(0, 1);
    basicGrid->setColumnStretch(1, 1);
    usrpParameterLayout->addWidget(basicGroup);

    m_advancedParamsToggle = new QPushButton(QStringLiteral("显示高级参数"), m_usrpParameterPanel);
    m_advancedParamsToggle->setObjectName(QStringLiteral("UsrpAdvancedParametersToggle"));
    m_advancedParamsToggle->setCheckable(true);
    m_advancedParamsToggle->setProperty("secondary", true);
    usrpParameterLayout->addWidget(m_advancedParamsToggle, 0, Qt::AlignLeft);

    m_advancedParamsPanel = new QWidget(m_usrpParameterPanel);
    m_advancedParamsPanel->setObjectName(QStringLiteral("UsrpAdvancedParametersPanel"));
    auto *advancedGrid = new QGridLayout(m_advancedParamsPanel);
    advancedGrid->setContentsMargins(0, 0, 0, 0);
    advancedGrid->setHorizontalSpacing(14);
    advancedGrid->setVerticalSpacing(12);
    advancedGrid->addWidget(fieldBlockWithEditor(QStringLiteral("设备参数"), m_advancedParamsPanel, [this](QWidget *parent) {
        auto *edit = new QLineEdit(parent);
        edit->setObjectName(QStringLiteral("UsrpDeviceArgsEdit"));
        edit->setPlaceholderText(QStringLiteral("留空为自动发现，例如 serial=30FB59C"));
        edit->setMinimumHeight(36);
        m_deviceArgsEdit = edit;
        return edit;
    }), 0, 0);
    advancedGrid->addWidget(fieldBlockWithEditor(QStringLiteral("通道"), m_advancedParamsPanel, [this](QWidget *parent) {
        m_channelCombo = editableCombo(QStringLiteral("UsrpChannelCombo"), {QStringLiteral("0"), QStringLiteral("1")}, QStringLiteral("0"), parent);
        return m_channelCombo;
    }), 0, 1);
    advancedGrid->addWidget(fieldBlockWithEditor(QStringLiteral("CPU 格式"), m_advancedParamsPanel, [this](QWidget *parent) {
        m_cpuFormatCombo = editableCombo(QStringLiteral("UsrpCpuFormatCombo"), {QStringLiteral("sc16"), QStringLiteral("fc32")}, QStringLiteral("sc16"), parent);
        return m_cpuFormatCombo;
    }), 1, 0);
    advancedGrid->addWidget(fieldBlockWithEditor(QStringLiteral("线缆格式"), m_advancedParamsPanel, [this](QWidget *parent) {
        m_wireFormatCombo = editableCombo(QStringLiteral("UsrpWireFormatCombo"), {QStringLiteral("sc16"), QStringLiteral("sc8")}, QStringLiteral("sc16"), parent);
        return m_wireFormatCombo;
    }), 1, 1);
    advancedGrid->setColumnStretch(0, 1);
    advancedGrid->setColumnStretch(1, 1);
    m_advancedParamsPanel->setVisible(false);
    usrpParameterLayout->addWidget(m_advancedParamsPanel);

    usrpParameterLayout->addStretch();
    m_parameterStack->addWidget(m_usrpParameterPanel);
    m_ceyearControlPanel = buildCeyearControlPanel();
    m_parameterStack->addWidget(m_ceyearControlPanel);
    parametersLayout->addWidget(m_parameterStack, 1);

    m_validationMessage = new QLabel(QStringLiteral("参数就绪。"), parametersColumn);
    m_validationMessage->setObjectName(QStringLiteral("UsrpCaptureValidationMessage"));
    m_validationMessage->setWordWrap(true);
    m_validationMessage->setProperty("kind", QStringLiteral("neutral"));
    parametersLayout->addWidget(m_validationMessage);
    workbench->addWidget(parametersColumn, 43);

    auto *runColumn = new QGroupBox(QStringLiteral("运行"), this);
    runColumn->setObjectName(QStringLiteral("CaptureRunColumn"));
    runColumn->setMinimumWidth(300);
    auto *runLayout = scrollColumnContent(runColumn, QStringLiteral("CaptureRunColumnScroll"));

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

    m_stopCeyearCaptureButton = new QPushButton(QStringLiteral("停止 3943B 采集"), runColumn);
    m_stopCeyearCaptureButton->setObjectName(QStringLiteral("StopCeyearCaptureButton"));
    m_stopCeyearCaptureButton->setMinimumHeight(38);
    m_stopCeyearCaptureButton->setProperty("secondary", true);
    runLayout->addWidget(m_stopCeyearCaptureButton);

    m_logOutput = new QPlainTextEdit(runColumn);
    m_logOutput->setObjectName(QStringLiteral("UsrpCaptureLog"));
    m_logOutput->setReadOnly(true);
    m_logOutput->setMinimumHeight(180);
    m_logOutput->setPlainText(QStringLiteral("选择采集源并检查连接。"));
    runLayout->addWidget(m_logOutput, 1);
    workbench->addWidget(runColumn, 34);

    contentLayout()->addLayout(workbench, 4);

    auto *resultsGroup = new QGroupBox(QStringLiteral("结果"), this);
    resultsGroup->setObjectName(QStringLiteral("CaptureResultsPanel"));
    resultsGroup->setMinimumHeight(280);
    auto *resultsLayout = new QVBoxLayout(resultsGroup);
    resultsLayout->setContentsMargins(16, 20, 16, 16);
    resultsLayout->setSpacing(10);

    m_resultsTabs = new QTabWidget(resultsGroup);
    m_resultsTabs->setObjectName(QStringLiteral("CaptureResultsTabs"));
    m_resultsTabs->setMinimumHeight(220);

    m_deviceTable = new QTableWidget(0, 4, m_resultsTabs);
    m_deviceTable->setObjectName(QStringLiteral("UsrpDeviceTable"));
    m_deviceTable->setMinimumHeight(180);
    m_deviceTable->setAlternatingRowColors(true);
    m_deviceTable->setHorizontalHeaderLabels({QStringLiteral("设备"), QStringLiteral("类型"), QStringLiteral("序列号"), QStringLiteral("状态")});
    m_deviceTable->horizontalHeader()->setStretchLastSection(true);
    m_deviceTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_deviceTable, QStringLiteral("设备列表"));

    m_captureTable = new QTableWidget(0, 5, m_resultsTabs);
    m_captureTable->setObjectName(QStringLiteral("UsrpCaptureTable"));
    m_captureTable->setMinimumHeight(180);
    m_captureTable->setAlternatingRowColors(true);
    m_captureTable->setHorizontalHeaderLabels({QStringLiteral("文件名"), QStringLiteral("设备"), QStringLiteral("中心频率"), QStringLiteral("样本数"), QStringLiteral("状态")});
    m_captureTable->horizontalHeader()->setStretchLastSection(true);
    m_captureTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_captureTable, QStringLiteral("采集记录"));

    m_ceyearStatusTable = new QTableWidget(0, 4, m_resultsTabs);
    m_ceyearStatusTable->setObjectName(QStringLiteral("CeyearStatusTable"));
    m_ceyearStatusTable->setMinimumHeight(180);
    m_ceyearStatusTable->setAlternatingRowColors(true);
    m_ceyearStatusTable->setHorizontalHeaderLabels({QStringLiteral("项目"), QStringLiteral("SCPI"), QStringLiteral("响应"), QStringLiteral("状态")});
    m_ceyearStatusTable->horizontalHeader()->setStretchLastSection(true);
    m_ceyearStatusTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_ceyearStatusTable, QStringLiteral("3943B 状态"));
    resultsLayout->addWidget(m_resultsTabs, 1);
    contentLayout()->addWidget(resultsGroup, 2);

    m_ceyearContinuousTimer = new QTimer(this);
    m_ceyearContinuousTimer->setSingleShot(true);
    connect(m_ceyearContinuousTimer, &QTimer::timeout, this, [this]() {
        if (!m_ceyearContinuousActive || isUsrpSelected() || m_ceyearBusy || m_ceyearThread) {
            if (m_ceyearContinuousActive && !isUsrpSelected()) {
                m_ceyearContinuousTimer->start(1);
            }
            return;
        }
        startCeyearCaptureFromUi(true);
    });

    connect(m_usrpSourceButton, &QPushButton::clicked, this, [this]() {
        m_deviceTypeCombo->setCurrentText(QStringLiteral("USRP B210"));
    });
    connect(m_3943bSourceButton, &QPushButton::clicked, this, [this]() {
        m_deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    });
    connect(m_deviceTypeCombo, &QComboBox::currentTextChanged, this, &CapturePage::updateDeviceTypeUi);
    connect(m_discoverButton, &QPushButton::clicked, this, &CapturePage::discoverDevices);
    connect(m_captureButton, &QPushButton::clicked, this, &CapturePage::startCapture);
    connect(m_stopCeyearCaptureButton, &QPushButton::clicked, this, &CapturePage::stopCeyearCaptureAsync);
    connect(m_ceyearResourceEdit, &QLineEdit::editingFinished, this, &CapturePage::updateConnectionSummary);
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
    auto *panel = new QWidget();
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
    auto *panel = new QWidget();
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    m_3943bPlaceholder = new QLabel(QStringLiteral("3943B 接口待接入，当前仅保留设备选择入口。"), panel);
    m_3943bPlaceholder->setObjectName(QStringLiteral("Device3943BPlaceholder"));
    m_3943bPlaceholder->setWordWrap(true);
    m_3943bPlaceholder->setText(QStringLiteral("3943B LAN/VISA 状态查询。确认电脑网口与仪器同网段后，点击左侧检查设备。"));
    m_3943bPlaceholder->setProperty("muted", true);
    layout->addWidget(m_3943bPlaceholder);

    layout->addWidget(fieldBlockWithEditor(QStringLiteral("VISA Resource"), panel, [this](QWidget *parent) {
        auto *edit = new QLineEdit(defaultCeyearVisaResource(), parent);
        edit->setObjectName(QStringLiteral("CeyearVisaResourceEdit"));
        edit->setMinimumHeight(36);
        edit->setPlaceholderText(defaultCeyearVisaResource());
        m_ceyearResourceEdit = edit;
        return edit;
    }));
    layout->addStretch();
    return panel;
}

QWidget *CapturePage::buildCeyearControlPanel()
{
    auto *panel = new QWidget();
    panel->setObjectName(QStringLiteral("CeyearControlPanel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *hint = new QLabel(QStringLiteral("采集时会自动写入当前 3943B 频点、跨度、解调和检波参数，无需提前单独写入。"), panel);
    hint->setObjectName(QStringLiteral("CeyearControlHint"));
    hint->setWordWrap(true);
    hint->setProperty("muted", true);
    layout->addWidget(hint);

    auto *controlGroup = new QGroupBox(QStringLiteral("3943B 可写控制"), panel);
    controlGroup->setObjectName(QStringLiteral("CeyearControlGroup"));
    auto *grid = new QGridLayout(controlGroup);
    grid->setContentsMargins(14, 18, 14, 14);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(12);

    grid->addWidget(fieldBlockWithEditor(QStringLiteral("中心频率 Hz"), controlGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(QStringLiteral("2400000000"), parent);
        edit->setObjectName(QStringLiteral("CeyearCenterFrequencyEdit"));
        edit->setMinimumHeight(36);
        m_ceyearCenterFrequencyEdit = edit;
        return edit;
    }), 0, 0);
    grid->addWidget(fieldBlockWithEditor(QStringLiteral("频率跨度 Hz"), controlGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(QStringLiteral("20000000"), parent);
        edit->setObjectName(QStringLiteral("CeyearFrequencySpanEdit"));
        edit->setMinimumHeight(36);
        m_ceyearFrequencySpanEdit = edit;
        return edit;
    }), 0, 1);
    grid->addWidget(fieldBlockWithEditor(QStringLiteral("解调"), controlGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearDemodulationCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("IQ"), QStringLiteral("FM"), QStringLiteral("AM"), QStringLiteral("PM"), QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("CW")});
        combo->setCurrentText(QStringLiteral("IQ"));
        combo->setMinimumHeight(36);
        m_ceyearDemodulationCombo = combo;
        return combo;
    }), 1, 0);
    grid->addWidget(fieldBlockWithEditor(QStringLiteral("检波"), controlGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearDetectorCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("PEAK"), QStringLiteral("AVG"), QStringLiteral("RMS"), QStringLiteral("FAST")});
        combo->setCurrentText(QStringLiteral("PEAK"));
        combo->setMinimumHeight(36);
        m_ceyearDetectorCombo = combo;
        return combo;
    }), 1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    layout->addWidget(controlGroup);

    auto *captureGroup = new QGroupBox(QStringLiteral("3943B 采集配置"), panel);
    captureGroup->setObjectName(QStringLiteral("CeyearCaptureGroup"));
    auto *captureGrid = new QGridLayout(captureGroup);
    captureGrid->setContentsMargins(14, 18, 14, 14);
    captureGrid->setHorizontalSpacing(14);
    captureGrid->setVerticalSpacing(12);

    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("IQ 点数"), captureGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(QStringLiteral("4096"), parent);
        edit->setObjectName(QStringLiteral("CeyearIqPointCountEdit"));
        edit->setMinimumHeight(36);
        m_ceyearIqPointCountEdit = edit;
        return edit;
    }), 0, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("测量模式"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearMeasureModeCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("FIX"), QStringLiteral("PER"), QStringLiteral("PSCAN"), QStringLiteral("FSCAN")});
        combo->setCurrentText(QStringLiteral("FIX"));
        combo->setMinimumHeight(36);
        m_ceyearMeasureModeCombo = combo;
        return combo;
    }), 0, 1);

    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("目标机型"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearTargetModelCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("Yu4Pro"),
                         QStringLiteral("Mini5Pro"),
                         QStringLiteral("Air3S"),
                         QStringLiteral("Air3"),
                         QStringLiteral("Mini4Pro"),
                         QStringLiteral("Air2S"),
                         QStringLiteral("Mini3Pro"),
                         QStringLiteral("Air2")});
        combo->setCurrentText(QStringLiteral("Yu4Pro"));
        combo->setMinimumHeight(36);
        m_ceyearTargetModelCombo = combo;
        return combo;
    }), 1, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("机型编号"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearModelIndexCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("01"), QStringLiteral("02")});
        combo->setCurrentText(QStringLiteral("01"));
        combo->setMinimumHeight(36);
        m_ceyearModelIndexCombo = combo;
        return combo;
    }), 1, 1);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("距离"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearDistanceCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("D00"),
                         QStringLiteral("D01"),
                         QStringLiteral("D02"),
                         QStringLiteral("D03")});
        combo->setCurrentText(QStringLiteral("D02"));
        combo->setMinimumHeight(36);
        m_ceyearDistanceCombo = combo;
        return combo;
    }), 2, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("高度"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearHeightCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("H00"),
                         QStringLiteral("H01"),
                         QStringLiteral("H02"),
                         QStringLiteral("H03")});
        combo->setCurrentText(QStringLiteral("H02"));
        combo->setMinimumHeight(36);
        m_ceyearHeightCombo = combo;
        return combo;
    }), 2, 1);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("频段"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearBandCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("F24"), QStringLiteral("F58")});
        combo->setCurrentText(QStringLiteral("F24"));
        combo->setMinimumHeight(36);
        m_ceyearBandCombo = combo;
        return combo;
    }), 3, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("片段序号"), captureGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(QStringLiteral("001"), parent);
        edit->setObjectName(QStringLiteral("CeyearSegmentEdit"));
        edit->setMinimumHeight(36);
        m_ceyearSegmentEdit = edit;
        return edit;
    }), 3, 1);

    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("候选数据源"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearRecordSourceCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("IQAudio"), QStringLiteral("IQ"), QStringLiteral("IFP"), QStringLiteral("AF")});
        combo->setCurrentText(QStringLiteral("IQAudio"));
        combo->setMinimumHeight(36);
        m_ceyearRecordSourceCombo = combo;
        return combo;
    }), 4, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("上报路径"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearReportPathCombo"));
        combo->setEditable(true);
        combo->addItems({QStringLiteral("UDP"), QStringLiteral("TCP")});
        combo->setCurrentText(QStringLiteral("UDP"));
        combo->setMinimumHeight(36);
        m_ceyearReportPathCombo = combo;
        return combo;
    }), 4, 1);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("PC 接收协议"), captureGroup, [this](QWidget *parent) {
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("CeyearPcReceiveProtocolCombo"));
        combo->addItems({QStringLiteral("UDP"), QStringLiteral("TCP")});
        combo->setCurrentText(QStringLiteral("UDP"));
        combo->setMinimumHeight(36);
        m_ceyearPcReceiveProtocolCombo = combo;
        return combo;
    }), 5, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("PC 本机 IP"), captureGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(QStringLiteral("192.168.50.20"), parent);
        edit->setObjectName(QStringLiteral("CeyearPcReceiveIpEdit"));
        edit->setMinimumHeight(36);
        m_ceyearPcReceiveIpEdit = edit;
        return edit;
    }), 5, 1);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("PC 接收端口"), captureGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(QStringLiteral("5030"), parent);
        edit->setObjectName(QStringLiteral("CeyearPcReceivePortEdit"));
        edit->setMinimumHeight(36);
        m_ceyearPcReceivePortEdit = edit;
        return edit;
    }), 6, 0);
    captureGrid->addWidget(fieldBlockWithEditor(QStringLiteral("PC 保存文件"), captureGroup, [this](QWidget *parent) {
        auto *edit = new QLineEdit(parent);
        edit->setObjectName(QStringLiteral("CeyearPcOutputFileEdit"));
        edit->setReadOnly(true);
        edit->setMinimumHeight(36);
        m_ceyearPcOutputFileEdit = edit;
        return edit;
    }), 7, 0, 1, 2);
    m_ceyearRecordEnableCheck = new QCheckBox(QStringLiteral("使用安全触发 IQ 上报并保存 IQRAW"), captureGroup);
    m_ceyearRecordEnableCheck->setObjectName(QStringLiteral("CeyearRecordEnableCheck"));
    m_ceyearRecordEnableCheck->setChecked(true);
    captureGrid->addWidget(m_ceyearRecordEnableCheck, 8, 0, 1, 2);

    m_ceyearContinuousCaptureCheck = new QCheckBox(QStringLiteral("连续采集：每轮按 IQ 点数结束，完成后立即开始下一轮"), captureGroup);
    m_ceyearContinuousCaptureCheck->setObjectName(QStringLiteral("CeyearContinuousCaptureCheck"));
    captureGrid->addWidget(m_ceyearContinuousCaptureCheck, 9, 0, 1, 2);
    captureGrid->setColumnStretch(0, 1);
    captureGrid->setColumnStretch(1, 1);
    layout->addWidget(captureGroup);

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
    if (m_parameterStack) {
        m_parameterStack->setCurrentWidget(usrp ? m_usrpParameterPanel : m_ceyearControlPanel);
    }
    if (m_taskMetric) {
        m_taskMetric->setValue(usrp ? QStringLiteral("待执行") : QStringLiteral("3943B 待采集"));
    }
    if (m_taskStateLabel) {
        m_taskStateLabel->setText(usrp ? QStringLiteral("待执行") : QStringLiteral("3943B 待采集"));
        m_taskStateLabel->setProperty("state", QStringLiteral("idle"));
        refreshWidgetStyle(m_taskStateLabel);
    }
    if (!usrp) {
        m_devices.clear();
        if (m_deviceTable) {
            m_deviceTable->setRowCount(0);
        }
        if (m_deviceMetric) {
            m_deviceMetric->setValue(QStringLiteral("3943B 待查询"));
        }
        appendValidationMessage(QStringLiteral("3943B 支持 LAN/VISA 采集控制；未连接实机时可先做离线界面和命令测试。"), QStringLiteral("neutral"));
        appendLog(QStringLiteral("3943B 设备类型已选择，可通过 LAN/VISA 查询状态或启动采集控制。"));
    } else {
        if (m_deviceMetric && m_devices.isEmpty()) {
            m_deviceMetric->setValue(QStringLiteral("未检查"));
        }
        appendValidationMessage(QStringLiteral("参数就绪。"), QStringLiteral("neutral"));
    }
    if (m_discoverButton) {
        m_discoverButton->setText(usrp ? QStringLiteral("检查设备") : QStringLiteral("查询状态"));
    }
    if (m_captureButton) {
        m_captureButton->setText(usrp ? QStringLiteral("开始采集") : QStringLiteral("开始 3943B IQ 采集"));
    }
    updateConnectionSummary();
    setBusy(false);
}

void CapturePage::discoverDevices()
{
    if (!isUsrpSelected()) {
        queryCeyearStatusAsync();
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
    captureDiagLog(QStringLiteral("startCapture entered; isUsrp=%1").arg(isUsrpSelected() ? QStringLiteral("true") : QStringLiteral("false")));
    if (!isUsrpSelected()) {
        if (m_ceyearBusy || m_ceyearThread) {
            captureDiagLog(QStringLiteral("3943B start ignored; operation already busy"));
            return;
        }
        const bool continuous = m_ceyearContinuousCaptureCheck && m_ceyearContinuousCaptureCheck->isChecked();
        if (continuous) {
            m_ceyearContinuousActive = true;
            m_ceyearContinuousRound = 0;
            appendLog(QStringLiteral("3943B 连续采集已启动：每轮按 IQ 点数达到后保存 IQRAW。"));
            appendLog(QStringLiteral("3943B 连续采集流程：本轮结束后立即开始下一轮，不再按固定间隔等待。"));
        }
        if (!startCeyearCaptureFromUi(continuous)) {
            if (continuous) {
                cancelCeyearContinuousCapture();
            }
        }
        captureDiagLog(QStringLiteral("3943B startCeyearCaptureAsync returned"));
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
    config.bandwidthHz = config.sampleRateHz;
    config.gainDb = readComboDouble(m_gainCombo, config.gainDb);
    config.durationSeconds = readComboDouble(m_durationCombo, config.durationSeconds);
    config.cpuFormat = readComboText(m_cpuFormatCombo, config.cpuFormat);
    config.wireFormat = readComboText(m_wireFormatCombo, config.wireFormat);
    config.outputFilePath = nextCapturePath();

    appendValidationMessage(QStringLiteral("参数已通过校验。"), QStringLiteral("good"));
    appendLog(QStringLiteral("设备类型：USRP B210"));
    appendLog(QStringLiteral("采集参数：CH%1 %2, %3 MHz, %4 MSps, Gain %5 dB, %6/%7")
                  .arg(config.channel)
                  .arg(config.antenna)
                  .arg(config.centerFrequencyHz / 1000000.0, 0, 'f', 3)
                  .arg(config.sampleRateHz / 1000000.0, 0, 'f', 3)
                  .arg(config.gainDb, 0, 'f', 1)
                  .arg(config.cpuFormat, config.wireFormat));
    appendLog(QStringLiteral("开始采集：%1 s，输出 %2").arg(config.durationSeconds, 0, 'f', 3).arg(config.outputFilePath));
    startCaptureAsync(config);
}

void CapturePage::setPresetCenterFrequencyHz(double frequencyHz)
{
    if (m_ceyearCenterFrequencyEdit) {
        m_ceyearCenterFrequencyEdit->setText(QString::number(frequencyHz, 'f', 0));
    }
    if (m_deviceTypeCombo) {
        m_deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    }
    updateDeviceTypeUi();
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

QString CapturePage::validateCeyearControl(CeyearControlConfig *config) const
{
    bool ok = false;
    const double centerFrequency = m_ceyearCenterFrequencyEdit ? m_ceyearCenterFrequencyEdit->text().trimmed().toDouble(&ok) : 0.0;
    if (!ok || centerFrequency <= 0.0 || !std::isfinite(centerFrequency)) {
        return QStringLiteral("3943B 中心频率必须是大于 0 的数字。");
    }

    ok = false;
    const double span = m_ceyearFrequencySpanEdit ? m_ceyearFrequencySpanEdit->text().trimmed().toDouble(&ok) : 0.0;
    if (!ok || span <= 0.0 || !std::isfinite(span)) {
        return QStringLiteral("3943B 频率跨度必须是大于 0 的数字。");
    }

    const QString demodulation = m_ceyearDemodulationCombo ? m_ceyearDemodulationCombo->currentText().trimmed().toUpper() : QString();
    if (demodulation.isEmpty()) {
        return QStringLiteral("3943B 解调方式不能为空。");
    }

    const QString detector = m_ceyearDetectorCombo ? m_ceyearDetectorCombo->currentText().trimmed().toUpper() : QString();
    if (detector.isEmpty()) {
        return QStringLiteral("3943B 检波方式不能为空。");
    }

    if (config) {
        config->centerFrequencyHz = centerFrequency;
        config->frequencySpanHz = span;
        config->demodulation = demodulation;
        config->detector = detector;
    }
    return {};
}

QString CapturePage::validateCeyearCapture(CeyearCaptureConfig *config) const
{
    CeyearControlConfig control;
    const QString controlError = validateCeyearControl(&control);
    if (!controlError.isEmpty()) {
        return controlError;
    }

    const QString measureMode = m_ceyearMeasureModeCombo ? m_ceyearMeasureModeCombo->currentText().trimmed().toUpper() : QString();
    if (measureMode.isEmpty()) {
        return QStringLiteral("3943B 测量模式不能为空。");
    }

    bool ok = false;
    const int iqPointCount = m_ceyearIqPointCountEdit ? m_ceyearIqPointCountEdit->text().trimmed().toInt(&ok) : 0;
    if (!ok || iqPointCount <= 0) {
        return QStringLiteral("3943B IQ 点数必须是大于 0 的整数。");
    }

    const QString recordSource = m_ceyearRecordSourceCombo ? m_ceyearRecordSourceCombo->currentText().trimmed() : QString();
    if (recordSource.isEmpty()) {
        return QStringLiteral("3943B 候选数据源不能为空。");
    }

    const QString selectedReportPath = m_ceyearReportPathCombo ? m_ceyearReportPathCombo->currentText().trimmed().toUpper() : QString();
    if (selectedReportPath.isEmpty()) {
        return QStringLiteral("3943B 上报路径不能为空。");
    }
    if (selectedReportPath != QStringLiteral("TCP") && selectedReportPath != QStringLiteral("UDP")) {
        return QStringLiteral("3943B PC 端保存目前只支持 TCP 或 UDP 上报。");
    }

    const QString pcProtocol = m_ceyearPcReceiveProtocolCombo ? m_ceyearPcReceiveProtocolCombo->currentText().trimmed().toUpper() : selectedReportPath;
    if (pcProtocol != QStringLiteral("TCP") && pcProtocol != QStringLiteral("UDP")) {
        return QStringLiteral("PC 接收协议必须是 TCP 或 UDP。");
    }
    const QString reportPath = selectedReportPath;

    const QString pcIpAddress = m_ceyearPcReceiveIpEdit ? m_ceyearPcReceiveIpEdit->text().trimmed() : QString();
    QHostAddress pcHostAddress;
    if (pcIpAddress.isEmpty() || !pcHostAddress.setAddress(pcIpAddress)) {
        return QStringLiteral("PC 本机 IP 地址无效。");
    }

    ok = false;
    const int pcPort = m_ceyearPcReceivePortEdit ? m_ceyearPcReceivePortEdit->text().trimmed().toInt(&ok) : 0;
    if (!ok || pcPort <= 0 || pcPort > 65535) {
        return QStringLiteral("PC 接收端口必须是 1 到 65535 的整数。");
    }

    const QString receiverId = QStringLiteral("B");
    const QString targetModel = cleanCaptureToken(comboTextToken(m_ceyearTargetModelCombo, QStringLiteral("Yu4Pro")),
                                                  QStringLiteral("Yu4Pro"));
    const QString modelIndex = twoDigitCaptureToken(m_ceyearModelIndexCombo, QStringLiteral("01"));
    const QString distance = prefixedCaptureToken(m_ceyearDistanceCombo, QLatin1Char('D'), QStringLiteral("D02"));
    const QString height = prefixedCaptureToken(m_ceyearHeightCombo, QLatin1Char('H'), QStringLiteral("H02"));
    const QString band = prefixedCaptureToken(m_ceyearBandCombo, QLatin1Char('F'), QStringLiteral("F24"));
    const QString segment = segmentCaptureToken(m_ceyearSegmentEdit);
    const QDateTime now = QDateTime::currentDateTime();
    const QString fileName = QStringLiteral("%1_%2_%3_%4_%5_%6_%7_%8_%9.iqraw")
                                 .arg(receiverId,
                                      targetModel,
                                      modelIndex,
                                      distance,
                                      height,
                                      band,
                                      segment,
                                      now.toString(QStringLiteral("yyyyMMdd")),
                                      now.toString(QStringLiteral("HHmmss")));

    const QString rawDir = QDir(ProjectPaths::dataDirectory()).filePath(QStringLiteral("raw"));
    QDir().mkpath(rawDir);
    const QString pcOutputFile = QDir(rawDir).filePath(fileName);

    if (config) {
        config->centerFrequencyHz = control.centerFrequencyHz;
        config->frequencySpanHz = control.frequencySpanHz;
        config->measureMode = measureMode;
        config->measurementTimeSeconds = 0.0;
        config->iqPointCount = iqPointCount;
        config->recordEnabled = false;
        config->recordSource = recordSource;
        config->demodulation = m_ceyearDemodulationCombo ? m_ceyearDemodulationCombo->currentText().trimmed().toUpper() : QStringLiteral("IQ");
        config->reportPath = reportPath;
        config->pcReceiveProtocol = pcProtocol;
        config->pcReceiveIpAddress = pcIpAddress;
        config->pcReceivePort = pcPort;
        config->detector = control.detector;
        config->pcReceiveTimeoutMs = 0;
        config->outputFilePath = pcOutputFile;
    }
    if (m_ceyearPcOutputFileEdit) {
        m_ceyearPcOutputFileEdit->setText(pcOutputFile);
    }
    if (m_ceyearReportPathCombo && m_ceyearReportPathCombo->currentText().trimmed().toUpper() != reportPath) {
        m_ceyearReportPathCombo->setCurrentText(reportPath);
    }
    return {};
}

bool CapturePage::startCeyearCaptureFromUi(bool continuous)
{
    if (m_ceyearBusy || m_ceyearThread) {
        return false;
    }

    CeyearCaptureConfig config;
    captureDiagLog(QStringLiteral("3943B validateCeyearCapture before"));
    const QString validationError = validateCeyearCapture(&config);
    captureDiagLog(QStringLiteral("3943B validateCeyearCapture after; error=%1").arg(validationError));
    if (!validationError.isEmpty()) {
        appendValidationMessage(validationError, QStringLiteral("error"));
        appendLog(validationError);
        return false;
    }

    if (continuous) {
        ++m_ceyearContinuousRound;
        appendLog(QStringLiteral("3943B 连续采集第 %1 轮开始：目标 IQ 点数 %2，输出 %3")
                      .arg(m_ceyearContinuousRound)
                      .arg(config.iqPointCount)
                      .arg(QDir::toNativeSeparators(config.outputFilePath)));
    }

    appendValidationMessage(continuous
                                ? QStringLiteral("3943B 连续采集参数已通过校验。")
                                : QStringLiteral("3943B 上报路径参数已通过校验。"),
                            QStringLiteral("good"));
    appendLog(QStringLiteral("设备类型：3943B"));
    appendLog(QStringLiteral("3943B 上报路径参数：Freq %1 Hz, Span %2 Hz, Mode %3, IQ 点数 %4")
                  .arg(config.centerFrequencyHz, 0, 'f', 0)
                  .arg(config.frequencySpanHz, 0, 'f', 0)
                  .arg(config.measureMode)
                  .arg(config.iqPointCount));
    captureDiagLog(QStringLiteral("3943B startCeyearCaptureAsync before; resource=%1 pc=%2:%3 output=%4")
                       .arg(m_ceyearResourceEdit ? m_ceyearResourceEdit->text().trimmed() : QString(),
                            config.pcReceiveIpAddress)
                       .arg(config.pcReceivePort)
                       .arg(config.outputFilePath));
    m_ceyearCurrentCaptureTimer.restart();
    startCeyearCaptureAsync(config);
    return true;
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
    QPointer<CapturePage> page(this);
    QThread *thread = QThread::create([page, service, config]() {
        UsrpCaptureResult result;
        try {
            result = service->captureToCap(config);
        } catch (const std::exception &ex) {
            result.message = QStringLiteral("USRP capture crashed before completion: %1").arg(QString::fromLocal8Bit(ex.what()));
        } catch (...) {
            result.message = QStringLiteral("USRP capture crashed before completion.");
        }
        if (!page) {
            return;
        }
        QMetaObject::invokeMethod(page, [page, result, config]() {
            if (!page) {
                return;
            }
            page->finalizeCapture(result, config);
        }, Qt::QueuedConnection);
    });
    m_captureThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_captureThread == thread) {
            m_captureThread = nullptr;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CapturePage::startCeyearCaptureAsync(const CeyearCaptureConfig &config)
{
    captureDiagLog(QStringLiteral("startCeyearCaptureAsync entered; existingThread=%1 service=%2")
                       .arg(m_ceyearThread ? QStringLiteral("true") : QStringLiteral("false"),
                            m_ceyearService ? QStringLiteral("true") : QStringLiteral("false")));
    if (!m_ceyearService || m_ceyearThread || m_ceyearBusy) {
        captureDiagLog(QStringLiteral("startCeyearCaptureAsync early return"));
        return;
    }

    const QString resource = m_ceyearResourceEdit ? m_ceyearResourceEdit->text().trimmed() : defaultCeyearVisaResource();
    const QString normalizedResource = resource.isEmpty() ? defaultCeyearVisaResource() : resource;
    if (m_ceyearResourceEdit && m_ceyearResourceEdit->text() != normalizedResource) {
        m_ceyearResourceEdit->setText(normalizedResource);
    }

    m_ceyearBusy = true;
    setBusy(true);
    if (m_taskMetric) {
        m_taskMetric->setValue(QStringLiteral("采集中"));
    }
    if (m_taskStateLabel) {
        m_taskStateLabel->setText(QStringLiteral("3943B 采集中"));
        m_taskStateLabel->setProperty("state", QStringLiteral("running"));
        refreshWidgetStyle(m_taskStateLabel);
    }
    appendValidationMessage(QStringLiteral("正在触发 3943B IQ 上报并保存 IQRAW。"), QStringLiteral("neutral"));
    appendLog(QStringLiteral("启动 3943B IQ 采集：%1").arg(normalizedResource));
    appendLog(QStringLiteral("PC 端接收目标：%1 %2:%3 -> %4")
                  .arg(config.pcReceiveProtocol, config.pcReceiveIpAddress)
                  .arg(config.pcReceivePort)
                  .arg(QDir::toNativeSeparators(config.outputFilePath)));
    if (m_fileStateLabel) {
        m_fileStateLabel->setText(QStringLiteral("输出文件：%1").arg(QDir::toNativeSeparators(config.outputFilePath)));
    }

    auto *service = m_ceyearService.get();
    QPointer<CapturePage> page(this);
    QThread *thread = QThread::create([page, service, normalizedResource, config]() {
        captureDiagLog(QStringLiteral("3943B worker entered"));
        CeyearCaptureResult result;
        try {
            captureDiagLog(QStringLiteral("3943B worker before triggerIqReport"));
            result = service->triggerIqReport(normalizedResource, config);
            captureDiagLog(QStringLiteral("3943B worker after triggerIqReport; success=%1 bytes=%2 message=%3")
                               .arg(result.success ? QStringLiteral("true") : QStringLiteral("false"))
                               .arg(result.bytesReceived)
                               .arg(result.message));
            if (result.success && result.bytesReceived > 0) {
                const QString rawPath = result.outputFilePath.isEmpty() ? config.outputFilePath : result.outputFilePath;
                captureDiagLog(QStringLiteral("3943B worker saved IQRAW only; raw=%1").arg(rawPath));
                result.logs << QStringLiteral("3943B IQ raw saved to %1").arg(rawPath);
                result.logs << QStringLiteral("3943B IQ bytes received: %1").arg(result.bytesReceived);
                result.outputFilePath = rawPath;
            }
        } catch (const std::exception &ex) {
            result.resource = normalizedResource;
            result.captureName = config.outputFilePath;
            result.outputFilePath = config.outputFilePath;
            result.message = QStringLiteral("3943B IQ capture crashed before completion: %1").arg(QString::fromLocal8Bit(ex.what()));
            captureDiagLog(result.message);
        } catch (...) {
            result.resource = normalizedResource;
            result.captureName = config.outputFilePath;
            result.outputFilePath = config.outputFilePath;
            result.message = QStringLiteral("3943B IQ capture crashed before completion.");
            captureDiagLog(result.message);
        }
        if (!page) {
            captureDiagLog(QStringLiteral("3943B worker page gone before invoke"));
            return;
        }
        captureDiagLog(QStringLiteral("3943B worker before invoke finalize"));
        QMetaObject::invokeMethod(page, [page, result, config]() {
            if (!page) {
                captureDiagLog(QStringLiteral("3943B finalize skipped; page gone"));
                return;
            }
            captureDiagLog(QStringLiteral("3943B finalize entered"));
            page->finalizeCeyearCapture(result, config);
            captureDiagLog(QStringLiteral("3943B finalize returned"));
        }, Qt::QueuedConnection);
        captureDiagLog(QStringLiteral("3943B worker invoke finalize queued"));
    });
    m_ceyearThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        captureDiagLog(QStringLiteral("3943B thread finished signal"));
        if (m_ceyearThread == thread) {
            m_ceyearThread = nullptr;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    captureDiagLog(QStringLiteral("3943B thread start before"));
    thread->start();
    captureDiagLog(QStringLiteral("3943B thread start after"));
}

void CapturePage::scheduleNextCeyearContinuousCapture()
{
    if (!m_ceyearContinuousActive || !m_ceyearContinuousTimer) {
        return;
    }

    const qint64 elapsedMs = m_ceyearCurrentCaptureTimer.isValid() ? m_ceyearCurrentCaptureTimer.elapsed() : 0;
    appendLog(QStringLiteral("3943B 连续采集第 %1 轮已结束，用时 %2 s；立即启动第 %3 轮。")
                  .arg(m_ceyearContinuousRound)
                  .arg(elapsedMs / 1000.0, 0, 'f', 3)
                  .arg(m_ceyearContinuousRound + 1));
    if (m_taskMetric) {
        m_taskMetric->setValue(QStringLiteral("连续采集中"));
    }
    if (m_taskStateLabel) {
        m_taskStateLabel->setText(QStringLiteral("连续采集中"));
        m_taskStateLabel->setProperty("state", QStringLiteral("running"));
        refreshWidgetStyle(m_taskStateLabel);
    }
    m_ceyearContinuousTimer->start(0);
}

void CapturePage::cancelCeyearContinuousCapture()
{
    if (m_ceyearContinuousTimer) {
        m_ceyearContinuousTimer->stop();
    }
    if (m_ceyearContinuousActive) {
        appendLog(QStringLiteral("3943B 连续采集已停止。"));
    }
    m_ceyearContinuousActive = false;
    m_ceyearContinuousRound = 0;
}

void CapturePage::stopCeyearCaptureAsync()
{
    if (isUsrpSelected() || !m_ceyearService) {
        return;
    }

    cancelCeyearContinuousCapture();
    if (m_ceyearBusy) {
        m_ceyearService->requestStop();
        appendValidationMessage(QStringLiteral("已请求停止当前 3943B 上报，正在关闭接收并发送停止命令。"), QStringLiteral("neutral"));
        appendLog(QStringLiteral("请求停止当前 3943B 上报，接收端将立即中止等待点数。"));
        setBusy(true);
        return;
    }

    const QString resource = m_ceyearResourceEdit ? m_ceyearResourceEdit->text().trimmed() : defaultCeyearVisaResource();
    const QString normalizedResource = resource.isEmpty() ? defaultCeyearVisaResource() : resource;
    if (m_ceyearResourceEdit && m_ceyearResourceEdit->text() != normalizedResource) {
        m_ceyearResourceEdit->setText(normalizedResource);
    }

    m_ceyearBusy = true;
    setBusy(true);
    appendValidationMessage(QStringLiteral("正在停止 3943B 上报..."), QStringLiteral("neutral"));
    appendLog(QStringLiteral("停止 3943B 上报：%1").arg(normalizedResource));

    CeyearCaptureConfig config;
    config.outputFilePath = QStringLiteral("3943B_CAPTURE");
    auto *service = m_ceyearService.get();
    QPointer<CapturePage> page(this);
    QThread *thread = QThread::create([page, service, normalizedResource, config]() {
        CeyearCaptureResult result;
        try {
            result = service->stopCapture(normalizedResource);
        } catch (const std::exception &ex) {
            result.resource = normalizedResource;
            result.captureName = config.outputFilePath;
            result.message = QStringLiteral("3943B stop crashed before completion: %1").arg(QString::fromLocal8Bit(ex.what()));
        } catch (...) {
            result.resource = normalizedResource;
            result.captureName = config.outputFilePath;
            result.message = QStringLiteral("3943B stop crashed before completion.");
        }
        if (!page) {
            return;
        }
        QMetaObject::invokeMethod(page, [page, result, config]() {
            if (!page) {
                return;
            }
            page->finalizeCeyearCapture(result, config);
        }, Qt::QueuedConnection);
    });
    m_ceyearThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_ceyearThread == thread) {
            m_ceyearThread = nullptr;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CapturePage::queryCeyearStatusAsync()
{
    if (!m_ceyearService || m_ceyearThread || m_ceyearBusy) {
        return;
    }

    const QString resource = m_ceyearResourceEdit ? m_ceyearResourceEdit->text().trimmed() : defaultCeyearVisaResource();
    const QString normalizedResource = resource.isEmpty() ? defaultCeyearVisaResource() : resource;
    if (m_ceyearResourceEdit && m_ceyearResourceEdit->text() != normalizedResource) {
        m_ceyearResourceEdit->setText(normalizedResource);
    }

    m_ceyearBusy = true;
    setBusy(true);
    if (m_deviceMetric) {
        m_deviceMetric->setValue(QStringLiteral("查询中"));
    }
    appendValidationMessage(QStringLiteral("正在查询 3943B 状态..."), QStringLiteral("neutral"));
    appendLog(QStringLiteral("开始查询 3943B：%1").arg(normalizedResource));

    auto *service = m_ceyearService.get();
    QPointer<CapturePage> page(this);
    QThread *thread = QThread::create([page, service, normalizedResource]() {
        CeyearStatusResult result;
        try {
            result = service->queryStatus(normalizedResource);
        } catch (const std::exception &ex) {
            result.resource = normalizedResource;
            result.message = QStringLiteral("3943B status query crashed before completion: %1").arg(QString::fromLocal8Bit(ex.what()));
        } catch (...) {
            result.resource = normalizedResource;
            result.message = QStringLiteral("3943B status query crashed before completion.");
        }
        if (!page) {
            return;
        }
        QMetaObject::invokeMethod(page, [page, result]() {
            if (!page) {
                return;
            }
            page->finalizeCeyearStatus(result);
        }, Qt::QueuedConnection);
    });
    m_ceyearThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_ceyearThread == thread) {
            m_ceyearThread = nullptr;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
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
        const QString resource = m_ceyearResourceEdit ? m_ceyearResourceEdit->text().trimmed() : defaultCeyearVisaResource();
        m_connectionSummary->setText(QStringLiteral("当前选择 3943B。LAN/VISA：%1").arg(resource.isEmpty() ? defaultCeyearVisaResource() : resource));
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
    const bool ceyearLocked = busy || m_ceyearContinuousActive;
    if (m_discoverButton) {
        m_discoverButton->setEnabled(!ceyearLocked);
    }
    if (m_captureButton) {
        m_captureButton->setEnabled(!ceyearLocked && (usrp ? !m_devices.isEmpty() : true));
    }
    if (m_deviceTypeCombo) {
        m_deviceTypeCombo->setEnabled(!ceyearLocked);
    }
    if (m_usrpSourceButton) {
        m_usrpSourceButton->setEnabled(!ceyearLocked);
    }
    if (m_3943bSourceButton) {
        m_3943bSourceButton->setEnabled(!ceyearLocked);
    }
    if (m_ceyearResourceEdit) {
        m_ceyearResourceEdit->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearCenterFrequencyEdit) {
        m_ceyearCenterFrequencyEdit->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearFrequencySpanEdit) {
        m_ceyearFrequencySpanEdit->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearDemodulationCombo) {
        m_ceyearDemodulationCombo->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearDetectorCombo) {
        m_ceyearDetectorCombo->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearMeasureModeCombo) {
        m_ceyearMeasureModeCombo->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearIqPointCountEdit) {
        m_ceyearIqPointCountEdit->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearRecordSourceCombo) {
        m_ceyearRecordSourceCombo->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearReportPathCombo) {
        m_ceyearReportPathCombo->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearPcReceiveProtocolCombo) {
        m_ceyearPcReceiveProtocolCombo->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearPcReceiveIpEdit) {
        m_ceyearPcReceiveIpEdit->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearPcReceivePortEdit) {
        m_ceyearPcReceivePortEdit->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearRecordEnableCheck) {
        m_ceyearRecordEnableCheck->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_ceyearContinuousCaptureCheck) {
        m_ceyearContinuousCaptureCheck->setEnabled(!ceyearLocked && !usrp);
    }
    if (m_stopCeyearCaptureButton) {
        m_stopCeyearCaptureButton->setEnabled(!usrp);
        if (usrp) {
            m_stopCeyearCaptureButton->setText(QStringLiteral("停止 3943B 采集"));
        } else if (m_ceyearContinuousActive) {
            m_stopCeyearCaptureButton->setText(QStringLiteral("停止连续采集"));
        } else if (m_ceyearBusy || m_ceyearThread) {
            m_stopCeyearCaptureButton->setText(QStringLiteral("停止当前上报"));
        } else {
            m_stopCeyearCaptureButton->setText(QStringLiteral("停止 3943B 采集"));
        }
    }
}

void CapturePage::finalizeCapture(const UsrpCaptureResult &result, const UsrpCaptureConfig &config)
{
    for (const auto &line : result.logs) {
        appendLog(line);
    }
    if (!result.outputFilePath.isEmpty()) {
        appendLog(QStringLiteral("PC 保存文件：%1").arg(QDir::toNativeSeparators(result.outputFilePath)));
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
        emit captureFileReady(result.outputFilePath);
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

void CapturePage::finalizeCeyearStatus(const CeyearStatusResult &result)
{
    if (m_ceyearStatusTable) {
        m_ceyearStatusTable->setRowCount(0);
        for (const auto &item : result.items) {
            const int row = m_ceyearStatusTable->rowCount();
            m_ceyearStatusTable->insertRow(row);
            m_ceyearStatusTable->setItem(row, 0, readonlyItem(item.label));
            m_ceyearStatusTable->setItem(row, 1, readonlyItem(item.command));
            m_ceyearStatusTable->setItem(row, 2, readonlyItem(item.success ? item.response : item.error));
            m_ceyearStatusTable->setItem(row, 3, readonlyItem(item.success ? QStringLiteral("正常") : QStringLiteral("失败")));
        }
        if (m_resultsTabs) {
            m_resultsTabs->setCurrentWidget(m_ceyearStatusTable);
        }
    }

    appendLog(result.message);
    appendValidationMessage(result.message, result.success ? QStringLiteral("good") : QStringLiteral("error"));
    if (m_deviceMetric) {
        m_deviceMetric->setValue(result.success ? QStringLiteral("在线") : QStringLiteral("异常"));
    }
    updateConnectionSummary();
    m_ceyearBusy = false;
    setBusy(false);
}

void CapturePage::finalizeCeyearCapture(const CeyearCaptureResult &result, const CeyearCaptureConfig &config)
{
    for (const auto &line : result.logs) {
        appendLog(line);
    }

    if (m_ceyearStatusTable) {
        m_ceyearStatusTable->setRowCount(0);
        for (const auto &item : result.items) {
            const int row = m_ceyearStatusTable->rowCount();
            m_ceyearStatusTable->insertRow(row);
            m_ceyearStatusTable->setItem(row, 0, readonlyItem(item.label));
            m_ceyearStatusTable->setItem(row, 1, readonlyItem(item.command));
            m_ceyearStatusTable->setItem(row, 2, readonlyItem(item.success ? item.response : item.error));
            m_ceyearStatusTable->setItem(row, 3, readonlyItem(item.success ? QStringLiteral("正常") : QStringLiteral("失败")));
        }
    }

    appendLog(result.message);
    appendValidationMessage(result.message, result.success ? QStringLiteral("good") : QStringLiteral("error"));

    if (result.success && result.bytesReceived > 0 && m_captureTable) {
        const int row = m_captureTable->rowCount();
        m_captureTable->insertRow(row);
        const QString fileName = result.outputFilePath.isEmpty()
            ? (result.captureName.isEmpty() ? config.outputFilePath : result.captureName)
            : QFileInfo(result.outputFilePath).fileName();
        m_captureTable->setItem(row, 0, readonlyItem(fileName));
        m_captureTable->setItem(row, 1, readonlyItem(QStringLiteral("3943B")));
        m_captureTable->setItem(row, 2, readonlyItem(QStringLiteral("%1 MHz").arg(config.centerFrequencyHz / 1000000.0, 0, 'f', 3)));
        const QString dataText = result.sampleCount > 0
            ? QStringLiteral("%1 / %2 B").arg(result.sampleCount).arg(result.bytesReceived)
            : QStringLiteral("%1 B").arg(result.bytesReceived);
        m_captureTable->setItem(row, 3, readonlyItem(dataText));
        m_captureTable->setItem(row, 4, readonlyItem(QStringLiteral("完成")));
        if (m_fileMetric) {
            m_fileMetric->setValue(QString::number(m_captureTable->rowCount()));
        }
        emit captureFileReady(result.outputFilePath.isEmpty() ? config.outputFilePath : result.outputFilePath);
    }

    if (m_resultsTabs) {
        m_resultsTabs->setCurrentWidget(result.success && result.bytesReceived > 0 && m_captureTable
                                            ? static_cast<QWidget *>(m_captureTable)
                                            : static_cast<QWidget *>(m_ceyearStatusTable));
    }
    if (m_taskMetric) {
        m_taskMetric->setValue(result.success ? QStringLiteral("完成") : QStringLiteral("失败"));
    }
    if (m_taskStateLabel) {
        m_taskStateLabel->setText(result.success ? QStringLiteral("完成") : QStringLiteral("失败"));
        m_taskStateLabel->setProperty("state", result.success ? QStringLiteral("done") : QStringLiteral("error"));
        refreshWidgetStyle(m_taskStateLabel);
    }

    updateConnectionSummary();
    m_ceyearBusy = false;
    const bool shouldContinue = m_ceyearContinuousActive
        && config.outputFilePath.endsWith(QStringLiteral(".iqraw"), Qt::CaseInsensitive);
    if (shouldContinue && !result.success) {
        appendLog(QStringLiteral("3943B 连续采集因本轮失败而停止。"));
        cancelCeyearContinuousCapture();
    }
    setBusy(false);
    if (shouldContinue && result.success) {
        scheduleNextCeyearContinuousCapture();
        setBusy(false);
    }
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
