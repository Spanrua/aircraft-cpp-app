#include "ui/pages/PreprocessPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "services/CapProbeService.h"
#include "services/PreprocessProcessRunner.h"
#include "ui/widgets/MetricCard.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QFrame>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QVBoxLayout>

namespace
{
constexpr int kPathEditMinWidth = 520;
constexpr int kPathEditMinHeight = 42;
constexpr int kParameterEditMinWidth = 180;
constexpr int kParameterEditMinHeight = 42;

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

QFrame *panelFrame(const QString &objectName, QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setFrameShadow(QFrame::Plain);
    return frame;
}

QLineEdit *parameterEdit(const QString &objectName, const QString &value, QWidget *parent)
{
    auto *edit = new QLineEdit(value, parent);
    edit->setObjectName(objectName);
    edit->setMinimumHeight(kParameterEditMinHeight);
    edit->setMinimumWidth(kParameterEditMinWidth);
    return edit;
}

QLineEdit *pathEdit(const QString &objectName, const QString &value, QWidget *parent)
{
    auto *edit = new QLineEdit(value, parent);
    edit->setObjectName(objectName);
    edit->setMinimumHeight(kPathEditMinHeight);
    edit->setMinimumWidth(kPathEditMinWidth);
    return edit;
}

QFrame *workbenchColumn(const QString &objectName, const QString &title, QWidget *parent, QVBoxLayout **layoutOut)
{
    auto *column = panelFrame(objectName, parent);
    column->setMinimumWidth(objectName == QStringLiteral("PreprocessParametersColumn") ? 620 : 360);

    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto *titleLabel = new QLabel(title, column);
    titleLabel->setObjectName(objectName + QStringLiteral("Title"));
    titleLabel->setProperty("sectionTitle", true);
    layout->addWidget(titleLabel);

    if (layoutOut) {
        *layoutOut = layout;
    }
    return column;
}

double readDouble(const QLineEdit *edit, double fallback)
{
    bool ok = false;
    const double value = edit ? edit->text().trimmed().toDouble(&ok) : fallback;
    return ok ? value : fallback;
}

int readInt(const QLineEdit *edit, int fallback)
{
    bool ok = false;
    const int value = edit ? edit->text().trimmed().toInt(&ok) : fallback;
    return ok ? value : fallback;
}
} // namespace

PreprocessPage::PreprocessPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("信号预处理"), QStringLiteral("读取 CAP 头信息，配置切片与样本输出参数，并启动预处理任务。"), parent, true)
{
    buildUi();
}

PreprocessPage::~PreprocessPage()
{
    if (m_preprocessThread) {
        m_preprocessThread->quit();
        m_preprocessThread->wait();
    }
}

