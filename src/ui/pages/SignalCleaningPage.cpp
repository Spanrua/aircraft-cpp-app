#include "ui/pages/SignalCleaningPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "ui/widgets/MetricCard.h"
#include "ui/widgets/SpectrogramReviewWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
QFrame *panel(const QString &objectName, const QString &title, QWidget *parent, QVBoxLayout **layoutOut)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(9);
    auto *titleLabel = new QLabel(title, frame);
    titleLabel->setProperty("sectionTitle", true);
    layout->addWidget(titleLabel);
    if (layoutOut) {
        *layoutOut = layout;
    }
    return frame;
}

QLineEdit *pathEdit(const QString &objectName, const QString &value, QWidget *parent)
{
    auto *edit = new QLineEdit(value, parent);
    edit->setObjectName(objectName);
    edit->setMinimumHeight(36);
    edit->setReadOnly(false);
    return edit;
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString latestPreprocessRoot()
{
    const QDir root(ProjectPaths::preprocessRunsDirectory());
    if (!root.exists()) {
        return {};
    }
    const QFileInfoList manifests = root.entryInfoList(
        {QStringLiteral("manifest.jsonl")},
        QDir::Files | QDir::Readable,
        QDir::Time);
    if (!manifests.isEmpty()) {
        return manifests.first().absolutePath();
    }

    QFileInfo newest;
    const QFileInfoList directories = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &directory : directories) {
        const QFileInfo candidate(QDir(directory.absoluteFilePath()).filePath(QStringLiteral("manifest.jsonl")));
        if (candidate.exists() && (!newest.exists() || candidate.lastModified() > newest.lastModified())) {
            newest = candidate;
        }
    }
    return newest.exists() ? newest.absolutePath() : QString();
}

QString probabilityText(double probability)
{
    return QStringLiteral("%1%").arg(probability * 100.0, 0, 'f', 2);
}

QString displayLabel(const QString &label)
{
    return label == QStringLiteral("signal")
        ? QStringLiteral("图传")
        : label == QStringLiteral("noise")
            ? QStringLiteral("纯噪声")
            : label;
}

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

bool samePrediction(const SampleRecord &sample, const SignalCleaningPrediction &prediction)
{
    if (!prediction.sampleId.isEmpty() && sample.sampleId == prediction.sampleId) {
        return true;
    }
    if (normalizedPath(sample.sampleFilePath) != normalizedPath(prediction.path)) {
        return false;
    }
    return sample.sampleIndex == prediction.index
        || sample.windowIndex == prediction.index
        || (sample.sampleIndex == 0 && sample.windowIndex == 0 && prediction.index == 0);
}
} // namespace

SignalCleaningPage::SignalCleaningPage(QWidget *parent)
    : WorkflowPage(
          QStringLiteral("信号清洗"),
          QStringLiteral("用单窗口图传/纯噪声模型对预处理结果进行自动模型判定；每个窗口独立判定，不依赖连续窗口或投票，人工复核仅在数据集页进行。"),
          parent,
          true)
{
    m_processRunner = new SignalCleaningProcessRunner(this);
    buildUi();
    loadDefaults();

    connect(m_processRunner, &SignalCleaningProcessRunner::started, this, [this]() {
        m_runStateLabel->setText(QStringLiteral("清洗中"));
        m_runStateLabel->setProperty("state", "running");
        m_runStateLabel->style()->unpolish(m_runStateLabel);
        m_runStateLabel->style()->polish(m_runStateLabel);
        appendLog(QStringLiteral("信号清洗进程已启动。"));
        emit workflowStateChanged();
    });
    connect(m_processRunner, &SignalCleaningProcessRunner::progressChanged,
            this, &SignalCleaningPage::updateProgress);
    connect(m_processRunner, &SignalCleaningProcessRunner::failed, this, [this](const QString &message) {
        setPageMessage(message, QStringLiteral("error"));
        appendLog(message);
        emit workflowStateChanged();
    });
    connect(m_processRunner, &SignalCleaningProcessRunner::finished,
            this, &SignalCleaningPage::showResult);
    connect(m_processRunner, &SignalCleaningProcessRunner::cancelled, this, [this]() {
        setBusy(false);
        m_runStateLabel->setText(QStringLiteral("已取消"));
        m_runStateLabel->setProperty("state", "warning");
        m_runStateLabel->style()->unpolish(m_runStateLabel);
        m_runStateLabel->style()->polish(m_runStateLabel);
        setPageMessage(QStringLiteral("清洗已停止，已保留已经生成的结果。"), QStringLiteral("warning"));
        emit workflowStateChanged();
    });
}

