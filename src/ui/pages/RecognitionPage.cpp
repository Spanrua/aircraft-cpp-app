#include "ui/pages/RecognitionPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "ui/widgets/MetricCard.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString shortPath(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? info.fileName() : path;
}
} // namespace

RecognitionPage::RecognitionPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("无人机识别"),
                   QStringLiteral("选择已训练模型和样本，使用当前 GPU 环境输出识别结果。"),
                   parent,
                   true)
{
    auto *context = new QLabel(QStringLiteral("识别只读取模型和样本，不会修改原始采集文件或数据集版本。"), this);
    context->setObjectName(QStringLiteral("RecognitionContext"));
    context->setProperty("muted", true);
    context->setWordWrap(true);
    contentLayout()->addWidget(context);

    auto *metrics = new QHBoxLayout();
    m_modelMetric = new MetricCard(QStringLiteral("模型状态"), this);
    m_confidenceMetric = new MetricCard(QStringLiteral("最高置信度"), this);
    m_resultMetric = new MetricCard(QStringLiteral("识别结果"), this);
    m_modelMetric->setObjectName(QStringLiteral("RecognitionModelMetric"));
    m_confidenceMetric->setObjectName(QStringLiteral("RecognitionConfidenceMetric"));
    m_resultMetric->setObjectName(QStringLiteral("RecognitionResultMetric"));
    m_modelMetric->setValue(QStringLiteral("未加载"));
    m_confidenceMetric->setValue(QStringLiteral("-"));
    m_resultMetric->setValue(QStringLiteral("待识别"));
    metrics->addWidget(m_modelMetric);
    metrics->addWidget(m_confidenceMetric);
    metrics->addWidget(m_resultMetric);
    contentLayout()->addLayout(metrics);

    auto *modelPanel = new QFrame(this);
    modelPanel->setObjectName(QStringLiteral("RecognitionModelPanel"));
    auto *modelLayout = new QFormLayout(modelPanel);
    modelLayout->setContentsMargins(14, 12, 14, 12);
    modelLayout->setSpacing(8);
    m_modelCombo = new QComboBox(modelPanel);
    m_modelCombo->setObjectName(QStringLiteral("RecognitionModelCombo"));
    modelLayout->addRow(QStringLiteral("当前模型"), m_modelCombo);
    m_modelSummary = new QLabel(QStringLiteral("尚未找到可用的训练模型。"), modelPanel);
    m_modelSummary->setObjectName(QStringLiteral("RecognitionModelSummary"));
    m_modelSummary->setWordWrap(true);
    modelLayout->addRow(QStringLiteral("模型信息"), m_modelSummary);
    contentLayout()->addWidget(modelPanel);

    auto *samplePanel = new QFrame(this);
    samplePanel->setObjectName(QStringLiteral("RecognitionSamplePanel"));
    auto *sampleLayout = new QFormLayout(samplePanel);
    sampleLayout->setContentsMargins(14, 12, 14, 12);
    sampleLayout->setSpacing(8);
    m_sampleCombo = new QComboBox(samplePanel);
    m_sampleCombo->setObjectName(QStringLiteral("RecognitionSampleCombo"));
    sampleLayout->addRow(QStringLiteral("数据集样本"), m_sampleCombo);
    auto *samplePathRow = new QHBoxLayout();
    m_samplePathEdit = new QLineEdit(samplePanel);
    m_samplePathEdit->setObjectName(QStringLiteral("RecognitionSamplePathEdit"));
    m_samplePathEdit->setReadOnly(true);
    auto *chooseButton = new QPushButton(QStringLiteral("选择 NPZ"), samplePanel);
    chooseButton->setObjectName(QStringLiteral("RecognitionChooseSampleButton"));
    chooseButton->setProperty("secondary", true);
    samplePathRow->addWidget(m_samplePathEdit, 1);
    samplePathRow->addWidget(chooseButton);
    sampleLayout->addRow(QStringLiteral("样本文件"), samplePathRow);
    m_sampleSummary = new QLabel(QStringLiteral("请选择一个包含 iq 数组的 NPZ 样本。"), samplePanel);
    m_sampleSummary->setObjectName(QStringLiteral("RecognitionSampleSummary"));
    m_sampleSummary->setWordWrap(true);
    sampleLayout->addRow(QStringLiteral("样本信息"), m_sampleSummary);
    contentLayout()->addWidget(samplePanel);

    auto *actions = new QHBoxLayout();
    auto *refreshButton = new QPushButton(QStringLiteral("刷新模型和样本"), this);
    refreshButton->setObjectName(QStringLiteral("RecognitionRefreshButton"));
    refreshButton->setProperty("secondary", true);
    m_runButton = new QPushButton(QStringLiteral("执行识别"), this);
    m_runButton->setObjectName(QStringLiteral("RecognitionRunButton"));
    m_runButton->setProperty("primary", true);
    m_cancelButton = new QPushButton(QStringLiteral("取消识别"), this);
    m_cancelButton->setObjectName(QStringLiteral("RecognitionCancelButton"));
    m_cancelButton->setProperty("secondary", true);
    m_cancelButton->setEnabled(false);
    actions->addWidget(refreshButton);
    actions->addWidget(m_runButton);
    actions->addWidget(m_cancelButton);
    actions->addStretch(1);
    contentLayout()->addLayout(actions);

    m_resultDetail = new QLabel(QStringLiteral("等待识别。"), this);
    m_resultDetail->setObjectName(QStringLiteral("RecognitionResultDetail"));
    m_resultDetail->setWordWrap(true);
    contentLayout()->addWidget(m_resultDetail);
    m_topCandidates = new QLabel(QStringLiteral("-"), this);
    m_topCandidates->setObjectName(QStringLiteral("RecognitionTopCandidates"));
    m_topCandidates->setWordWrap(true);
    contentLayout()->addWidget(m_topCandidates);

    m_resultTable = new QTableWidget(0, 5, this);
    m_resultTable->setObjectName(QStringLiteral("RecognitionResultTable"));
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setHorizontalHeaderLabels({
        QStringLiteral("样本"),
        QStringLiteral("预测类型"),
        QStringLiteral("个体"),
        QStringLiteral("置信度"),
        QStringLiteral("设备"),
    });
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < m_resultTable->columnCount(); ++column) {
        m_resultTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_resultTable->verticalHeader()->setVisible(false);
    contentLayout()->addWidget(m_resultTable, 1);

    m_runner = new RecognitionProcessRunner(this);
    connect(refreshButton, &QPushButton::clicked, this, [this]() {
        loadModels();
        loadSamples();
    });
    connect(m_modelCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { selectModelFromCombo(); });
    connect(m_sampleCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { selectSampleFromCombo(); });
    connect(chooseButton, &QPushButton::clicked, this, &RecognitionPage::chooseSample);
    connect(m_runButton, &QPushButton::clicked, this, &RecognitionPage::runRecognition);
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        if (m_runner) {
            m_runner->cancel();
        }
    });
    connect(m_runner, &RecognitionProcessRunner::finished,
            this, &RecognitionPage::handleRecognitionFinished);
    connect(m_runner, &RecognitionProcessRunner::failed,
            this, &RecognitionPage::handleRecognitionFailed);
    loadModels();
    loadSamples();
    updateRunAvailability();
}