void PreprocessPage::buildUi()
{
    if (auto *scrollContent = findChild<QWidget *>(QStringLiteral("WorkflowPageScrollContent"))) {
        scrollContent->setMinimumHeight(720);
    }

    auto *workbench = new QHBoxLayout();
    workbench->setSpacing(14);

    QVBoxLayout *inputLayout = nullptr;
    auto *inputColumn = workbenchColumn(QStringLiteral("PreprocessInputColumn"), QStringLiteral("输入"), this, &inputLayout);

    m_capPathEdit = pathEdit(QStringLiteral("PreprocessCapPathEdit"), QDir(ProjectPaths::workspaceRoot()).filePath("IQ_2025_01_09_13_55_30.cap"), inputColumn);
    m_capPathEdit->setPlaceholderText(QStringLiteral("选择或输入 .cap 文件路径"));
    inputLayout->addWidget(fieldBlock(QStringLiteral("CAP 文件"), m_capPathEdit, inputColumn));

    auto *fileActions = new QHBoxLayout();
    auto *browseButton = new QPushButton(QStringLiteral("选择文件"), inputColumn);
    browseButton->setObjectName(QStringLiteral("PreprocessBrowseCapButton"));
    m_previewButton = new QPushButton(QStringLiteral("读取头信息"), inputColumn);
    m_previewButton->setObjectName(QStringLiteral("PreprocessPreviewButton"));
    m_previewButton->setProperty("secondary", true);
    fileActions->addWidget(browseButton);
    fileActions->addWidget(m_previewButton);
    inputLayout->addLayout(fileActions);

    m_validationMessage = new QLabel(QStringLiteral("等待读取 CAP 文件。"), inputColumn);
    m_validationMessage->setObjectName(QStringLiteral("PreprocessValidationMessage"));
    m_validationMessage->setWordWrap(true);
    m_validationMessage->setProperty("kind", QStringLiteral("neutral"));
    inputLayout->addWidget(m_validationMessage);

    m_capMetric = new MetricCard(QStringLiteral("CAP 状态"), inputColumn);
    m_capMetric->setValue(QStringLiteral("未读取"));
    inputLayout->addWidget(m_capMetric);

    auto *summaryBox = panelFrame(QStringLiteral("PreprocessSummaryPanel"), inputColumn);
    auto *summaryLayout = new QFormLayout(summaryBox);
    summaryLayout->setContentsMargins(14, 14, 14, 14);
    summaryLayout->setSpacing(9);
    auto *summaryTitle = new QLabel(QStringLiteral("CAP 摘要"), summaryBox);
    summaryTitle->setProperty("muted", true);
    summaryLayout->setWidget(0, QFormLayout::SpanningRole, summaryTitle);
    m_versionLabel = new QLabel("-", summaryBox);
    m_headerLengthLabel = new QLabel("-", summaryBox);
    m_bandwidthLabel = new QLabel("-", summaryBox);
    m_sampleRateLabel = new QLabel("-", summaryBox);
    m_centerFrequencyLabel = new QLabel("-", summaryBox);
    m_sampleCountLabel = new QLabel("-", summaryBox);
    summaryLayout->addRow(QStringLiteral("版本"), m_versionLabel);
    summaryLayout->addRow(QStringLiteral("头长度"), m_headerLengthLabel);
    summaryLayout->addRow(QStringLiteral("带宽"), m_bandwidthLabel);
    summaryLayout->addRow(QStringLiteral("采样率"), m_sampleRateLabel);
    summaryLayout->addRow(QStringLiteral("中心频率"), m_centerFrequencyLabel);
    summaryLayout->addRow(QStringLiteral("样本数"), m_sampleCountLabel);
    inputLayout->addWidget(summaryBox);
    inputLayout->addStretch();
    workbench->addWidget(inputColumn, 3);

    QVBoxLayout *parametersLayout = nullptr;
    auto *parametersColumn = workbenchColumn(QStringLiteral("PreprocessParametersColumn"), QStringLiteral("预处理参数"), this, &parametersLayout);

    auto *hint = new QLabel(QStringLiteral("默认值适合先验证 USRP B210 采集的 CAP 文件；需要调算法时可直接修改数值。"), parametersColumn);
    hint->setObjectName(QStringLiteral("PreprocessParametersHint"));
    hint->setWordWrap(true);
    hint->setProperty("muted", true);
    parametersLayout->addWidget(hint);

    m_outputDirEdit = pathEdit(QStringLiteral("PreprocessOutputDirEdit"), QDir(ProjectPaths::samplesDirectory()).filePath("preprocess"), parametersColumn);
    m_modelPathEdit = pathEdit(QStringLiteral("PreprocessModelPathEdit"), QDir(ProjectPaths::modelsDirectory()).filePath("model_weights.pth"), parametersColumn);

    auto *pathGroup = panelFrame(QStringLiteral("PreprocessPathParametersPanel"), parametersColumn);
    pathGroup->setObjectName(QStringLiteral("PreprocessPathParametersPanel"));
    pathGroup->setMinimumHeight(156);
    auto *pathLayout = new QGridLayout(pathGroup);
    pathLayout->setContentsMargins(14, 14, 14, 14);
    pathLayout->setHorizontalSpacing(14);
    pathLayout->setVerticalSpacing(12);
    auto *pathTitle = new QLabel(QStringLiteral("输出与模型"), pathGroup);
    pathTitle->setProperty("muted", true);
    pathLayout->addWidget(pathTitle, 0, 0, 1, 2);
    pathLayout->addWidget(fieldBlock(QStringLiteral("样本输出目录"), m_outputDirEdit, pathGroup), 1, 0, 1, 2);
    pathLayout->addWidget(fieldBlock(QStringLiteral("模型权重路径"), m_modelPathEdit, pathGroup), 2, 0, 1, 2);
    pathLayout->setColumnStretch(0, 1);
    pathLayout->setColumnStretch(1, 1);
    parametersLayout->addWidget(pathGroup);

    m_sliceLengthEdit = parameterEdit(QStringLiteral("PreprocessSliceLengthEdit"), QStringLiteral("4096"), parametersColumn);
    m_energyThresholdEdit = parameterEdit(QStringLiteral("PreprocessEnergyThresholdEdit"), QStringLiteral("1.0"), parametersColumn);
    m_aiConfidenceEdit = parameterEdit(QStringLiteral("PreprocessAiConfidenceEdit"), QStringLiteral("0.85"), parametersColumn);

    auto *basicGroup = panelFrame(QStringLiteral("PreprocessBasicParametersPanel"), parametersColumn);
    basicGroup->setObjectName(QStringLiteral("PreprocessBasicParametersPanel"));
    basicGroup->setMinimumHeight(156);
    auto *basicGrid = new QGridLayout(basicGroup);
    basicGrid->setContentsMargins(14, 14, 14, 14);
    basicGrid->setHorizontalSpacing(14);
    basicGrid->setVerticalSpacing(12);
    auto *basicTitle = new QLabel(QStringLiteral("基础参数"), basicGroup);
    basicTitle->setProperty("muted", true);
    basicGrid->addWidget(basicTitle, 0, 0, 1, 3);
    basicGrid->addWidget(fieldBlock(QStringLiteral("切片长度"), m_sliceLengthEdit, basicGroup), 1, 0);
    basicGrid->addWidget(fieldBlock(QStringLiteral("能量阈值 dB"), m_energyThresholdEdit, basicGroup), 1, 1);
    basicGrid->addWidget(fieldBlock(QStringLiteral("AI 置信阈值"), m_aiConfidenceEdit, basicGroup), 1, 2);
    basicGrid->setColumnStretch(0, 1);
    basicGrid->setColumnStretch(1, 1);
    basicGrid->setColumnStretch(2, 1);
    parametersLayout->addWidget(basicGroup);

    m_advancedParamsToggle = new QPushButton(QStringLiteral("显示高级参数"), parametersColumn);
    m_advancedParamsToggle->setObjectName(QStringLiteral("PreprocessAdvancedParametersToggle"));
    m_advancedParamsToggle->setCheckable(true);
    m_advancedParamsToggle->setProperty("secondary", true);
    parametersLayout->addWidget(m_advancedParamsToggle, 0, Qt::AlignLeft);

    m_noiseFloorEdit = parameterEdit(QStringLiteral("PreprocessNoiseFloorEdit"), QStringLiteral("-90.0"), parametersColumn);
    m_minBandwidthEdit = parameterEdit(QStringLiteral("PreprocessMinBandwidthEdit"), QStringLiteral("6.0"), parametersColumn);
    m_minDurationEdit = parameterEdit(QStringLiteral("PreprocessMinDurationEdit"), QStringLiteral("0.05"), parametersColumn);

    m_advancedParamsPanel = panelFrame(QStringLiteral("PreprocessAdvancedParametersPanel"), parametersColumn);
    m_advancedParamsPanel->setObjectName(QStringLiteral("PreprocessAdvancedParametersPanel"));
    auto *advancedGrid = new QGridLayout(m_advancedParamsPanel);
    advancedGrid->setContentsMargins(14, 14, 14, 14);
    advancedGrid->setHorizontalSpacing(14);
    advancedGrid->setVerticalSpacing(12);
    auto *advancedTitle = new QLabel(QStringLiteral("高级参数"), m_advancedParamsPanel);
    advancedTitle->setProperty("muted", true);
    advancedGrid->addWidget(advancedTitle, 0, 0, 1, 3);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("噪声底 dBm"), m_noiseFloorEdit, m_advancedParamsPanel), 1, 0);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("最小带宽 MHz"), m_minBandwidthEdit, m_advancedParamsPanel), 1, 1);
    advancedGrid->addWidget(fieldBlock(QStringLiteral("最小时长 ms"), m_minDurationEdit, m_advancedParamsPanel), 1, 2);
    advancedGrid->setColumnStretch(0, 1);
    advancedGrid->setColumnStretch(1, 1);
    advancedGrid->setColumnStretch(2, 1);
    m_advancedParamsPanel->setVisible(false);
    parametersLayout->addWidget(m_advancedParamsPanel);
    parametersLayout->addStretch();
    workbench->addWidget(parametersColumn, 4);

    QVBoxLayout *runLayout = nullptr;
    auto *runColumn = workbenchColumn(QStringLiteral("PreprocessRunColumn"), QStringLiteral("运行"), this, &runLayout);

    m_segmentMetric = new MetricCard(QStringLiteral("候选片段"), runColumn);
    m_segmentMetric->setValue(QStringLiteral("0"));
    m_sampleMetric = new MetricCard(QStringLiteral("输出样本"), runColumn);
    m_sampleMetric->setValue(QStringLiteral("0"));
    runLayout->addWidget(m_segmentMetric);
    runLayout->addWidget(m_sampleMetric);

    m_runStateLabel = new QLabel(QStringLiteral("待执行"), runColumn);
    m_runStateLabel->setObjectName(QStringLiteral("PreprocessRunStateLabel"));
    m_runStateLabel->setProperty("state", QStringLiteral("idle"));
    runLayout->addWidget(m_runStateLabel);

    m_outputSummaryLabel = new QLabel(QStringLiteral("输出目录：%1").arg(QDir::toNativeSeparators(m_outputDirEdit->text())), runColumn);
    m_outputSummaryLabel->setObjectName(QStringLiteral("PreprocessOutputSummaryLabel"));
    m_outputSummaryLabel->setWordWrap(true);
    m_outputSummaryLabel->setProperty("muted", true);
    runLayout->addWidget(m_outputSummaryLabel);

    m_runButton = new QPushButton(QStringLiteral("开始预处理"), runColumn);
    m_runButton->setObjectName(QStringLiteral("StartPreprocessButton"));
    m_runButton->setProperty("primary", true);
    m_runButton->setMinimumHeight(42);
    m_demoButton = new QPushButton(QStringLiteral("演示生成样本"), runColumn);
    m_demoButton->setObjectName(QStringLiteral("DemoPreprocessButton"));
    m_demoButton->setProperty("secondary", true);
    runLayout->addWidget(m_runButton);
    runLayout->addWidget(m_demoButton);

    m_logOutput = new QPlainTextEdit(runColumn);
    m_logOutput->setObjectName(QStringLiteral("PreprocessLog"));
    m_logOutput->setReadOnly(true);
    m_logOutput->setMinimumHeight(260);
    m_logOutput->setPlainText(QStringLiteral("等待 CAP 预览或预处理任务。"));
    runLayout->addWidget(m_logOutput, 1);
    workbench->addWidget(runColumn, 3);

    contentLayout()->addLayout(workbench, 6);

    auto *resultsGroup = new QGroupBox(QStringLiteral("结果"), this);
    resultsGroup->setObjectName(QStringLiteral("PreprocessResultsPanel"));
    resultsGroup->setMaximumHeight(220);
    auto *resultsLayout = new QVBoxLayout(resultsGroup);
    resultsLayout->setContentsMargins(16, 20, 16, 16);
    resultsLayout->setSpacing(10);

    m_emptyState = new QLabel(QStringLiteral("暂无预处理结果。读取 CAP 后可启动预处理任务。"), resultsGroup);
    m_emptyState->setObjectName(QStringLiteral("EmptyState"));
    resultsLayout->addWidget(m_emptyState);

    m_resultsTabs = new QTabWidget(resultsGroup);
    m_resultsTabs->setObjectName(QStringLiteral("PreprocessResultsTabs"));

    m_segmentTable = new QTableWidget(0, 6, m_resultsTabs);
    m_segmentTable->setObjectName(QStringLiteral("PreprocessSegmentTable"));
    m_segmentTable->setAlternatingRowColors(true);
    m_segmentTable->setHorizontalHeaderLabels({
        QStringLiteral("片段"),
        QStringLiteral("起始点"),
        QStringLiteral("结束点"),
        QStringLiteral("时长 ms"),
        QStringLiteral("置信度"),
        QStringLiteral("状态"),
    });
    m_segmentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_segmentTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_segmentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_segmentTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_segmentTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_segmentTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_segmentTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_segmentTable, QStringLiteral("候选片段"));

    m_sampleTable = new QTableWidget(0, 5, m_resultsTabs);
    m_sampleTable->setObjectName(QStringLiteral("PreprocessSampleTable"));
    m_sampleTable->setAlternatingRowColors(true);
    m_sampleTable->setHorizontalHeaderLabels({
        QStringLiteral("样本"),
        QStringLiteral("标签"),
        QStringLiteral("中心频率"),
        QStringLiteral("文件"),
        QStringLiteral("状态"),
    });
    m_sampleTable->horizontalHeader()->setStretchLastSection(true);
    m_sampleTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_sampleTable, QStringLiteral("输出样本"));
    resultsLayout->addWidget(m_resultsTabs, 1);
    contentLayout()->addWidget(resultsGroup, 1);

    connect(m_previewButton, &QPushButton::clicked, this, &PreprocessPage::previewCap);
    connect(m_runButton, &QPushButton::clicked, this, &PreprocessPage::runPreprocess);
    connect(m_demoButton, &QPushButton::clicked, this, &PreprocessPage::runDemoPreprocess);
    connect(m_outputDirEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_outputSummaryLabel) {
            m_outputSummaryLabel->setText(QStringLiteral("输出目录：%1").arg(QDir::toNativeSeparators(text)));
        }
    });
    connect(m_advancedParamsToggle, &QPushButton::toggled, this, [this](bool checked) {
        if (m_advancedParamsPanel) {
            m_advancedParamsPanel->setVisible(checked);
        }
        if (m_advancedParamsToggle) {
            m_advancedParamsToggle->setText(checked ? QStringLiteral("隐藏高级参数") : QStringLiteral("显示高级参数"));
        }
    });
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 CAP 文件"), ProjectPaths::workspaceRoot(), QStringLiteral("CAP 文件 (*.cap)"));
        if (!path.isEmpty()) {
            m_capPathEdit->setText(path);
            previewCap();
        }
    });
}

