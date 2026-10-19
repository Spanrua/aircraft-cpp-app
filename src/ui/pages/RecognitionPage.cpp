#include "ui/pages/RecognitionPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "ui/widgets/MetricCard.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString csvField(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString shortPath(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? info.fileName() : path;
}

struct RecognitionDisplay
{
    QString protocolType;
    QString individual;
};

QString normalizedRecognitionLabel(const QString &value)
{
    QString normalized;
    for (const QChar character : value.toLower()) {
        if (character.isLetterOrNumber() || character == QLatin1Char('+')) {
            normalized.append(character);
        }
    }
    return normalized;
}

RecognitionDisplay displayForRecognition(const RecognitionResult &result)
{
    struct ProtocolMapping
    {
        const char *token;
        const char *protocolType;
        const char *individual;
    };
    static const ProtocolMapping mappings[] = {
        {"air2s", "O3", "Air2s"},
        {"mini3pro", "O3", "Mini3 Pro"},
        {"air3s", "O4", "Air3s"},
        {"air3", "O4", "Air3"},
        {"mini4pro", "O4", "Mini4 Pro"},
        {"yu4pro", "O4+", "Yu4Pro"},
        {"mini5pro", "O4+", "Mini5 Pro"},
    };

    const QStringList candidates = {result.predictedIndividual, result.predictedType};
    for (const QString &candidate : candidates) {
        const QString normalized = normalizedRecognitionLabel(candidate);
        for (const ProtocolMapping &mapping : mappings) {
            if (normalized.contains(QLatin1String(mapping.token))) {
                return {QLatin1String(mapping.protocolType), QLatin1String(mapping.individual)};
            }
        }
    }

    const QString rawIndividual = !result.predictedIndividual.trimmed().isEmpty()
        ? result.predictedIndividual.trimmed()
        : result.predictedType.trimmed();
    const QString normalizedType = normalizedRecognitionLabel(result.predictedType);
    if (normalizedType == QStringLiteral("o3")
        || normalizedType == QStringLiteral("o4")
        || normalizedType == QStringLiteral("o4+")) {
        return {result.predictedType.trimmed().toUpper(), rawIndividual.isEmpty() ? QStringLiteral("-") : rawIndividual};
    }
    return {QStringLiteral("未映射"), rawIndividual.isEmpty() ? QStringLiteral("-") : rawIndividual};
}

struct CaptureSegmentSummary
{
    RecognitionDisplay display;
    double confidence = 0.0;
    int supportingWindows = 0;
    int completedWindows = 0;
};

CaptureSegmentSummary summarizeCaptureSegment(const QVector<RecognitionResult> &results)
{
    const auto clampProbability = [](double value) {
        if (value < 0.0) {
            return 0.0;
        }
        return value > 1.0 ? 1.0 : value;
    };
    QMap<QString, double> accumulatedProbabilities;
    for (const RecognitionResult &result : results) {
        if (result.classProbabilities.isEmpty()) {
            const QString label = !result.predictedIndividual.trimmed().isEmpty()
                ? result.predictedIndividual.trimmed()
                : result.predictedType.trimmed();
            if (!label.isEmpty()) {
                accumulatedProbabilities[label] += clampProbability(result.confidence / 100.0);
            }
            continue;
        }
        for (auto probability = result.classProbabilities.cbegin(); probability != result.classProbabilities.cend(); ++probability) {
            accumulatedProbabilities[probability.key()] += clampProbability(probability.value());
        }
    }

    CaptureSegmentSummary summary;
    summary.completedWindows = results.size();
    if (accumulatedProbabilities.isEmpty() || results.isEmpty()) {
        summary.display = {QStringLiteral("未映射"), QStringLiteral("-")};
        return summary;
    }

    auto best = accumulatedProbabilities.cbegin();
    for (auto candidate = accumulatedProbabilities.cbegin(); candidate != accumulatedProbabilities.cend(); ++candidate) {
        if (candidate.value() > best.value()) {
            best = candidate;
        }
    }
    RecognitionResult fusedResult;
    fusedResult.predictedType = best.key();
    fusedResult.predictedIndividual = best.key();
    summary.display = displayForRecognition(fusedResult);
    summary.confidence = best.value() * 100.0 / results.size();
    for (const RecognitionResult &result : results) {
        if (displayForRecognition(result).individual == summary.display.individual) {
            ++summary.supportingWindows;
        }
    }
    return summary;
}

struct ExternalModelVerification
{
    QString status = QStringLiteral("unverified");
    QString message;
    QStringList classNames;
    QString featureMode;
    QString modelSize;
    int sampleLength = 4096;
};

bool loadJsonObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("缺少 %1").arg(QFileInfo(path).fileName());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("%1 不是有效 JSON 对象").arg(QFileInfo(path).fileName());
        return false;
    }
    *object = document.object();
    return true;
}

bool classNamesFromJson(const QJsonValue &value, QStringList *classNames)
{
    if (!value.isArray()) {
        return false;
    }
    QStringList parsed;
    for (const QJsonValue &item : value.toArray()) {
        const QString name = item.toString().trimmed();
        if (name.isEmpty()) {
            return false;
        }
        parsed.append(name);
    }
    if (parsed.isEmpty()) {
        return false;
    }
    *classNames = parsed;
    return true;
}

QString sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