bool RecognitionPage::hasUsableModel() const
{
    if (m_selectedModelPath.isEmpty() || !QFileInfo::exists(m_selectedModelPath)) {
        return false;
    }
    return m_modelCombo && m_modelCombo->count() > 0;
}

void RecognitionPage::showEvent(QShowEvent *event)
{
    WorkflowPage::showEvent(event);
    loadModels();
    loadSamples();
}

void RecognitionPage::setModelForRecognition(const QString &modelPath, const QString &datasetVersionId)
{
    if (modelPath.trimmed().isEmpty()) {
        return;
    }
    m_selectedModelPath = modelPath;
    int matchingIndex = -1;
    for (int index = 0; index < m_trainingRuns.size(); ++index) {
        if (m_trainingRuns.at(index).modelMode != QStringLiteral("closed_set")) {
            continue;
        }
        if (m_trainingRuns.at(index).bestModelPath == modelPath
            || (!datasetVersionId.isEmpty()
                && m_trainingRuns.at(index).datasetVersionId == datasetVersionId
                && m_trainingRuns.at(index).modelAvailable)) {
            matchingIndex = index;
            break;
        }
    }
    if (matchingIndex >= 0 && m_modelCombo) {
        m_modelCombo->setCurrentIndex(matchingIndex);
    } else if (m_modelSummary) {
        m_modelSummary->setText(QStringLiteral("模型：%1\n数据集版本：%2")
                                    .arg(QDir::toNativeSeparators(modelPath),
                                         datasetVersionId.isEmpty() ? QStringLiteral("未知") : datasetVersionId));
        m_modelMetric->setValue(QStringLiteral("已加载"));
    }
}