SignalCleaningPage::~SignalCleaningPage()
{
    if (m_processRunner && m_processRunner->isRunning()) {
        m_processRunner->cancel();
    }
}

bool SignalCleaningPage::hasResults() const
{
    return m_latestResult.success && !m_latestResult.cancelled;
}

bool SignalCleaningPage::isRunning() const
{
    return m_processRunner && m_processRunner->isRunning();
}

bool SignalCleaningPage::hasConfiguredModel() const
{
    return m_modelPathEdit
        && QFileInfo(m_modelPathEdit->text().trimmed()).isFile()
        && m_backendPathEdit
        && QFileInfo(m_backendPathEdit->text().trimmed()).isFile();
}

void SignalCleaningPage::setInputRootPath(const QString &path)
{
    if (path.trimmed().isEmpty() || !QFileInfo(path).exists()) {
        return;
    }
    const QString inputRoot = QFileInfo(path).absoluteFilePath();
    const bool inputChanged = normalizedPath(m_inputRootEdit->text()) != normalizedPath(inputRoot);
    if (!inputChanged) {
        return;
    }

    m_inputRootEdit->setText(inputRoot);
    m_latestResult = SignalCleaningResult();
    m_visiblePredictions.clear();
    m_predictionTable->setRowCount(0);
    m_spectrogramReviewWidget->clearImage();
    m_totalMetric->setValue(QStringLiteral("-"));
    m_signalMetric->setValue(QStringLiteral("-"));
    m_noiseMetric->setValue(QStringLiteral("-"));
    m_modelledMetric->setValue(QStringLiteral("-"));
    m_predictionDetailLabel->setText(QStringLiteral("预处理输出已变更，请重新清洗后选择窗口查看详情。"));
    m_nextDatasetButton->setEnabled(false);
    m_runStateLabel->setText(QStringLiteral("待清洗"));
    m_runStateLabel->setProperty("state", QVariant());
    m_runStateLabel->style()->unpolish(m_runStateLabel);
    m_runStateLabel->style()->polish(m_runStateLabel);
    setPageMessage(QStringLiteral("预处理输出已变更，请重新执行信号清洗。"), QStringLiteral("warning"));
    emit workflowStateChanged();
}

void SignalCleaningPage::setWorkflowExecutionAllowed(bool allowed, const QString &reason)
{
    m_workflowExecutionAllowed = allowed;
    m_workflowBlockingReason = reason.trimmed();
    if (m_workflowGateLabel) {
        const QString message = allowed
            ? QStringLiteral("执行已解锁：预处理样本已就绪。请确认模型、推理脚本和输入目录后开始清洗。")
            : QStringLiteral("执行已锁定：%1")
                  .arg(m_workflowBlockingReason.isEmpty()
                           ? QStringLiteral("请先完成预处理并生成样本。")
                           : m_workflowBlockingReason);
        m_workflowGateLabel->setText(message);
        m_workflowGateLabel->setProperty("kind", allowed ? "good" : "warning");
        m_workflowGateLabel->style()->unpolish(m_workflowGateLabel);
        m_workflowGateLabel->style()->polish(m_workflowGateLabel);
    }
    updateStartAvailability();
}

void SignalCleaningPage::showEvent(QShowEvent *event)
{
    WorkflowPage::showEvent(event);
    if (!m_defaultsLoaded || isRunning()) {
        return;
    }
    if (!QFileInfo(m_inputRootEdit->text().trimmed()).exists()) {
        const QString latest = latestPreprocessRoot();
        if (!latest.isEmpty()) {
            m_inputRootEdit->setText(latest);
        }
    }
}