void PreprocessPage::previewCap()
{
    CapProbeService service;
    const auto result = service.probe(m_capPathEdit->text().trimmed());
    if (!result.has_value()) {
        setValidationMessage(service.lastError(), QStringLiteral("error"));
        if (m_capMetric) {
            m_capMetric->setValue(QStringLiteral("无效"));
        }
        m_logOutput->setPlainText(service.lastError());
        return;
    }

    updateCapSummary(*result);
    setValidationMessage(QStringLiteral("CAP 头信息读取完成。"), QStringLiteral("good"));
    if (m_capMetric) {
        m_capMetric->setValue(QStringLiteral("已读取"));
    }
    m_logOutput->setPlainText(QStringLiteral("CAP 预览完成。版本：%1，头长度：0x%2，IQ 预览点数：%3")
                                  .arg(result->version)
                                  .arg(result->headerLength, 0, 16)
                                  .arg(result->previewPairs.size()));
}

void PreprocessPage::runPreprocess()
{
    if (m_preprocessThread) {
        return;
    }

    const QString capPath = m_capPathEdit->text().trimmed();
    CapProbeService probeService;
    const auto capInfo = probeService.probe(capPath);
    if (!capInfo.has_value()) {
        setValidationMessage(probeService.lastError(), QStringLiteral("error"));
        appendLog(probeService.lastError());
        return;
    }

    updateCapSummary(*capInfo);
    setValidationMessage(QStringLiteral("参数已通过校验，开始预处理。"), QStringLiteral("good"));
    setRunState(QStringLiteral("预处理中"), QStringLiteral("running"));
    setBusy(true);
    m_logOutput->setPlainText(QStringLiteral("正在启动 Python 预处理任务..."));

    const PreprocessRequest request = buildRequest(capPath);
    m_preprocessThread = QThread::create([this, request, capInfo]() {
        QString error;
        auto result = PreprocessProcessRunner::run(request, *capInfo, &error);
        QMetaObject::invokeMethod(this, [this, result, error]() {
            m_preprocessThread = nullptr;
            setBusy(false);
            if (!result.has_value()) {
                appendLog(QStringLiteral("预处理失败：%1").arg(error));
                setRunState(QStringLiteral("失败"), QStringLiteral("error"));
                setValidationMessage(error, QStringLiteral("error"));
                m_emptyState->setText(QStringLiteral("预处理失败。请检查 CAP 文件、Python 环境和模型权重路径。"));
                m_emptyState->setVisible(true);
                return;
            }

            showRunResult(*result);
            if (result->success) {
                saveSampleRecords(result->sampleRecords);
            } else {
                setValidationMessage(result->message, QStringLiteral("error"));
            }
        }, Qt::QueuedConnection);
    });
    connect(m_preprocessThread, &QThread::finished, m_preprocessThread, &QObject::deleteLater);
    m_preprocessThread->start();
}