ExternalModelVerification inspectExternalModel(const QFileInfo &modelInfo)
{
    ExternalModelVerification result;
    const QDir directory(modelInfo.absolutePath());
    QJsonObject config;
    QJsonObject artifact;
    QJsonObject classNamesDocument;
    QStringList problems;
    QString error;

    if (!loadJsonObject(directory.filePath(QStringLiteral("run_config.json")), &config, &error)) {
        problems.append(error);
    }
    if (!loadJsonObject(directory.filePath(QStringLiteral("artifact_report.json")), &artifact, &error)) {
        problems.append(error);
    }

    const QString classNamesPath = directory.filePath(QStringLiteral("class_names.json"));
    QFile classNamesFile(classNamesPath);
    QJsonParseError classNamesError;
    QJsonDocument classNamesJson;
    if (!classNamesFile.open(QIODevice::ReadOnly)) {
        problems.append(QStringLiteral("缺少 class_names.json"));
    } else {
        classNamesJson = QJsonDocument::fromJson(classNamesFile.readAll(), &classNamesError);
        if (classNamesError.error != QJsonParseError::NoError || !classNamesJson.isArray()) {
            problems.append(QStringLiteral("class_names.json 不是有效 JSON 数组"));
        }
    }

    QStringList configClassNames;
    QStringList fileClassNames;
    QStringList artifactClassNames;
    if (!config.isEmpty()) {
        if (!classNamesFromJson(config.value(QStringLiteral("class_names")), &configClassNames)) {
            problems.append(QStringLiteral("run_config.json 缺少有效类别顺序"));
        }
        const QJsonObject args = config.value(QStringLiteral("args")).toObject();
        result.featureMode = config.value(QStringLiteral("feature_mode"))
                                 .toString(args.value(QStringLiteral("feature_mode")).toString());
        result.modelSize = config.value(QStringLiteral("model_arch"))
                               .toString(args.value(QStringLiteral("model_arch")).toString());
        const int stitchWindows = args.value(QStringLiteral("stitch_windows")).toInt(1);
        const int effectiveStitchWindows = stitchWindows < 1 ? 1 : stitchWindows;
        result.sampleLength = effectiveStitchWindows * 4096;
        if (result.featureMode.isEmpty() || result.modelSize.isEmpty()) {
            problems.append(QStringLiteral("run_config.json 缺少特征模式或网络结构"));
        }
    }
    if (classNamesJson.isArray()
        && !classNamesFromJson(QJsonValue(classNamesJson.array()), &fileClassNames)) {
        problems.append(QStringLiteral("class_names.json 缺少有效类别顺序"));
    }
    if (!artifact.isEmpty()) {
        if (artifact.value(QStringLiteral("checkpoint")).toString() != modelInfo.fileName()) {
            problems.append(QStringLiteral("artifact_report.json 的权重文件名不匹配"));
        }
        if (!classNamesFromJson(artifact.value(QStringLiteral("class_names")), &artifactClassNames)) {
            problems.append(QStringLiteral("artifact_report.json 缺少有效类别顺序"));
        }
        const QString expectedHash = artifact.value(QStringLiteral("sha256")).toObject()
                                         .value(modelInfo.fileName()).toString().toLower();
        const QString actualHash = sha256File(modelInfo.absoluteFilePath());
        if (expectedHash.isEmpty() || actualHash.isEmpty() || expectedHash != actualHash.toLower()) {
            problems.append(QStringLiteral("权重 SHA-256 与交付清单不匹配"));
        }
    }
    if (!configClassNames.isEmpty() && !fileClassNames.isEmpty() && configClassNames != fileClassNames) {
        problems.append(QStringLiteral("run_config.json 与 class_names.json 的类别顺序不一致"));
    }
    if (!configClassNames.isEmpty() && !artifactClassNames.isEmpty() && configClassNames != artifactClassNames) {
        problems.append(QStringLiteral("run_config.json 与 artifact_report.json 的类别顺序不一致"));
    }

    result.classNames = !configClassNames.isEmpty() ? configClassNames : fileClassNames;
    if (problems.isEmpty()) {
        result.status = QStringLiteral("verified");
        result.message = QStringLiteral("已验证交付包元数据、类别顺序和权重 SHA-256。");
    } else {
        result.message = QStringLiteral("未验证：%1。识别前将要求再次确认。").arg(problems.join(QStringLiteral("；")));
    }
    return result;
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
    auto *modelRow = new QHBoxLayout();
    m_modelCombo = new QComboBox(modelPanel);
    m_modelCombo->setObjectName(QStringLiteral("RecognitionModelCombo"));
    m_chooseModelButton = new QPushButton(QStringLiteral("导入外部模型"), modelPanel);
    m_chooseModelButton->setObjectName(QStringLiteral("RecognitionChooseModelButton"));
    m_chooseModelButton->setProperty("secondary", true);
    modelRow->addWidget(m_modelCombo, 1);
    modelRow->addWidget(m_chooseModelButton);
    modelLayout->addRow(QStringLiteral("当前模型"), modelRow);
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
    m_chooseSampleButton = new QPushButton(QStringLiteral("选择 NPZ"), samplePanel);
    m_chooseSampleButton->setObjectName(QStringLiteral("RecognitionChooseSampleButton"));
    m_chooseSampleButton->setProperty("secondary", true);
    samplePathRow->addWidget(m_samplePathEdit, 1);
    samplePathRow->addWidget(m_chooseSampleButton);
    sampleLayout->addRow(QStringLiteral("样本文件"), samplePathRow);
    m_sampleSummary = new QLabel(QStringLiteral("请选择一个包含 iq 数组的 NPZ 样本。"), samplePanel);
    m_sampleSummary->setObjectName(QStringLiteral("RecognitionSampleSummary"));
    m_sampleSummary->setWordWrap(true);
    sampleLayout->addRow(QStringLiteral("样本信息"), m_sampleSummary);
    auto *batchSampleRow = new QHBoxLayout();
    m_batchSampleSummary = new QLabel(QStringLiteral("未选择额外批量样本；也可直接使用数据集已登记样本。"), samplePanel);
    m_batchSampleSummary->setObjectName(QStringLiteral("RecognitionBatchSampleSummary"));
    m_batchSampleSummary->setWordWrap(true);
    m_chooseBatchSamplesButton = new QPushButton(QStringLiteral("选择批量文件"), samplePanel);
    m_chooseBatchSamplesButton->setObjectName(QStringLiteral("RecognitionChooseBatchSamplesButton"));
    m_chooseBatchSamplesButton->setProperty("secondary", true);
    m_chooseBatchDirectoryButton = new QPushButton(QStringLiteral("选择文件夹"), samplePanel);
    m_chooseBatchDirectoryButton->setObjectName(QStringLiteral("RecognitionChooseBatchDirectoryButton"));
    m_chooseBatchDirectoryButton->setProperty("secondary", true);
    batchSampleRow->addWidget(m_batchSampleSummary, 1);
    batchSampleRow->addWidget(m_chooseBatchSamplesButton);
    batchSampleRow->addWidget(m_chooseBatchDirectoryButton);
    sampleLayout->addRow(QStringLiteral("批量样本"), batchSampleRow);
    contentLayout()->addWidget(samplePanel);

    auto *actions = new QHBoxLayout();
    m_refreshButton = new QPushButton(QStringLiteral("刷新模型和样本"), this);
    m_refreshButton->setObjectName(QStringLiteral("RecognitionRefreshButton"));
    m_refreshButton->setProperty("secondary", true);
    m_runButton = new QPushButton(QStringLiteral("执行识别"), this);
    m_runButton->setObjectName(QStringLiteral("RecognitionRunButton"));
    m_runButton->setProperty("primary", true);
    m_batchRunButton = new QPushButton(QStringLiteral("执行批量识别"), this);
    m_batchRunButton->setObjectName(QStringLiteral("RecognitionRunAllSamplesButton"));
    m_batchRunButton->setProperty("secondary", true);
    m_cancelButton = new QPushButton(QStringLiteral("取消识别"), this);
    m_cancelButton->setObjectName(QStringLiteral("RecognitionCancelButton"));
    m_cancelButton->setProperty("secondary", true);
    m_cancelButton->setEnabled(false);
    m_retryFailedButton = new QPushButton(QStringLiteral("重试失败样本"), this);
    m_retryFailedButton->setObjectName(QStringLiteral("RecognitionRetryFailedButton"));
    m_retryFailedButton->setProperty("secondary", true);
    m_retryFailedButton->setEnabled(false);
    m_exportResultsButton = new QPushButton(QStringLiteral("导出结果 CSV"), this);
    m_exportResultsButton->setObjectName(QStringLiteral("RecognitionExportResultsButton"));
    m_exportResultsButton->setProperty("secondary", true);
    m_exportResultsButton->setEnabled(false);
    actions->addWidget(m_refreshButton);
    actions->addWidget(m_runButton);
    actions->addWidget(m_batchRunButton);
    actions->addWidget(m_retryFailedButton);
    actions->addWidget(m_exportResultsButton);
    actions->addWidget(m_cancelButton);
    actions->addStretch(1);
    contentLayout()->addLayout(actions);

    m_resultDetail = new QLabel(QStringLiteral("等待识别。"), this);
    m_resultDetail->setObjectName(QStringLiteral("RecognitionResultDetail"));
    m_resultDetail->setWordWrap(true);
    contentLayout()->addWidget(m_resultDetail);
    m_segmentSummary = new QLabel(QStringLiteral("段级融合：选择同一段采集信号切分出的文件夹后，将以全部窗口的类别概率生成一条综合结论。"), this);
    m_segmentSummary->setObjectName(QStringLiteral("RecognitionSegmentSummary"));
    m_segmentSummary->setWordWrap(true);
    m_segmentSummary->setProperty("summary", true);
    contentLayout()->addWidget(m_segmentSummary);
    m_provenanceLabel = new QLabel(QStringLiteral("模型来源：未选择模型 · 数据集版本：- · 输入样本：-\n识别结果为模型建议；置信度低于 70% 时请转数据集页人工复核。"), this);
    m_provenanceLabel->setObjectName(QStringLiteral("RecognitionProvenanceLabel"));
    m_provenanceLabel->setWordWrap(true);
    m_provenanceLabel->setProperty("summary", true);
    contentLayout()->addWidget(m_provenanceLabel);
    m_batchProgress = new QProgressBar(this);
    m_batchProgress->setObjectName(QStringLiteral("RecognitionBatchProgressBar"));
    m_batchProgress->setRange(0, 0);
    m_batchProgress->setFormat(QStringLiteral("等待批量任务"));
    m_batchProgress->setTextVisible(true);
    contentLayout()->addWidget(m_batchProgress);
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
        QStringLiteral("链路类型"),
        QStringLiteral("识别个体"),
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
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        loadModels();
        loadSamples();
    });
    connect(m_modelCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { selectModelFromCombo(); });
    connect(m_sampleCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { selectSampleFromCombo(); });
    connect(m_modelCombo, qOverload<int>(&QComboBox::activated),
            this, [this]() { emit recognitionInputChanged(); });
    connect(m_sampleCombo, qOverload<int>(&QComboBox::activated),
            this, [this]() { emit recognitionInputChanged(); });
    connect(m_chooseModelButton, &QPushButton::clicked, this, &RecognitionPage::chooseModel);
    connect(m_chooseSampleButton, &QPushButton::clicked, this, &RecognitionPage::chooseSample);
    connect(m_chooseBatchSamplesButton, &QPushButton::clicked, this, &RecognitionPage::chooseBatchSamples);
    connect(m_chooseBatchDirectoryButton, &QPushButton::clicked, this, &RecognitionPage::chooseBatchSampleDirectory);
    connect(m_runButton, &QPushButton::clicked, this, &RecognitionPage::runRecognition);
    connect(m_batchRunButton, &QPushButton::clicked, this, &RecognitionPage::runAllDatasetSamples);
    connect(m_retryFailedButton, &QPushButton::clicked, this, &RecognitionPage::retryFailedBatchSamples);
    connect(m_exportResultsButton, &QPushButton::clicked, this, &RecognitionPage::exportRecognitionResults);
    connect(m_cancelButton, &QPushButton::clicked, this, &RecognitionPage::cancelRecognition);
    connect(m_runner, &RecognitionProcessRunner::finished,
            this, &RecognitionPage::handleRecognitionFinished);
    connect(m_runner, &RecognitionProcessRunner::failed,
            this, &RecognitionPage::handleRecognitionFailed);
    loadModels();
    loadSamples();
    updateRunAvailability();
}