void RecognitionPage::loadModels()
{
    AppStateStore store;
    const auto state = store.load();
    m_trainingRuns = state.has_value() ? state->trainingRuns : QVector<TrainingRunRecord>();
    m_modelCombo->clear();
    m_selectedRunIndex = -1;
    m_selectedModelPath.clear();
    int activeIndex = -1;
    const QString activeRunId = state.has_value() ? state->activeTrainingRunId : QString();
    for (int index = 0; index < m_trainingRuns.size(); ++index) {
        const TrainingRunRecord &run = m_trainingRuns.at(index);
        if (run.bestModelPath.isEmpty() || !QFileInfo::exists(run.bestModelPath)
            || run.modelMode != QStringLiteral("closed_set")
            || run.state != QStringLiteral("已完成")
            || !run.modelAvailable) {
            continue;
        }
        const QString label = QStringLiteral("%1 · %2 · %3")
                                  .arg(run.runId, run.datasetVersionId, run.state);
        m_modelCombo->addItem(label, index);
        if (run.runId == activeRunId) {
            activeIndex = m_modelCombo->count() - 1;
        }
    }
    if (m_modelCombo->count() > 0) {
        m_modelCombo->setCurrentIndex(activeIndex >= 0 ? activeIndex : 0);
        selectModelFromCombo();
    } else {
        m_modelMetric->setValue(QStringLiteral("未加载"));
        m_modelSummary->setText(QStringLiteral("暂无可用模型。请先完成一次 GPU 训练并生成最佳模型。"));
    }
    updateRunAvailability();
}

void RecognitionPage::loadSamples()
{
    AppStateStore store;
    const auto state = store.load();
    m_samples = state.has_value() ? state->samples : QVector<SampleRecord>();
    m_sampleCombo->clear();
    m_selectedSampleIndex = -1;
    for (int index = 0; index < m_samples.size(); ++index) {
        const SampleRecord &sample = m_samples.at(index);
        if (sample.sampleFilePath.isEmpty() || !QFileInfo::exists(sample.sampleFilePath)) {
            continue;
        }
        m_sampleCombo->addItem(QStringLiteral("%1 · %2")
                                   .arg(sample.sampleId, sample.labelType.isEmpty()
                                           ? QStringLiteral("未标注")
                                           : sample.labelType),
                               index);
    }
    if (m_sampleCombo->count() > 0) {
        m_sampleCombo->setCurrentIndex(0);
        selectSampleFromCombo();
    } else {
        m_sampleSummary->setText(QStringLiteral("暂无可用 NPZ 样本。请先在预处理页生成样本。"));
    }
    updateRunAvailability();
}

void RecognitionPage::selectModelFromCombo()
{
    const int comboIndex = m_modelCombo ? m_modelCombo->currentIndex() : -1;
    if (comboIndex < 0) {
        return;
    }
    const int index = m_modelCombo->itemData(comboIndex).toInt();
    if (index < 0 || index >= m_trainingRuns.size()) {
        return;
    }
    m_selectedRunIndex = index;
    const TrainingRunRecord &run = m_trainingRuns.at(index);
    m_selectedModelPath = run.bestModelPath;
    m_modelMetric->setValue(QStringLiteral("已加载"));
    m_modelSummary->setText(QStringLiteral("训练任务：%1\n数据集版本：%2\n验证准确率：%3%\n设备：%4\n模型：%5")
                                .arg(run.runId, run.datasetVersionId)
                                .arg(run.bestValAccuracy, 0, 'f', 2)
                                .arg(run.gpuDeviceName.isEmpty() ? QStringLiteral("CUDA GPU") : run.gpuDeviceName,
                                     QDir::toNativeSeparators(run.bestModelPath)));
}

void RecognitionPage::selectSampleFromCombo()
{
    const int comboIndex = m_sampleCombo ? m_sampleCombo->currentIndex() : -1;
    if (comboIndex < 0) {
        return;
    }
    const int index = m_sampleCombo->itemData(comboIndex).toInt();
    if (index < 0 || index >= m_samples.size()) {
        return;
    }
    m_selectedSampleIndex = index;
    const SampleRecord &sample = m_samples.at(index);
    m_samplePathEdit->setText(sample.sampleFilePath);
    m_samplePathEdit->setToolTip(sample.sampleFilePath);
    m_sampleSummary->setText(QStringLiteral("样本：%1\n类别：%2\n中心频率：%3 MHz，带宽：%4 MHz")
                                 .arg(sample.sampleId,
                                      sample.labelType.isEmpty() ? QStringLiteral("未标注") : sample.labelType)
                                 .arg(sample.centerFrequencyHz / 1000000.0, 0, 'f', 3)
                                 .arg(sample.bandwidthHz / 1000000.0, 0, 'f', 3));
}