void SignalCleaningPage::buildUi()
{
    QVBoxLayout *modelLayout = nullptr;
    auto *modelPanel = panel(QStringLiteral("SignalCleaningModelPanel"),
                             QStringLiteral("1. 选择单窗口模型"),
                             this,
                             &modelLayout);
    auto *modelGrid = new QGridLayout();
    modelGrid->setHorizontalSpacing(10);
    modelGrid->setVerticalSpacing(8);

    m_modelPathEdit = pathEdit(QStringLiteral("SignalCleaningModelPathEdit"),
                               SignalCleaningProcessRunner::modelPath(),
                               modelPanel);
    auto *modelBrowseButton = new QPushButton(QStringLiteral("选择模型"), modelPanel);
    modelBrowseButton->setObjectName(QStringLiteral("SignalCleaningBrowseModelButton"));
    modelBrowseButton->setProperty("secondary", true);
    auto *modelRow = new QHBoxLayout();
    modelRow->addWidget(m_modelPathEdit, 1);
    modelRow->addWidget(modelBrowseButton);
    modelGrid->addWidget(new QLabel(QStringLiteral("模型文件"), modelPanel), 0, 0);
    modelGrid->addLayout(modelRow, 0, 1);

    m_backendPathEdit = pathEdit(QStringLiteral("SignalCleaningBackendPathEdit"),
                                 SignalCleaningProcessRunner::backendScriptPath(),
                                 modelPanel);
    auto *backendBrowseButton = new QPushButton(QStringLiteral("选择脚本"), modelPanel);
    backendBrowseButton->setObjectName(QStringLiteral("SignalCleaningBrowseBackendButton"));
    backendBrowseButton->setProperty("secondary", true);
    auto *backendRow = new QHBoxLayout();
    backendRow->addWidget(m_backendPathEdit, 1);
    backendRow->addWidget(backendBrowseButton);
    modelGrid->addWidget(new QLabel(QStringLiteral("推理脚本"), modelPanel), 1, 0);
    modelGrid->addLayout(backendRow, 1, 1);

    m_modelSummaryLabel = new QLabel(modelPanel);
    m_modelSummaryLabel->setObjectName(QStringLiteral("SignalCleaningModelSummary"));
    m_modelSummaryLabel->setWordWrap(true);
    m_modelSummaryLabel->setProperty("muted", true);
    modelGrid->addWidget(m_modelSummaryLabel, 2, 0, 1, 2);
    modelLayout->addLayout(modelGrid);
    contentLayout()->addWidget(modelPanel);

    QVBoxLayout *inputLayout = nullptr;
    auto *inputPanel = panel(QStringLiteral("SignalCleaningInputPanel"),
                             QStringLiteral("2. 选择清洗输入"),
                             this,
                             &inputLayout);
    auto *inputRow = new QHBoxLayout();
    m_inputRootEdit = pathEdit(QStringLiteral("SignalCleaningInputRootEdit"),
                               latestPreprocessRoot(),
                               inputPanel);
    auto *inputBrowseButton = new QPushButton(QStringLiteral("选择目录"), inputPanel);
    inputBrowseButton->setObjectName(QStringLiteral("SignalCleaningBrowseInputButton"));
    inputBrowseButton->setProperty("secondary", true);
    inputRow->addWidget(m_inputRootEdit, 1);
    inputRow->addWidget(inputBrowseButton);
    inputLayout->addWidget(new QLabel(QStringLiteral("预处理样本目录或 manifest.jsonl 所在目录"), inputPanel));
    inputLayout->addLayout(inputRow);

    auto *parameterRow = new QHBoxLayout();
    m_thresholdEdit = pathEdit(QStringLiteral("SignalCleaningThresholdEdit"), QStringLiteral("0.50"), inputPanel);
    m_thresholdEdit->setMaximumWidth(120);
    m_batchSizeEdit = pathEdit(QStringLiteral("SignalCleaningBatchSizeEdit"), QStringLiteral("64"), inputPanel);
    m_batchSizeEdit->setMaximumWidth(120);
    m_cpuCheck = new QCheckBox(QStringLiteral("使用 CPU"), inputPanel);
    m_cpuCheck->setObjectName(QStringLiteral("SignalCleaningCpuCheck"));
    parameterRow->addWidget(new QLabel(QStringLiteral("图传阈值"), inputPanel));
    parameterRow->addWidget(m_thresholdEdit);
    parameterRow->addSpacing(12);
    parameterRow->addWidget(new QLabel(QStringLiteral("批大小"), inputPanel));
    parameterRow->addWidget(m_batchSizeEdit);
    parameterRow->addSpacing(12);
    parameterRow->addWidget(m_cpuCheck);
    parameterRow->addStretch(1);
    inputLayout->addLayout(parameterRow);

    auto *outputRow = new QHBoxLayout();
    m_outputRootEdit = pathEdit(QStringLiteral("SignalCleaningOutputRootEdit"),
                                ProjectPaths::signalCleaningRunsDirectory(),
                                inputPanel);
    m_outputRootEdit->setReadOnly(true);
    outputRow->addWidget(new QLabel(QStringLiteral("结果目录"), inputPanel));
    outputRow->addWidget(m_outputRootEdit, 1);
    inputLayout->addLayout(outputRow);

    auto *actionRow = new QHBoxLayout();
    m_startButton = new QPushButton(QStringLiteral("开始清洗"), inputPanel);
    m_startButton->setObjectName(QStringLiteral("StartSignalCleaningButton"));
    m_startButton->setProperty("primary", true);
    m_cancelButton = new QPushButton(QStringLiteral("停止"), inputPanel);
    m_cancelButton->setObjectName(QStringLiteral("CancelSignalCleaningButton"));
    m_cancelButton->setProperty("secondary", true);
    m_cancelButton->setEnabled(false);
    auto *openButton = new QPushButton(QStringLiteral("打开结果目录"), inputPanel);
    openButton->setObjectName(QStringLiteral("OpenSignalCleaningOutputButton"));
    openButton->setProperty("secondary", true);
    m_nextDatasetButton = new QPushButton(QStringLiteral("去数据集人工复核"), inputPanel);
    m_nextDatasetButton->setObjectName(QStringLiteral("GoToDatasetAfterCleaningButton"));
    m_nextDatasetButton->setProperty("secondary", true);
    m_nextDatasetButton->setEnabled(false);
    m_runStateLabel = new QLabel(QStringLiteral("待执行"), inputPanel);
    m_runStateLabel->setObjectName(QStringLiteral("SignalCleaningRunStateLabel"));
    actionRow->addWidget(m_startButton);
    actionRow->addWidget(m_cancelButton);
    actionRow->addWidget(openButton);
    actionRow->addStretch(1);
    actionRow->addWidget(m_runStateLabel);
    actionRow->addWidget(m_nextDatasetButton);
    inputLayout->addLayout(actionRow);
    contentLayout()->addWidget(inputPanel);

    m_workflowGateLabel = new QLabel(this);
    m_workflowGateLabel->setObjectName(QStringLiteral("SignalCleaningWorkflowGateLabel"));
    m_workflowGateLabel->setWordWrap(true);
    m_workflowGateLabel->setProperty("kind", "warning");
    contentLayout()->addWidget(m_workflowGateLabel);

    auto *metricsContainer = new QWidget(this);
    auto *metrics = new QHBoxLayout(metricsContainer);
    metrics->setContentsMargins(0, 0, 0, 0);
    m_totalMetric = new MetricCard(QStringLiteral("总窗口"), metricsContainer);
    m_signalMetric = new MetricCard(QStringLiteral("图传候选"), metricsContainer);
    m_noiseMetric = new MetricCard(QStringLiteral("纯噪声"), metricsContainer);
    m_modelledMetric = new MetricCard(QStringLiteral("模型覆盖"), metricsContainer);
    metrics->addWidget(m_totalMetric);
    metrics->addWidget(m_signalMetric);
    metrics->addWidget(m_noiseMetric);
    metrics->addWidget(m_modelledMetric);
    contentLayout()->addWidget(metricsContainer);

    auto *progressPanel = new QFrame(this);
    progressPanel->setObjectName(QStringLiteral("SignalCleaningProgressPanel"));
    auto *progressLayout = new QVBoxLayout(progressPanel);
    progressLayout->setContentsMargins(14, 11, 14, 11);
    m_progressDetailLabel = new QLabel(QStringLiteral("等待开始。"), progressPanel);
    m_progressDetailLabel->setObjectName(QStringLiteral("SignalCleaningProgressDetail"));
    m_progressBar = new QProgressBar(progressPanel);
    m_progressBar->setObjectName(QStringLiteral("SignalCleaningProgressBar"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    progressLayout->addWidget(m_progressDetailLabel);
    progressLayout->addWidget(m_progressBar);
    contentLayout()->addWidget(progressPanel);

    auto *resultsPanel = new QFrame(this);
    resultsPanel->setObjectName(QStringLiteral("SignalCleaningResultsPanel"));
    auto *resultsLayout = new QVBoxLayout(resultsPanel);
    resultsLayout->setContentsMargins(14, 12, 14, 12);
    auto *resultsHeader = new QHBoxLayout();
    auto *resultsTitle = new QLabel(QStringLiteral("窗口级模型判定（自动）"), resultsPanel);
    resultsTitle->setProperty("sectionTitle", true);
    resultsHeader->addWidget(resultsTitle);
    resultsHeader->addStretch(1);
    m_predictionFilter = new QComboBox(resultsPanel);
    m_predictionFilter->setObjectName(QStringLiteral("SignalCleaningPredictionFilter"));
    m_predictionFilter->addItem(QStringLiteral("全部窗口"), QStringLiteral("all"));
    m_predictionFilter->addItem(QStringLiteral("图传候选"), QStringLiteral("signal"));
    m_predictionFilter->addItem(QStringLiteral("纯噪声"), QStringLiteral("noise"));
    resultsHeader->addWidget(m_predictionFilter);
    resultsLayout->addLayout(resultsHeader);

    m_predictionTable = new QTableWidget(0, 6, resultsPanel);
    m_predictionTable->setObjectName(QStringLiteral("SignalCleaningPredictionTable"));
    m_predictionTable->setHorizontalHeaderLabels({
        QStringLiteral("模型建议"),
        QStringLiteral("图传概率"),
        QStringLiteral("窗口序号"),
        QStringLiteral("样本 ID"),
        QStringLiteral("样本文件"),
        QStringLiteral("来源组"),
    });
    m_predictionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_predictionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_predictionTable->setAlternatingRowColors(true);
    m_predictionTable->verticalHeader()->setVisible(false);
    m_predictionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_predictionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_predictionTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_predictionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_predictionTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_predictionTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_predictionTable->setMinimumHeight(280);
    resultsLayout->addWidget(m_predictionTable);

    m_predictionDetailLabel = new QLabel(QStringLiteral("选择窗口后显示概率、来源和已有时频图。"), resultsPanel);
    m_predictionDetailLabel->setObjectName(QStringLiteral("SignalCleaningPredictionDetail"));
    m_predictionDetailLabel->setWordWrap(true);
    m_predictionDetailLabel->setProperty("muted", true);
    resultsLayout->addWidget(m_predictionDetailLabel);
    m_spectrogramReviewWidget = new SpectrogramReviewWidget(resultsPanel);
    m_spectrogramReviewWidget->setObjectName(QStringLiteral("SignalCleaningSpectrogramWidget"));
    m_spectrogramReviewWidget->setMinimumHeight(220);
    resultsLayout->addWidget(m_spectrogramReviewWidget);
    contentLayout()->addWidget(resultsPanel);

    m_pageMessage = new QLabel(this);
    m_pageMessage->setObjectName(QStringLiteral("SignalCleaningPageMessage"));
    m_pageMessage->setWordWrap(true);
    m_pageMessage->setVisible(false);
    contentLayout()->addWidget(m_pageMessage);

    m_logOutput = new QPlainTextEdit(this);
    m_logOutput->setObjectName(QStringLiteral("SignalCleaningLog"));
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumBlockCount(500);
    m_logOutput->setPlaceholderText(QStringLiteral("清洗日志"));
    contentLayout()->addWidget(m_logOutput);

    connect(modelBrowseButton, &QPushButton::clicked, this, &SignalCleaningPage::browseModel);
    connect(backendBrowseButton, &QPushButton::clicked, this, &SignalCleaningPage::browseBackend);
    connect(inputBrowseButton, &QPushButton::clicked, this, &SignalCleaningPage::browseInputRoot);
    connect(m_startButton, &QPushButton::clicked, this, &SignalCleaningPage::startCleaning);
    connect(m_cancelButton, &QPushButton::clicked, this, &SignalCleaningPage::cancelCleaning);
    connect(openButton, &QPushButton::clicked, this, &SignalCleaningPage::openOutputDirectory);
    connect(m_nextDatasetButton, &QPushButton::clicked, this, &SignalCleaningPage::requestOpenDataset);
    connect(m_predictionFilter, &QComboBox::currentTextChanged, this,
            &SignalCleaningPage::rebuildPredictionTable);
    connect(m_predictionTable, &QTableWidget::itemSelectionChanged, this,
            &SignalCleaningPage::updateSelectedPrediction);
    const auto updateAvailability = [this]() { updateStartAvailability(); };
    connect(m_modelPathEdit, &QLineEdit::textChanged, this, updateAvailability);
    connect(m_backendPathEdit, &QLineEdit::textChanged, this, updateAvailability);
    connect(m_inputRootEdit, &QLineEdit::textChanged, this, updateAvailability);
    m_workflowGateLabel->setText(QStringLiteral("执行已锁定：请先完成预处理并生成样本。"));
    updateStartAvailability();
}

void SignalCleaningPage::loadDefaults()
{
    m_defaultsLoaded = true;
    loadModelMetadata();
    if (!QFileInfo(m_inputRootEdit->text().trimmed()).exists()) {
        m_inputRootEdit->setText(latestPreprocessRoot());
    }
}

void SignalCleaningPage::loadModelMetadata()
{
    const QString metadataPath = QFileInfo(m_modelPathEdit->text()).dir().filePath(
        QStringLiteral("training_metadata.json"));
    QFile file(metadataPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_modelSummaryLabel->setText(QStringLiteral("未找到训练元数据；模型仍可手动运行，默认阈值为 0.50。"));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject json = document.object();
    const double threshold = json.value(QStringLiteral("threshold")).toDouble(0.5);
    m_thresholdEdit->setText(QString::number(threshold, 'f', 2));
    const QString feature = json.value(QStringLiteral("feature")).toString(QStringLiteral("log_power_stft_v1"));
    const int trainRecords = json.value(QStringLiteral("train_records")).toInt();
    const int valRecords = json.value(QStringLiteral("val_records")).toInt();
    m_modelSummaryLabel->setText(
        QStringLiteral("特征：%1；输入：单窗口 [2, 4096]；默认阈值：%2；训练窗口：%3；验证窗口：%4。")
            .arg(feature)
            .arg(threshold, 0, 'f', 2)
            .arg(trainRecords)
            .arg(valRecords));
}

void SignalCleaningPage::browseModel()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择图传/纯噪声模型"),
        QFileInfo(m_modelPathEdit->text()).absolutePath(),
        QStringLiteral("PyTorch 模型 (*.pt *.pth);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    m_modelPathEdit->setText(path);
    loadModelMetadata();
}

void SignalCleaningPage::browseBackend()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择图传/纯噪声推理脚本"),
        QFileInfo(m_backendPathEdit->text()).absolutePath(),
        QStringLiteral("Python 脚本 (*.py *.pyw);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_backendPathEdit->setText(path);
    }
}

void SignalCleaningPage::browseInputRoot()
{
    const QString path = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择预处理样本目录"),
        m_inputRootEdit->text().trimmed().isEmpty()
            ? ProjectPaths::preprocessRunsDirectory()
            : m_inputRootEdit->text());
    if (!path.isEmpty()) {
        m_inputRootEdit->setText(path);
    }
}

void SignalCleaningPage::startCleaning()
{
    if (isRunning() || m_busy) {
        return;
    }
    if (!m_workflowExecutionAllowed) {
        setPageMessage(QStringLiteral("执行已锁定：%1")
                           .arg(m_workflowBlockingReason.isEmpty()
                                    ? QStringLiteral("请先完成预处理并生成样本。")
                                    : m_workflowBlockingReason),
                       QStringLiteral("warning"));
        return;
    }
    bool thresholdOk = false;
    const double threshold = m_thresholdEdit->text().trimmed().toDouble(&thresholdOk);
    if (!thresholdOk || threshold < 0.0 || threshold > 1.0) {
        setPageMessage(QStringLiteral("图传阈值必须是 0 到 1 之间的数值。"), QStringLiteral("error"));
        return;
    }
    bool batchOk = false;
    const int batchSize = m_batchSizeEdit->text().trimmed().toInt(&batchOk);
    if (!batchOk || batchSize < 1 || batchSize > 4096) {
        setPageMessage(QStringLiteral("批大小必须是 1 到 4096 之间的整数。"), QStringLiteral("error"));
        return;
    }
    const QString modelPath = m_modelPathEdit->text().trimmed();
    const QString backendPath = m_backendPathEdit->text().trimmed();
    const QString inputRoot = m_inputRootEdit->text().trimmed();
    if (!QFileInfo(modelPath).isFile() || !QFileInfo(backendPath).isFile()) {
        setPageMessage(QStringLiteral("请先确认模型文件和推理脚本路径。"), QStringLiteral("error"));
        return;
    }
    if (!QFileInfo(inputRoot).exists()) {
        setPageMessage(QStringLiteral("请先选择预处理输出目录或 manifest.jsonl 所在目录。"), QStringLiteral("error"));
        return;
    }

    const QString runId = QStringLiteral("clean_%1")
                              .arg(QDateTime::currentDateTime().toString(
                                  QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString outputDirectory = QDir(ProjectPaths::signalCleaningRunsDirectory()).filePath(runId);
    SignalCleaningRequest request;
    request.modelPath = modelPath;
    request.backendScriptPath = backendPath;
    request.inputRoot = inputRoot;
    request.outputDirectory = outputDirectory;
    request.runId = runId;
    request.threshold = threshold;
    request.batchSize = batchSize;
    request.useCpu = m_cpuCheck->isChecked();

    m_latestResult = SignalCleaningResult();
    m_predictionTable->setRowCount(0);
    m_spectrogramReviewWidget->clearImage();
    m_predictionDetailLabel->setText(QStringLiteral("清洗完成后选择窗口查看详情。"));
    m_logOutput->clear();
    setPageMessage(QString(), QString());
    setBusy(true);
    if (!m_processRunner->start(request)) {
        setBusy(false);
    }
}

void SignalCleaningPage::cancelCleaning()
{
    if (m_processRunner) {
        m_processRunner->cancel();
    }
}

void SignalCleaningPage::openOutputDirectory()
{
    QString path = m_latestResult.outputDirectory;
    if (path.isEmpty()) {
        path = m_outputRootEdit->text().trimmed();
    }
    if (!QDir().mkpath(path)) {
        setPageMessage(QStringLiteral("无法打开结果目录：%1").arg(path), QStringLiteral("error"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void SignalCleaningPage::updateProgress(int percent, const QString &message)
{
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(qBound(0, percent, 100));
    m_progressDetailLabel->setText(message);
}

void SignalCleaningPage::showResult(const SignalCleaningResult &result)
{
    m_latestResult = result;
    setBusy(false);
    if (!result.success) {
        if (!result.cancelled) {
            setPageMessage(result.message, QStringLiteral("error"));
        }
        appendLog(result.message);
        emit workflowStateChanged();
        return;
    }
    savePredictionsToState(result);
    m_totalMetric->setValue(QString::number(result.totalRecords));
    m_signalMetric->setValue(QString::number(result.signalRecords));
    m_noiseMetric->setValue(QString::number(result.noiseRecords));
    m_modelledMetric->setValue(result.totalRecords > 0
                                   ? QStringLiteral("%1%").arg(
                                         100.0 * result.predictions.size() / result.totalRecords,
                                         0,
                                         'f',
                                         1)
                                   : QStringLiteral("0%"));
    rebuildPredictionTable();
    setPageMessage(result.message, QStringLiteral("good"));
    appendLog(result.message);
    for (const QString &line : result.logs) {
        appendLog(line);
    }
    m_nextDatasetButton->setEnabled(true);
    emit workflowStateChanged();
}

void SignalCleaningPage::rebuildPredictionTable()
{
    if (!m_predictionTable) {
        return;
    }
    const QString filter = m_predictionFilter->currentData().toString();
    m_visiblePredictions.clear();
    for (const SignalCleaningPrediction &prediction : m_latestResult.predictions) {
        if (filter != QStringLiteral("all") && prediction.label != filter) {
            continue;
        }
        m_visiblePredictions.append(prediction);
    }
    const QSignalBlocker blocker(m_predictionTable);
    m_predictionTable->setRowCount(0);
    for (const SignalCleaningPrediction &prediction : m_visiblePredictions) {
        const int row = m_predictionTable->rowCount();
        m_predictionTable->insertRow(row);
        auto *labelItem = readOnlyItem(displayLabel(prediction.label));
        labelItem->setData(Qt::UserRole, row);
        labelItem->setToolTip(prediction.label);
        m_predictionTable->setItem(row, 0, labelItem);
        m_predictionTable->setItem(row, 1, readOnlyItem(probabilityText(prediction.probability)));
        m_predictionTable->setItem(row, 2, readOnlyItem(QString::number(prediction.index)));
        m_predictionTable->setItem(row, 3, readOnlyItem(prediction.sampleId));
        auto *pathItem = readOnlyItem(QFileInfo(prediction.path).fileName());
        pathItem->setToolTip(prediction.path);
        m_predictionTable->setItem(row, 4, pathItem);
        m_predictionTable->setItem(row, 5, readOnlyItem(prediction.group));
    }
    if (m_predictionTable->rowCount() > 0) {
        m_predictionTable->selectRow(0);
    } else {
        updateSelectedPrediction();
    }
}

void SignalCleaningPage::updateSelectedPrediction()
{
    const int row = m_predictionTable->currentRow();
    if (row < 0 || row >= m_visiblePredictions.size()) {
        m_predictionDetailLabel->setText(QStringLiteral("选择窗口后显示概率、来源和已有时频图。"));
        m_spectrogramReviewWidget->clearImage();
        return;
    }
    const SignalCleaningPrediction &prediction = m_visiblePredictions.at(row);
    QString detail = QStringLiteral("模型建议：%1\n图传概率：%2\n窗口序号：%3\n样本 ID：%4\n样本文件：%5")
                         .arg(displayLabel(prediction.label))
                         .arg(probabilityText(prediction.probability))
                         .arg(prediction.index)
                         .arg(prediction.sampleId)
                         .arg(QDir::toNativeSeparators(prediction.path));

    AppStateStore store;
    const auto state = store.load();
    QString previewPath;
    if (state.has_value()) {
        for (const SampleRecord &sample : state->samples) {
            if (samePrediction(sample, prediction)) {
                previewPath = sample.candidatePreviewFilePath.isEmpty()
                    ? sample.spectrogramFilePath
                    : sample.candidatePreviewFilePath;
                detail.append(QStringLiteral("\n中心频率：%1 Hz\n带宽：%2 Hz\nSNR：%3 dB")
                                  .arg(sample.centerFrequencyHz, 0, 'f', 0)
                                  .arg(sample.bandwidthHz, 0, 'f', 0)
                                  .arg(sample.snrDb, 0, 'f', 2));
                break;
            }
        }
    }
    m_predictionDetailLabel->setText(detail);
    if (previewPath.isEmpty() || !QFileInfo::exists(previewPath)) {
        m_spectrogramReviewWidget->clearImage(QStringLiteral("当前窗口没有可用的时频图。"));
    } else {
        m_spectrogramReviewWidget->setImagePath(previewPath);
    }
}

void SignalCleaningPage::savePredictionsToState(const SignalCleaningResult &result)
{
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    int matched = 0;
    for (const SignalCleaningPrediction &prediction : result.predictions) {
        for (SampleRecord &sample : state.samples) {
            if (!samePrediction(sample, prediction)) {
                continue;
            }
            sample.signalNoiseProbability = prediction.probability;
            sample.signalNoiseThreshold = result.threshold;
            sample.signalNoiseLabel = prediction.label;
            sample.signalNoiseModelPath = result.modelPath;
            sample.signalNoiseRunId = result.runId;
            ++matched;
            break;
        }
    }
    if (!store.save(state)) {
        appendLog(QStringLiteral("图传模型判定已生成，但样本状态保存失败：%1").arg(store.lastError()));
    } else {
        appendLog(QStringLiteral("已把 %1 / %2 个窗口判定写入样本状态，未修改人工复核状态。")
                      .arg(matched)
                      .arg(result.predictions.size()));
    }
}

void SignalCleaningPage::setBusy(bool busy)
{
    m_busy = busy;
    updateStartAvailability();
    m_cancelButton->setEnabled(busy);
    m_modelPathEdit->setEnabled(!busy);
    m_backendPathEdit->setEnabled(!busy);
    m_inputRootEdit->setEnabled(!busy);
    m_thresholdEdit->setEnabled(!busy);
    m_batchSizeEdit->setEnabled(!busy);
    m_cpuCheck->setEnabled(!busy);
    if (busy) {
        m_progressBar->setRange(0, 0);
        m_progressDetailLabel->setText(QStringLiteral("正在进行单窗口推理，请稍候。"));
    } else if (m_progressBar->maximum() == 0) {
        m_progressBar->setRange(0, 100);
    }
}

void SignalCleaningPage::updateStartAvailability()
{
    if (!m_startButton) {
        return;
    }
    const bool hasInputRoot = m_inputRootEdit && QFileInfo(m_inputRootEdit->text().trimmed()).exists();
    m_startButton->setEnabled(!m_busy && m_workflowExecutionAllowed && hasConfiguredModel() && hasInputRoot);
}

void SignalCleaningPage::setPageMessage(const QString &message, const QString &kind)
{
    m_pageMessage->setText(message);
    m_pageMessage->setProperty("kind", kind);
    m_pageMessage->setVisible(!message.isEmpty());
    m_pageMessage->style()->unpolish(m_pageMessage);
    m_pageMessage->style()->polish(m_pageMessage);
}

void SignalCleaningPage::appendLog(const QString &message)
{
    if (m_logOutput && !message.trimmed().isEmpty()) {
        m_logOutput->appendPlainText(message.trimmed());
    }
}