RecognitionPage::~RecognitionPage()
{
    if (m_runner) {
        m_runner->cancel();
        delete m_runner;
        m_runner = nullptr;
    }
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
    const QFileInfo modelInfo(modelPath);
    if (modelPath.trimmed().isEmpty() || !modelInfo.isFile()) {
        if (m_modelSummary) {
            m_modelSummary->setText(QStringLiteral("模型文件不存在：%1")
                                        .arg(QDir::toNativeSeparators(modelPath)));
        }
        return;
    }

    const QString absoluteModelPath = modelInfo.absoluteFilePath();
    const ExternalModelVerification verification = inspectExternalModel(modelInfo);
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    int matchingIndex = -1;
    for (int index = 0; index < state.trainingRuns.size(); ++index) {
        if (QFileInfo(state.trainingRuns.at(index).bestModelPath).absoluteFilePath() == absoluteModelPath) {
            matchingIndex = index;
            break;
        }
    }

    if (matchingIndex < 0) {
        TrainingRunRecord importedRun;
        QString runId = QStringLiteral("external-%1").arg(modelInfo.completeBaseName());
        int suffix = 2;
        auto runIdInUse = [&state](const QString &candidate) {
            for (const TrainingRunRecord &run : state.trainingRuns) {
                if (run.runId == candidate) {
                    return true;
                }
            }
            return false;
        };
        while (runIdInUse(runId)) {
            runId = QStringLiteral("external-%1-%2").arg(modelInfo.completeBaseName()).arg(suffix++);
        }
        importedRun.runId = runId;
        importedRun.datasetVersionId = datasetVersionId.isEmpty()
            ? QStringLiteral("external-model") : datasetVersionId;
        importedRun.outputDirectory = modelInfo.absolutePath();
        importedRun.bestModelPath = absoluteModelPath;
        importedRun.state = QStringLiteral("已完成");
        importedRun.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
        importedRun.message = QStringLiteral("用户导入的外部识别模型");
        importedRun.modelMode = QStringLiteral("closed_set");
        importedRun.modelAvailable = true;
        importedRun.pipelineVersion = QStringLiteral("external-import");
        importedRun.featureMode = verification.featureMode;
        importedRun.modelSize = verification.modelSize;
        importedRun.sampleLength = verification.sampleLength;
        importedRun.modelVerificationStatus = verification.status;
        importedRun.modelVerificationMessage = verification.message;
        importedRun.modelClassNames = verification.classNames;
        importedRun.summaryPath = QDir(modelInfo.absolutePath()).filePath(QStringLiteral("artifact_report.json"));
        state.trainingRuns.append(importedRun);
        matchingIndex = state.trainingRuns.size() - 1;
    }

    TrainingRunRecord &selectedRun = state.trainingRuns[matchingIndex];
    selectedRun.bestModelPath = absoluteModelPath;
    selectedRun.modelMode = QStringLiteral("closed_set");
    selectedRun.state = QStringLiteral("已完成");
    selectedRun.modelAvailable = true;
    if (selectedRun.pipelineVersion == QStringLiteral("external-import")) {
        selectedRun.modelVerificationStatus = verification.status;
        selectedRun.modelVerificationMessage = verification.message;
        selectedRun.modelClassNames = verification.classNames;
    }
    if (!datasetVersionId.isEmpty()) {
        selectedRun.datasetVersionId = datasetVersionId;
    }
    state.activeTrainingRunId = selectedRun.runId;
    if (!store.save(state)) {
        if (m_modelSummary) {
            m_modelSummary->setText(QStringLiteral("模型登记失败：%1").arg(store.lastError()));
        }
        return;
    }
    loadModels();
    emit recognitionInputChanged();
}