void PreprocessPage::runDemoPreprocess()
{
    const QString capPath = m_capPathEdit->text().trimmed();
    CapProbeService probeService;
    auto capInfo = probeService.probe(capPath);
    if (!capInfo.has_value()) {
        capInfo = CapProbeResult{};
        capInfo->bandwidthHz = 10000000.0;
        capInfo->sampleRateHz = 12800000.0;
        capInfo->centerFrequencyHz = 1090000000.0;
        capInfo->version = QStringLiteral("DEMO");
        capInfo->headerLength = 0x200;
        capInfo->frameSampleCount = 4096;
        m_logOutput->setPlainText(QStringLiteral("演示模式：CAP 文件不可用，已使用默认 1090 MHz / 12.8 MHz 参数。"));
    }

    updateCapSummary(*capInfo);
    setBusy(true);
    setRunState(QStringLiteral("生成演示样本"), QStringLiteral("running"));
    const auto result = PreprocessProcessRunner::runDemo(buildRequest(capPath.isEmpty() ? QDir(ProjectPaths::workspaceRoot()).filePath("demo.cap") : capPath), *capInfo);
    setBusy(false);

    showRunResult(result);
    saveSampleRecords(result.sampleRecords);
    setValidationMessage(QStringLiteral("演示样本已生成。"), QStringLiteral("good"));
}