void RecognitionPage::chooseSample()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择识别样本"),
                                                      ProjectPaths::samplesDirectory(),
                                                      QStringLiteral("NPZ 样本 (*.npz);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_selectedSampleIndex = -1;
        m_samplePathEdit->setText(path);
        m_samplePathEdit->setToolTip(path);
        m_sampleSummary->setText(QStringLiteral("已选择外部样本：%1").arg(shortPath(path)));
        updateRunAvailability();
    }
}

void RecognitionPage::runRecognition()
{
    if (!m_runner || m_runner->isRunning()) {
        return;
    }
    if (m_selectedModelPath.isEmpty() || !QFileInfo::exists(m_selectedModelPath)) {
        handleRecognitionFailed(QStringLiteral("请先选择一个存在的最佳模型。"));
        return;
    }
    const QString samplePath = m_samplePathEdit ? m_samplePathEdit->text().trimmed() : QString();
    if (samplePath.isEmpty() || !QFileInfo::exists(samplePath)) {
        handleRecognitionFailed(QStringLiteral("请先选择一个存在的 NPZ 样本。"));
        return;
    }
    RecognitionRequest request;
    request.modelPath = m_selectedModelPath;
    request.samplePath = samplePath;
    setBusy(true);
    m_resultDetail->setText(QStringLiteral("正在启动 GPU 识别进程。"));
    m_resultMetric->setValue(QStringLiteral("识别中"));
    m_confidenceMetric->setValue(QStringLiteral("-"));
    if (!m_runner->start(request)) {
        setBusy(false);
    }
}

void RecognitionPage::handleRecognitionFinished(const RecognitionResult &result)
{
    setBusy(false);
    if (!result.success) {
        handleRecognitionFailed(result.message);
        return;
    }
    m_modelMetric->setValue(QStringLiteral("已加载"));
    m_confidenceMetric->setValue(QStringLiteral("%1%").arg(result.confidence, 0, 'f', 2));
    m_resultMetric->setValue(result.predictedType.isEmpty() ? QStringLiteral("未知") : result.predictedType);
    m_resultDetail->setText(QStringLiteral("样本：%1\n预测类型：%2\n预测个体：%3\n模型：%4")
                                .arg(shortPath(result.samplePath),
                                     result.predictedType,
                                     result.predictedIndividual.isEmpty() ? QStringLiteral("未单独训练个体分类") : result.predictedIndividual,
                                     QDir::toNativeSeparators(result.modelPath)));
    m_topCandidates->setText(QStringLiteral("Top-K 候选：%1\n设备：%2 · 特征：%3")
                                 .arg(result.topCandidates.join(QStringLiteral("；")),
                                      result.device.isEmpty() ? QStringLiteral("CUDA") : result.device,
                                      result.featureMode));
    const int row = m_resultTable->rowCount();
    m_resultTable->insertRow(row);
    m_resultTable->setItem(row, 0, readOnlyItem(shortPath(result.samplePath)));
    m_resultTable->setItem(row, 1, readOnlyItem(result.predictedType));
    m_resultTable->setItem(row, 2, readOnlyItem(result.predictedIndividual.isEmpty() ? QStringLiteral("-") : result.predictedIndividual));
    m_resultTable->setItem(row, 3, readOnlyItem(QStringLiteral("%1%").arg(result.confidence, 0, 'f', 2)));
    m_resultTable->setItem(row, 4, readOnlyItem(result.device));
}

void RecognitionPage::handleRecognitionFailed(const QString &message)
{
    setBusy(false);
    m_resultMetric->setValue(QStringLiteral("失败"));
    m_resultDetail->setText(message);
}

void RecognitionPage::setBusy(bool busy)
{
    m_runButton->setEnabled(!busy && hasUsableModel()
                            && m_samplePathEdit
                            && QFileInfo::exists(m_samplePathEdit->text().trimmed()));
    m_cancelButton->setEnabled(busy);
    m_modelCombo->setEnabled(!busy);
    m_sampleCombo->setEnabled(!busy);
}

void RecognitionPage::updateRunAvailability()
{
    setBusy(m_runner && m_runner->isRunning());
}