void RecognitionPage::chooseModel()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入外部识别模型"),
                                                      ProjectPaths::modelsDirectory(),
                                                      QStringLiteral("PyTorch 模型 (*.pth *.pt);;所有文件 (*)"));
    if (!path.isEmpty()) {
        const ExternalModelVerification verification = inspectExternalModel(QFileInfo(path));
        if (verification.status == QStringLiteral("unverified")) {
            const auto answer = QMessageBox::warning(
                this,
                QStringLiteral("外部模型未验证"),
                verification.message + QStringLiteral("\n\n仍要导入该模型吗？"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
        setModelForRecognition(path, QStringLiteral("external-model"));
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
        const QString label = QStringLiteral("%1 · %2 · %3 · %4")
                                  .arg(run.runId, run.datasetVersionId, run.state,
                                       run.modelVerificationStatus == QStringLiteral("unverified")
                                           ? QStringLiteral("未验证") : QStringLiteral("已验证"));
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
    m_selectedSampleNpzIndex = 0;
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
    const bool unverified = run.modelVerificationStatus == QStringLiteral("unverified");
    m_modelMetric->setValue(unverified ? QStringLiteral("未验证") : QStringLiteral("已验证"));
    m_modelSummary->setText(QStringLiteral("训练任务：%1\n数据集版本：%2\n验证状态：%3\n验证说明：%4\n验证准确率：%5%\n设备：%6\n模型：%7")
                                .arg(run.runId, run.datasetVersionId)
                                .arg(unverified ? QStringLiteral("未验证") : QStringLiteral("已验证"))
                                .arg(run.modelVerificationMessage.isEmpty()
                                         ? QStringLiteral("训练流程生成的模型")
                                         : run.modelVerificationMessage)
                                .arg(run.bestValAccuracy, 0, 'f', 2)
                                .arg(run.gpuDeviceName.isEmpty() ? QStringLiteral("CUDA GPU") : run.gpuDeviceName,
                                     QDir::toNativeSeparators(run.bestModelPath)));
    updateProvenance();
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
    m_selectedSampleNpzIndex = sample.sampleIndex;
    m_samplePathEdit->setText(sample.sampleFilePath);
    m_samplePathEdit->setToolTip(sample.sampleFilePath);
    m_sampleSummary->setText(QStringLiteral("样本：%1\n类别：%2\n中心频率：%3 MHz，带宽：%4 MHz")
                                 .arg(sample.sampleId,
                                      sample.labelType.isEmpty() ? QStringLiteral("未标注") : sample.labelType)
                                 .arg(sample.centerFrequencyHz / 1000000.0, 0, 'f', 3)
                                 .arg(sample.bandwidthHz / 1000000.0, 0, 'f', 3));
    updateProvenance();
}

void RecognitionPage::chooseSample()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择识别样本"),
                                                      ProjectPaths::samplesDirectory(),
                                                      QStringLiteral("NPZ 样本 (*.npz);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_selectedSampleIndex = -1;
        m_selectedSampleNpzIndex = 0;
        m_samplePathEdit->setText(path);
        m_samplePathEdit->setToolTip(path);
        m_sampleSummary->setText(QStringLiteral("已选择外部样本：%1").arg(shortPath(path)));
        updateProvenance();
        updateRunAvailability();
        emit recognitionInputChanged();
    }
}

void RecognitionPage::chooseBatchSamples()
{
    QFileDialog dialog(this, QStringLiteral("选择批量识别样本"), ProjectPaths::samplesDirectory(),
                       QStringLiteral("NPZ 样本 (*.npz);;所有文件 (*)"));
    dialog.setFileMode(QFileDialog::ExistingFiles);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList selectedPaths;
    for (const QString &selectedPath : dialog.selectedFiles()) {
        const QFileInfo info(selectedPath);
        if (!info.isFile() || info.suffix().compare(QStringLiteral("npz"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString absolutePath = info.absoluteFilePath();
        if (!selectedPaths.contains(absolutePath)) {
            selectedPaths.append(absolutePath);
        }
    }
    if (selectedPaths.isEmpty()) {
        m_batchSampleSummary->setText(QStringLiteral("未选择可用的 NPZ 文件。"));
        updateRunAvailability();
        return;
    }

    m_externalBatchSamplePaths = selectedPaths;
    m_batchSegmentDirectory.clear();
    m_batchSampleSummary->setText(QStringLiteral("已选择 %1 个外部 NPZ 样本，将与数据集已登记样本合并并去重。")
                                      .arg(m_externalBatchSamplePaths.size()));
    m_segmentSummary->setText(QStringLiteral("段级融合：未选择采集段文件夹；本次将按独立样本展示结果。"));
    updateRunAvailability();
    emit recognitionInputChanged();
}

void RecognitionPage::chooseBatchSampleDirectory()
{
    QFileDialog dialog(this, QStringLiteral("选择包含 NPZ 样本的文件夹"), ProjectPaths::samplesDirectory());
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString selectedDirectory = dialog.selectedFiles().isEmpty()
        ? dialog.directory().absolutePath()
        : dialog.selectedFiles().constFirst();
    QFileInfo directoryInfo(selectedDirectory);
    if (!directoryInfo.isDir()) {
        const QFileInfo dialogDirectoryInfo(dialog.directory().absolutePath());
        if (dialogDirectoryInfo.isDir()) {
            directoryInfo = dialogDirectoryInfo;
        }
    }
    if (!directoryInfo.isDir()) {
        m_batchSampleSummary->setText(QStringLiteral("请选择一个包含 NPZ 样本的文件夹。"));
        return;
    }

    QStringList selectedPaths;
    QDirIterator iterator(directoryInfo.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo sampleInfo(iterator.next());
        if (sampleInfo.suffix().compare(QStringLiteral("npz"), Qt::CaseInsensitive) == 0) {
            selectedPaths.append(sampleInfo.absoluteFilePath());
        }
    }
    selectedPaths.removeDuplicates();
    selectedPaths.sort();
    if (selectedPaths.isEmpty()) {
        m_batchSampleSummary->setText(QStringLiteral("所选文件夹中未找到 NPZ 样本，保留当前批量选择。"));
        return;
    }

    m_externalBatchSamplePaths = selectedPaths;
    m_batchSegmentDirectory = directoryInfo.absoluteFilePath();
    m_batchSampleSummary->setText(QStringLiteral("已从采集段文件夹收集 %1 个 NPZ 样本（含子文件夹）；本次只融合该文件夹内窗口，不混入数据集样本。")
                                      .arg(m_externalBatchSamplePaths.size()));
    m_segmentSummary->setText(QStringLiteral("已选择采集段：%1。完成后将显示综合结论、综合置信度和窗口一致性。")
                                  .arg(shortPath(m_batchSegmentDirectory)));
    updateRunAvailability();
    emit recognitionInputChanged();
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
    request.sampleIndex = m_selectedSampleNpzIndex;
    setBusy(true);
    m_resultDetail->setText(QStringLiteral("正在启动 GPU 识别进程。"));
    m_resultMetric->setValue(QStringLiteral("识别中"));
    m_confidenceMetric->setValue(QStringLiteral("-"));
    if (!m_runner->start(request)) {
        setBusy(false);
    }
}

void RecognitionPage::runAllDatasetSamples()
{
    if (!m_runner || m_runner->isRunning() || m_batchActive) {
        return;
    }
    if (m_selectedModelPath.isEmpty() || !QFileInfo::exists(m_selectedModelPath)) {
        handleRecognitionFailed(QStringLiteral("请先选择一个存在的最佳模型。"));
        return;
    }

    const QVector<RecognitionRequest> requests = availableBatchRequests();
    if (requests.isEmpty()) {
        handleRecognitionFailed(QStringLiteral("没有可用于批量识别的 NPZ 样本。请先选择文件、文件夹，或在数据集页登记样本。"));
        return;
    }

    beginBatchRecognition(requests, false);
}

void RecognitionPage::retryFailedBatchSamples()
{
    if (!m_runner || m_runner->isRunning() || m_batchActive || m_failedBatchRequests.isEmpty()) {
        return;
    }
    beginBatchRecognition(m_failedBatchRequests, true);
}

void RecognitionPage::beginBatchRecognition(const QVector<RecognitionRequest> &requests, bool retryingFailures)
{
    const bool isCaptureSegment = !m_batchSegmentDirectory.isEmpty();
    if (!retryingFailures) {
        m_captureSegmentResults.clear();
        m_captureSegmentExpectedWindows = isCaptureSegment ? requests.size() : 0;
    }
    m_batchRequests = requests;
    m_failedBatchRequests.clear();
    m_batchCurrentIndex = -1;
    m_batchSuccessCount = 0;
    m_batchFailureCount = 0;
    m_batchHighestConfidence = 0.0;
    m_batchCurrentFailureMessage.clear();
    m_batchActive = true;
    m_batchCancelled = false;
    m_batchElapsedTimer.start();
    m_resultTable->setRowCount(0);
    m_topCandidates->setText(retryingFailures
                                 ? QStringLiteral("正在重试上次失败的样本，原始数据不会被修改。")
                                 : (isCaptureSegment
                                        ? QStringLiteral("正在逐窗识别采集段；完成后将融合全部窗口的类别概率。")
                                        : QStringLiteral("批量识别将按样本顺序串行使用当前 GPU。")));
    if (isCaptureSegment && m_segmentSummary) {
        m_segmentSummary->setText(QStringLiteral("采集段融合中：正在收集 %1 个窗口的类别概率。")
                                      .arg(m_captureSegmentExpectedWindows));
    }
    m_resultMetric->setValue(QStringLiteral("批量识别中"));
    m_confidenceMetric->setValue(QStringLiteral("-"));
    m_batchProgress->setRange(0, m_batchRequests.size());
    m_batchProgress->setValue(0);
    updateBatchProgress();
    setBusy(true);
    startNextBatchRecognition();
}

void RecognitionPage::startNextBatchRecognition()
{
    if (!m_batchActive) {
        return;
    }
    if (m_batchCancelled || m_batchCurrentIndex + 1 >= m_batchRequests.size()) {
        finishBatchRecognition();
        return;
    }

    ++m_batchCurrentIndex;
    m_batchCurrentFailureMessage.clear();
    const RecognitionRequest request = m_batchRequests.at(m_batchCurrentIndex);
    m_resultDetail->setText(QStringLiteral("批量识别中：正在处理第 %1/%2 个样本\n样本：%3")
                                .arg(m_batchCurrentIndex + 1)
                                .arg(m_batchRequests.size())
                                .arg(shortPath(request.samplePath)));
    if (!m_runner->start(request)) {
        appendRecognitionFailure(request.samplePath,
                                 m_batchCurrentFailureMessage.isEmpty()
                                     ? QStringLiteral("无法启动识别进程。")
                                     : m_batchCurrentFailureMessage);
        ++m_batchFailureCount;
        m_failedBatchRequests.append(request);
        updateBatchProgress();
        QTimer::singleShot(0, this, &RecognitionPage::startNextBatchRecognition);
    }
}

void RecognitionPage::finishBatchRecognition()
{
    if (!m_batchActive) {
        return;
    }
    const int total = m_batchRequests.size();
    const QString state = m_batchCancelled ? QStringLiteral("批量识别已取消")
                                           : QStringLiteral("批量识别完成");
    m_resultDetail->setText(QStringLiteral("%1：成功 %2/%3，失败 %4；最高置信度：%5%")
                                .arg(state)
                                .arg(m_batchSuccessCount)
                                .arg(total)
                                .arg(m_batchFailureCount)
                                .arg(m_batchSuccessCount > 0 ? m_batchHighestConfidence : 0.0, 0, 'f', 2));
    m_confidenceMetric->setValue(m_batchSuccessCount > 0
                                     ? QStringLiteral("%1%").arg(m_batchHighestConfidence, 0, 'f', 2)
                                     : QStringLiteral("-"));
    m_resultMetric->setValue(m_batchCancelled
                                 ? QStringLiteral("已取消")
                                 : (m_batchFailureCount == 0 ? QStringLiteral("批量完成")
                                                              : QStringLiteral("部分失败")));
    if (m_batchSuccessCount == 0) {
        m_topCandidates->setText(QStringLiteral("批量任务没有产生成功的识别结果。"));
    }
    updateCaptureSegmentSummary();
    m_batchActive = false;
    m_batchCancelled = false;
    updateBatchProgress();
    setBusy(false);
}

void RecognitionPage::updateCaptureSegmentSummary()
{
    if (!m_segmentSummary || m_batchSegmentDirectory.isEmpty()) {
        return;
    }
    if (m_captureSegmentResults.isEmpty()) {
        m_segmentSummary->setText(QStringLiteral("采集段融合未生成有效窗口结果。请检查模型、样本文件和识别日志。"));
        return;
    }

    const CaptureSegmentSummary summary = summarizeCaptureSegment(m_captureSegmentResults);
    const int expectedWindows = m_captureSegmentExpectedWindows > summary.completedWindows
        ? m_captureSegmentExpectedWindows
        : summary.completedWindows;
    const double consistency = summary.completedWindows > 0
        ? 100.0 * summary.supportingWindows / summary.completedWindows
        : 0.0;
    const QString state = summary.confidence >= 80.0 && consistency >= 75.0
        ? QStringLiteral("识别可信")
        : QStringLiteral("建议复核");
    const QString confidenceText = QString::number(summary.confidence, 'f', 2);
    const QString consistencyText = QString::number(consistency, 'f', 2);
    m_segmentSummary->setText(QStringLiteral("采集段融合结果（%1）：\n段级结论：%2 · %3\n综合置信度：%4%\n窗口一致性：%5 / %6（%7%）\n有效窗口：%6 / %8")
                                  .arg(state)
                                  .arg(summary.display.protocolType)
                                  .arg(summary.display.individual)
                                  .arg(confidenceText)
                                  .arg(summary.supportingWindows)
                                  .arg(summary.completedWindows)
                                  .arg(consistencyText)
                                  .arg(expectedWindows));
    m_resultMetric->setValue(QStringLiteral("%1 · %2")
                                 .arg(summary.display.protocolType, summary.display.individual));
    m_confidenceMetric->setValue(QStringLiteral("%1%").arg(confidenceText));
    m_resultDetail->setText(QStringLiteral("采集段：%1\n段级结论：%2 · %3\n综合置信度：%4% · 窗口一致性：%5 / %6\n%7")
                                .arg(shortPath(m_batchSegmentDirectory))
                                .arg(summary.display.protocolType)
                                .arg(summary.display.individual)
                                .arg(confidenceText)
                                .arg(summary.supportingWindows)
                                .arg(summary.completedWindows)
                                .arg(state));
    m_topCandidates->setText(QStringLiteral("已融合 %1 个有效窗口的完整类别概率；%2。可在下方查看逐窗口明细。")
                                 .arg(summary.completedWindows)
                                 .arg(state));
}

void RecognitionPage::exportRecognitionResults()
{
    if (!m_resultTable || m_resultTable->rowCount() == 0) {
        return;
    }
    QDir().mkpath(ProjectPaths::exportsDirectory());
    const QString suggestedPath = QDir(ProjectPaths::exportsDirectory()).filePath(
        QStringLiteral("recognition_results_%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出识别结果"), suggestedPath,
                                                      QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法写入结果文件：%1").arg(file.errorString()));
        return;
    }
    QByteArray content("\xEF\xBB\xBF");
    QStringList headers;
    for (int column = 0; column < m_resultTable->columnCount(); ++column) {
        headers.append(csvField(m_resultTable->horizontalHeaderItem(column)->text()));
    }
    content.append(headers.join(QLatin1Char(',')).toUtf8());
    content.append('\n');
    for (int row = 0; row < m_resultTable->rowCount(); ++row) {
        QStringList fields;
        for (int column = 0; column < m_resultTable->columnCount(); ++column) {
            const auto *item = m_resultTable->item(row, column);
            fields.append(csvField(item ? item->text() : QString()));
        }
        content.append(fields.join(QLatin1Char(',')).toUtf8());
        content.append('\n');
    }
    if (file.write(content) != content.size()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("结果文件写入不完整：%1").arg(file.errorString()));
        return;
    }
    file.close();
    m_resultDetail->setText(QStringLiteral("识别结果已导出：%1").arg(QDir::toNativeSeparators(path)));
}

void RecognitionPage::updateBatchProgress()
{
    if (!m_batchProgress) {
        return;
    }
    const int total = m_batchRequests.size();
    int completed = m_batchSuccessCount + m_batchFailureCount;
    if (completed < 0) {
        completed = 0;
    } else if (completed > total) {
        completed = total;
    }
    m_batchProgress->setRange(0, total);
    m_batchProgress->setValue(completed);
    if (m_batchActive && completed > 0 && total > completed) {
        const qint64 elapsedMs = m_batchElapsedTimer.elapsed();
        const qint64 remainingMs = elapsedMs * (total - completed) / completed;
        m_batchProgress->setFormat(QStringLiteral("已完成 %1/%2，预计剩余约 %3 秒")
                                       .arg(completed)
                                       .arg(total)
                                       .arg(qMax<qint64>(1, (remainingMs + 999) / 1000)));
    } else if (m_batchActive) {
        m_batchProgress->setFormat(QStringLiteral("已完成 %1/%2").arg(completed).arg(total));
    } else if (total > 0) {
        m_batchProgress->setFormat(QStringLiteral("已完成 %1/%2").arg(completed).arg(total));
    } else {
        m_batchProgress->setFormat(QStringLiteral("等待批量任务"));
    }
}

void RecognitionPage::cancelRecognition()
{
    if (m_batchActive) {
        m_batchCancelled = true;
    }
    if (m_runner) {
        m_runner->cancel();
    }
}

void RecognitionPage::appendRecognitionResult(const RecognitionResult &result)
{
    const RecognitionDisplay display = displayForRecognition(result);
    const int row = m_resultTable->rowCount();
    m_resultTable->insertRow(row);
    m_resultTable->setItem(row, 0, readOnlyItem(shortPath(result.samplePath)));
    m_resultTable->setItem(row, 1, readOnlyItem(display.protocolType));
    m_resultTable->setItem(row, 2, readOnlyItem(display.individual));
    m_resultTable->setItem(row, 3, readOnlyItem(QStringLiteral("%1%").arg(result.confidence, 0, 'f', 2)));
    m_resultTable->setItem(row, 4, readOnlyItem(result.device));
    if (!m_batchActive && m_exportResultsButton) {
        m_exportResultsButton->setEnabled(true);
    }
}

void RecognitionPage::appendRecognitionFailure(const QString &samplePath, const QString &message)
{
    const int row = m_resultTable->rowCount();
    m_resultTable->insertRow(row);
    m_resultTable->setItem(row, 0, readOnlyItem(shortPath(samplePath)));
    m_resultTable->setItem(row, 1, readOnlyItem(QStringLiteral("失败")));
    m_resultTable->setItem(row, 2, readOnlyItem(message));
    m_resultTable->setItem(row, 3, readOnlyItem(QStringLiteral("-")));
    m_resultTable->setItem(row, 4, readOnlyItem(QStringLiteral("-")));
    if (!m_batchActive && m_exportResultsButton) {
        m_exportResultsButton->setEnabled(true);
    }
}

void RecognitionPage::handleRecognitionFinished(const RecognitionResult &result)
{
    if (m_batchActive) {
        if (result.success) {
            ++m_batchSuccessCount;
            if (result.confidence > m_batchHighestConfidence) {
                m_batchHighestConfidence = result.confidence;
            }
            if (!m_batchSegmentDirectory.isEmpty()) {
                m_captureSegmentResults.append(result);
            }
            appendRecognitionResult(result);
            m_modelMetric->setValue(QStringLiteral("已加载"));
            m_topCandidates->setText(QStringLiteral("最近完成样本的 Top-K 候选：%1\n设备：%2 · 特征：%3")
                                         .arg(result.topCandidates.join(QStringLiteral("；")),
                                              result.device.isEmpty() ? QStringLiteral("CUDA") : result.device,
                                              result.featureMode));
        } else {
            appendRecognitionFailure(result.samplePath,
                                     m_batchCurrentFailureMessage.isEmpty()
                                         ? result.message
                                         : m_batchCurrentFailureMessage);
            ++m_batchFailureCount;
            m_failedBatchRequests.append(m_batchRequests.at(m_batchCurrentIndex));
        }
        updateBatchProgress();
        if (m_batchCancelled) {
            finishBatchRecognition();
        } else {
            QTimer::singleShot(0, this, &RecognitionPage::startNextBatchRecognition);
        }
        return;
    }
    setBusy(false);
    if (!result.success) {
        handleRecognitionFailed(result.message);
        return;
    }
    m_modelMetric->setValue(QStringLiteral("已加载"));
    m_confidenceMetric->setValue(QStringLiteral("%1%").arg(result.confidence, 0, 'f', 2));
    const RecognitionDisplay display = displayForRecognition(result);
    m_resultMetric->setValue(display.protocolType);
    m_resultDetail->setText(QStringLiteral("样本：%1\n链路类型：%2\n识别个体：%3\n模型：%4")
                                .arg(shortPath(result.samplePath),
                                     display.protocolType,
                                     display.individual,
                                     QDir::toNativeSeparators(result.modelPath)));
    updateProvenance();
    m_topCandidates->setText(QStringLiteral("Top-K 候选：%1\n设备：%2 · 特征：%3")
                                 .arg(result.topCandidates.join(QStringLiteral("；")),
                                      result.device.isEmpty() ? QStringLiteral("CUDA") : result.device,
                                      result.featureMode));
    appendRecognitionResult(result);
    emit recognitionCompleted();
}

void RecognitionPage::handleRecognitionFailed(const QString &message)
{
    if (m_batchActive) {
        m_batchCurrentFailureMessage = message;
        return;
    }
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
    m_refreshButton->setEnabled(!busy);
    m_chooseModelButton->setEnabled(!busy);
    m_chooseSampleButton->setEnabled(!busy);
    m_chooseBatchSamplesButton->setEnabled(!busy);
    m_chooseBatchDirectoryButton->setEnabled(!busy);
    m_batchRunButton->setEnabled(!busy && hasUsableModel() && !availableBatchRequests().isEmpty());
    m_retryFailedButton->setEnabled(!busy && !m_failedBatchRequests.isEmpty());
    m_exportResultsButton->setEnabled(!busy && m_resultTable && m_resultTable->rowCount() > 0);
    m_modelCombo->setEnabled(!busy);
    m_sampleCombo->setEnabled(!busy);
}

QVector<RecognitionRequest> RecognitionPage::availableBatchRequests() const
{
    QVector<RecognitionRequest> requests;
    QStringList includedPaths;
    const auto appendRequest = [this, &requests, &includedPaths](const QString &samplePath, int sampleIndex) {
        const QFileInfo info(samplePath);
        if (!info.isFile() || info.suffix().compare(QStringLiteral("npz"), Qt::CaseInsensitive) != 0) {
            return;
        }
        const QString absolutePath = info.absoluteFilePath();
        if (includedPaths.contains(absolutePath)) {
            return;
        }
        RecognitionRequest request;
        request.modelPath = m_selectedModelPath;
        request.samplePath = absolutePath;
        request.sampleIndex = sampleIndex;
        requests.append(request);
        includedPaths.append(absolutePath);
    };

    if (m_batchSegmentDirectory.isEmpty()) {
        for (const SampleRecord &sample : m_samples) {
            appendRequest(sample.sampleFilePath, sample.sampleIndex);
        }
    }
    for (const QString &samplePath : m_externalBatchSamplePaths) {
        appendRequest(samplePath, 0);
    }
    return requests;
}

void RecognitionPage::updateRunAvailability()
{
    setBusy(m_batchActive || (m_runner && m_runner->isRunning()));
}

void RecognitionPage::updateProvenance()
{
    if (!m_provenanceLabel) {
        return;
    }
    QString modelSource = QStringLiteral("未选择模型");
    QString datasetVersion = QStringLiteral("-");
    if (m_selectedRunIndex >= 0 && m_selectedRunIndex < m_trainingRuns.size()) {
        const TrainingRunRecord &run = m_trainingRuns.at(m_selectedRunIndex);
        modelSource = run.pipelineVersion == QStringLiteral("external-import")
            ? QStringLiteral("外部导入：%1").arg(QFileInfo(run.bestModelPath).fileName())
            : QStringLiteral("训练任务：%1").arg(run.runId);
        datasetVersion = run.datasetVersionId.isEmpty() ? QStringLiteral("未记录") : run.datasetVersionId;
    }
    const QString inputPath = m_samplePathEdit ? m_samplePathEdit->text().trimmed() : QString();
    const QString inputSample = inputPath.isEmpty() ? QStringLiteral("未选择") : QFileInfo(inputPath).fileName();
    m_provenanceLabel->setText(QStringLiteral("模型来源：%1 · 数据集版本：%2 · 输入样本：%3\n识别结果为模型建议；置信度低于 70% 时请转数据集页人工复核。")
                                    .arg(modelSource, datasetVersion, inputSample));
}