void PreprocessPage::showRunResult(const PreprocessRunResult &result)
{
    m_segmentTable->setRowCount(0);
    for (const PreprocessSegment &segment : result.segments) {
        const int row = m_segmentTable->rowCount();
        m_segmentTable->insertRow(row);
        m_segmentTable->setItem(row, 0, readonlyItem(segment.segmentId));
        m_segmentTable->setItem(row, 1, readonlyItem(QString::number(segment.startSample)));
        m_segmentTable->setItem(row, 2, readonlyItem(QString::number(segment.endSample)));
        m_segmentTable->setItem(row, 3, readonlyItem(QString::number(segment.durationMs, 'f', 3)));
        m_segmentTable->setItem(row, 4, readonlyItem(QString::number(segment.score, 'f', 3)));
        m_segmentTable->setItem(row, 5, readonlyItem(segment.status));
    }

    m_sampleTable->setRowCount(0);
    for (const SampleRecord &record : result.sampleRecords) {
        const int row = m_sampleTable->rowCount();
        m_sampleTable->insertRow(row);
        m_sampleTable->setItem(row, 0, readonlyItem(record.sampleId));
        const QString label = record.labelIndividual.isEmpty() ? record.labelType : QStringLiteral("%1 / %2").arg(record.labelType, record.labelIndividual);
        m_sampleTable->setItem(row, 1, readonlyItem(label));
        m_sampleTable->setItem(row, 2, readonlyItem(QStringLiteral("%1 MHz").arg(record.centerFrequencyHz / 1000000.0, 0, 'f', 3)));
        m_sampleTable->setItem(row, 3, readonlyItem(QFileInfo(record.sampleFilePath).fileName()));
        m_sampleTable->setItem(row, 4, readonlyItem(record.status));
    }

    const bool hasRows = m_segmentTable->rowCount() > 0 || m_sampleTable->rowCount() > 0;
    m_emptyState->setVisible(!hasRows);
    m_emptyState->setText(hasRows ? QString() : QStringLiteral("预处理完成，但没有检测到可用片段。"));

    if (m_segmentMetric) {
        m_segmentMetric->setValue(QString::number(result.candidateSegmentCount));
    }
    if (m_sampleMetric) {
        m_sampleMetric->setValue(QString::number(result.sampleRecords.size()));
    }
    if (m_resultsTabs) {
        m_resultsTabs->setCurrentWidget(result.sampleRecords.isEmpty() ? m_segmentTable : m_sampleTable);
    }

    setRunState(result.success ? QStringLiteral("完成") : QStringLiteral("未成功"), result.success ? QStringLiteral("done") : QStringLiteral("error"));
    QStringList lines;
    lines << QStringLiteral("预处理完成：%1").arg(result.message);
    lines << QStringLiteral("候选片段：%1，有效输出样本：%2").arg(result.candidateSegmentCount).arg(result.sampleRecords.size());
    lines << result.logs;
    m_logOutput->setPlainText(lines.join('\n'));
}

void PreprocessPage::saveSampleRecords(const QVector<SampleRecord> &records)
{
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    for (const SampleRecord &record : records) {
        bool replaced = false;
        for (SampleRecord &existing : state.samples) {
            if (existing.sampleId == record.sampleId) {
                existing = record;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            state.samples.append(record);
        }
    }

    if (!store.save(state)) {
        appendLog(QStringLiteral("状态保存失败：%1").arg(store.lastError()));
        setValidationMessage(store.lastError(), QStringLiteral("error"));
    } else {
        appendLog(QStringLiteral("状态已保存：%1 个样本可在数据集页查看。").arg(records.size()));
    }
}

void PreprocessPage::setBusy(bool busy)
{
    if (m_runButton) {
        m_runButton->setEnabled(!busy);
    }
    if (m_demoButton) {
        m_demoButton->setEnabled(!busy);
    }
    if (m_previewButton) {
        m_previewButton->setEnabled(!busy);
    }
    if (m_capPathEdit) {
        m_capPathEdit->setEnabled(!busy);
    }
    if (m_outputDirEdit) {
        m_outputDirEdit->setEnabled(!busy);
    }
    if (m_modelPathEdit) {
        m_modelPathEdit->setEnabled(!busy);
    }
}

void PreprocessPage::setValidationMessage(const QString &message, const QString &kind)
{
    if (!m_validationMessage) {
        return;
    }
    m_validationMessage->setText(message);
    m_validationMessage->setProperty("kind", kind);
    refreshWidgetStyle(m_validationMessage);
}

void PreprocessPage::setRunState(const QString &message, const QString &state)
{
    if (!m_runStateLabel) {
        return;
    }
    m_runStateLabel->setText(message);
    m_runStateLabel->setProperty("state", state);
    refreshWidgetStyle(m_runStateLabel);
}

void PreprocessPage::appendLog(const QString &line)
{
    if (m_logOutput) {
        m_logOutput->appendPlainText(line);
    }
}

void PreprocessPage::updateCapSummary(const CapProbeResult &result)
{
    m_versionLabel->setText(result.version.isEmpty() ? QStringLiteral("-") : result.version);
    m_headerLengthLabel->setText(QStringLiteral("0x%1").arg(result.headerLength, 0, 16));
    m_bandwidthLabel->setText(QStringLiteral("%1 MHz").arg(result.bandwidthHz / 1000000.0, 0, 'f', 3));
    m_sampleRateLabel->setText(QStringLiteral("%1 MHz").arg(result.sampleRateHz / 1000000.0, 0, 'f', 3));
    m_centerFrequencyLabel->setText(QStringLiteral("%1 MHz").arg(result.centerFrequencyHz / 1000000.0, 0, 'f', 3));
    m_sampleCountLabel->setText(QString::number(result.frameSampleCount));
}

PreprocessRequest PreprocessPage::buildRequest(const QString &capPath) const
{
    PreprocessRequest request;
    request.inputFilePath = capPath;
    request.sampleOutputDir = m_outputDirEdit ? m_outputDirEdit->text().trimmed() : QString();
    request.modelWeightsPath = m_modelPathEdit ? m_modelPathEdit->text().trimmed() : QString();
    request.sliceLength = readInt(m_sliceLengthEdit, request.sliceLength);
    request.energyThresholdDb = readDouble(m_energyThresholdEdit, request.energyThresholdDb);
    request.noiseFloorDbm = readDouble(m_noiseFloorEdit, request.noiseFloorDbm);
    request.minBandwidthMhz = readDouble(m_minBandwidthEdit, request.minBandwidthMhz);
    request.minDurationMs = readDouble(m_minDurationEdit, request.minDurationMs);
    request.aiConfidenceThreshold = readDouble(m_aiConfidenceEdit, request.aiConfidenceThreshold);
    request.timeoutMs = 600000;
    return request;
}
