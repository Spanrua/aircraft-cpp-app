#include "ui/pages/TrainPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "ui/widgets/MetricCard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString percentText(double value)
{
    return value > 0.0 ? QStringLiteral("%1%").arg(value, 0, 'f', 2) : QStringLiteral("-");
}

QString trainingStateText(const TrainingRunRecord &run)
{
    return run.state.isEmpty() ? QStringLiteral("未知") : run.state;
}

QString manifestSha256(const QString &rootDirectory, qint64 *recordCount)
{
    QStringList manifestPaths;
    QDirIterator iterator(rootDirectory,
                          QStringList() << QStringLiteral("manifest.jsonl"),
                          QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        manifestPaths.append(iterator.next());
    }
    std::sort(manifestPaths.begin(), manifestPaths.end());

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 records = 0;
    for (const QString &path : manifestPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const QByteArray data = file.readAll();
        hash.addData(data);
        const QList<QByteArray> lines = data.split('\n');
        for (const QByteArray &line : lines) {
            if (!line.trimmed().isEmpty()) {
                ++records;
            }
        }
    }
    if (recordCount) {
        *recordCount = records;
    }
    return QString::fromLatin1(hash.result().toHex());
}

struct NpyHeader
{
    QString dtype;
    QVector<qint64> shape;
};

bool readNpyHeader(const QString &path, NpyHeader *header, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("无法打开 %1").arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    const QByteArray prefix = file.read(8);
    if (prefix.size() != 8 || prefix.left(6) != QByteArray("\x93NUMPY", 6)) {
        if (error) {
            *error = QStringLiteral("%1 不是有效的 NumPy 文件")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    const quint8 major = static_cast<quint8>(prefix.at(6));
    const quint8 minor = static_cast<quint8>(prefix.at(7));
    Q_UNUSED(minor)
    qint64 headerLength = 0;
    if (major == 1) {
        const QByteArray lengthBytes = file.read(2);
        if (lengthBytes.size() != 2) {
            return false;
        }
        headerLength = static_cast<quint8>(lengthBytes.at(0))
            | (static_cast<qint64>(static_cast<quint8>(lengthBytes.at(1))) << 8);
    } else if (major == 2 || major == 3) {
        const QByteArray lengthBytes = file.read(4);
        if (lengthBytes.size() != 4) {
            return false;
        }
        headerLength = static_cast<quint8>(lengthBytes.at(0))
            | (static_cast<qint64>(static_cast<quint8>(lengthBytes.at(1))) << 8)
            | (static_cast<qint64>(static_cast<quint8>(lengthBytes.at(2))) << 16)
            | (static_cast<qint64>(static_cast<quint8>(lengthBytes.at(3))) << 24);
    } else {
        if (error) {
            *error = QStringLiteral("%1 使用不支持的 NumPy 版本")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    if (headerLength <= 0 || headerLength > 1024 * 1024) {
        if (error) {
            *error = QStringLiteral("%1 的 NumPy 头长度无效")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    const QString text = QString::fromLatin1(file.read(headerLength));
    const QRegularExpression descrPattern(QStringLiteral("'descr'\\s*:\\s*'([^']+)'"));
    const QRegularExpression shapePattern(QStringLiteral("'shape'\\s*:\\s*\\(([^)]*)\\)"));
    const QRegularExpressionMatch descrMatch = descrPattern.match(text);
    const QRegularExpressionMatch shapeMatch = shapePattern.match(text);
    if (!descrMatch.hasMatch() || !shapeMatch.hasMatch()) {
        if (error) {
            *error = QStringLiteral("%1 的 NumPy 头缺少 dtype 或 shape")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    QVector<qint64> shape;
    const QStringList dimensions = shapeMatch.captured(1).split(
        QChar(','), QString::SkipEmptyParts);
    for (const QString &dimension : dimensions) {
        bool ok = false;
        const qint64 value = dimension.trimmed().toLongLong(&ok);
        if (!ok || value < 0) {
            if (error) {
                *error = QStringLiteral("%1 的 NumPy shape 无效")
                             .arg(QDir::toNativeSeparators(path));
            }
            return false;
        }
        shape.append(value);
    }
    if (header) {
        header->dtype = descrMatch.captured(1);
        header->shape = shape;
    }
    return true;
}

bool shapeMatches(const QVector<qint64> &actual, const QJsonValue &expectedValue)
{
    const QJsonArray expected = expectedValue.toArray();
    if (expected.size() != actual.size()) {
        return false;
    }
    for (int index = 0; index < actual.size(); ++index) {
        if (expected.at(index).toVariant().toLongLong() != actual.at(index)) {
            return false;
        }
    }
    return true;
}

QVector<qint64> jsonShape(const QJsonValue &value)
{
    QVector<qint64> shape;
    const QJsonArray values = value.toArray();
    for (const QJsonValue &item : values) {
        bool ok = false;
        const qint64 dimension = item.toVariant().toLongLong(&ok);
        if (!ok || dimension < 0) {
            return {};
        }
        shape.append(dimension);
    }
    return shape;
}
} // namespace

TrainPage::TrainPage(QWidget *parent)
    : WorkflowPage(
        QStringLiteral("模型训练"),
        QStringLiteral("选择数据集版本，配置训练参数，并在 GPU 进程中运行 CNNTRANS 模型。"),
        parent,
        true)
{
    auto *context = new QLabel(QStringLiteral("使用数据集版本训练 CNNTRANS。训练输出按任务独立保存，完成后可直接进入识别。"), this);
    context->setObjectName(QStringLiteral("TrainingContext"));
    context->setProperty("muted", true);
    context->setWordWrap(true);
    contentLayout()->addWidget(context);

    auto *metricsContainer = new QWidget(this);
    auto *metrics = new QHBoxLayout(metricsContainer);
    metrics->setContentsMargins(0, 0, 0, 0);
    m_datasetMetric = new MetricCard(QStringLiteral("数据集版本"), metricsContainer);
    m_sampleMetric = new MetricCard(QStringLiteral("样本数量"), metricsContainer);
    m_trainStateMetric = new MetricCard(QStringLiteral("训练状态"), metricsContainer);
    auto *accuracyMetric = new MetricCard(QStringLiteral("最佳验证准确率"), metricsContainer);
    accuracyMetric->setObjectName(QStringLiteral("TrainingBestAccuracyMetric"));
    m_datasetMetric->setValue(QStringLiteral("未选择"));
    m_sampleMetric->setValue(QStringLiteral("-"));
    m_trainStateMetric->setValue(QStringLiteral("待选择"));
    accuracyMetric->setValue(QStringLiteral("-"));
    metrics->addWidget(m_datasetMetric);
    metrics->addWidget(m_sampleMetric);
    metrics->addWidget(m_trainStateMetric);
    metrics->addWidget(accuracyMetric);
    contentLayout()->addWidget(metricsContainer);

    m_gpuStatusLabel = new QLabel(QStringLiteral("GPU 状态：CUDA 环境将在启动训练时确认，当前配置不允许 CPU 回退。"), this);
    m_gpuStatusLabel->setObjectName(QStringLiteral("TrainingGpuStatusLabel"));
    m_gpuStatusLabel->setProperty("summary", true);
    m_gpuStatusLabel->setWordWrap(true);
    contentLayout()->addWidget(m_gpuStatusLabel);

    m_cacheStatusLabel = new QLabel(QStringLiteral("预计算缓存：当前数据集尚未检查。"), this);
    m_cacheStatusLabel->setObjectName(QStringLiteral("TrainingCacheStatusLabel"));
    m_cacheStatusLabel->setProperty("summary", true);
    m_cacheStatusLabel->setWordWrap(true);
    contentLayout()->addWidget(m_cacheStatusLabel);

    auto *parameters = new QGroupBox(QStringLiteral("推荐训练参数"), this);
    parameters->setObjectName(QStringLiteral("TrainingParametersGroup"));
    auto *parameterLayout = new QVBoxLayout(parameters);
    parameterLayout->setContentsMargins(12, 10, 12, 10);

    auto *recommendation = new QLabel(
        QStringLiteral("v5 推荐配置：8 × 4096 长窗口（32768 点）· complex_signed_mag · hierarchical · 6 轮 · Batch size 32 · GPU"),
        parameters);
    recommendation->setProperty("muted", true);
    recommendation->setWordWrap(true);
    parameterLayout->addWidget(recommendation);

    auto *basicPanel = new QWidget(parameters);
    auto *basicParameters = new QHBoxLayout(basicPanel);
    basicParameters->setContentsMargins(0, 0, 0, 0);

    m_epochsSpin = new QSpinBox();
    m_epochsSpin->setObjectName(QStringLiteral("TrainingEpochsSpin"));
    m_epochsSpin->setRange(1, 1000);
    m_epochsSpin->setValue(6);
    m_batchSizeSpin = new QSpinBox();
    m_batchSizeSpin->setObjectName(QStringLiteral("TrainingBatchSizeSpin"));
    m_batchSizeSpin->setRange(1, 4096);
    m_batchSizeSpin->setValue(32);
    m_trainDataModeCombo = new QComboBox();
    m_trainDataModeCombo->setObjectName(QStringLiteral("TrainingDataModeCombo"));
    m_trainDataModeCombo->addItem(QStringLiteral("训练时读取原始 IQ"), QStringLiteral("raw"));
    m_trainDataModeCombo->addItem(QStringLiteral("使用预计算时频缓存"), QStringLiteral("cache"));
    m_modelSizeCombo = new QComboBox();
    m_modelSizeCombo->setObjectName(QStringLiteral("TrainingModelSizeCombo"));
    m_modelSizeCombo->addItem(QStringLiteral("v5 推荐：Axis Profile XLarge"), QStringLiteral("axis_profile_xlarge"));
    m_modelSizeCombo->addItem(QStringLiteral("v5 标准：Base"), QStringLiteral("base"));
    m_modelSizeCombo->addItem(QStringLiteral("多尺度门控实验"), QStringLiteral("axis_profile_xlarge_multiscale_gated"));
    m_featureModeCombo = new QComboBox();
    m_featureModeCombo->setObjectName(QStringLiteral("TrainingFeatureModeCombo"));
    m_featureModeCombo->addItem(QStringLiteral("Legacy：I/Q 单边时频图"), QStringLiteral("legacy"));
    m_featureModeCombo->addItem(QStringLiteral("Complex signed magnitude：保留正负频谱"), QStringLiteral("complex_signed_mag"));
    m_featureModeCombo->setCurrentIndex(1);
    m_sampleLengthCombo = new QComboBox();
    m_sampleLengthCombo->setObjectName(QStringLiteral("TrainingSampleLengthCombo"));
    m_sampleLengthCombo->addItem(QStringLiteral("4096 × 1（4096 点）"), 4096);
    m_sampleLengthCombo->addItem(QStringLiteral("4096 × 2（8192 点）"), 8192);
    m_sampleLengthCombo->addItem(QStringLiteral("4096 × 4（16384 点）"), 16384);
    m_sampleLengthCombo->addItem(QStringLiteral("4096 × 8（32768 点，推荐）"), 32768);
    m_sampleLengthCombo->setCurrentIndex(3);
    m_modelModeCombo = new QComboBox();
    m_modelModeCombo->setObjectName(QStringLiteral("TrainingModelModeCombo"));
    m_modelModeCombo->addItem(QStringLiteral("闭集训练"), QStringLiteral("closed_set"));
    m_modelModeCombo->addItem(QStringLiteral("开集基线训练（未知类评估）"), QStringLiteral("open_set"));
    m_modelModeCombo->setToolTip(QStringLiteral("闭集用于已知类别识别；开集调用 v5 open_set_baseline，训练后评估未知类拒识能力，不能直接替代闭集识别模型。"));
    m_featureDescriptionLabel = new QLabel(this);
    m_featureDescriptionLabel->setObjectName(QStringLiteral("TrainingFeatureDescriptionLabel"));
    m_featureDescriptionLabel->setProperty("muted", true);
    m_featureDescriptionLabel->setWordWrap(true);
    m_deterministicCheck = new QCheckBox(QStringLiteral("可复现实验"));
    m_deterministicCheck->setObjectName(QStringLiteral("TrainingDeterministicCheck"));
    m_deterministicCheck->setToolTip(QStringLiteral("使用更严格的确定性设置，训练速度可能降低。"));
    m_precomputeBatchSpin = new QSpinBox();
    m_precomputeBatchSpin->setObjectName(QStringLiteral("TrainingPrecomputeBatchSpin"));
    m_precomputeBatchSpin->setRange(1, 65536);
    m_precomputeBatchSpin->setValue(4096);
    m_overwriteCacheCheck = new QCheckBox(QStringLiteral("覆盖已有缓存"));
    m_overwriteCacheCheck->setObjectName(QStringLiteral("TrainingOverwriteCacheCheck"));
    m_overwriteCacheCheck->setToolTip(QStringLiteral("默认复用已有 cache；只有特征参数变更或缓存损坏时才需要覆盖。"));

    const QList<QPair<QString, QWidget *>> basicFields = {
        {QStringLiteral("训练轮数"), m_epochsSpin},
        {QStringLiteral("Batch size"), m_batchSizeSpin},
        {QStringLiteral("样本长度"), m_sampleLengthCombo},
        {QStringLiteral("模型模式"), m_modelModeCombo},
    };
    for (const auto &field : basicFields) {
        auto *block = new QWidget(basicPanel);
        auto *blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->addWidget(new QLabel(field.first, block));
        blockLayout->addWidget(field.second);
        basicParameters->addWidget(block);
    }
    basicParameters->addStretch(1);
    parameterLayout->addWidget(basicPanel);

    m_advancedToggle = new QPushButton(QStringLiteral("展开专家设置"), parameters);
    m_advancedToggle->setObjectName(QStringLiteral("TrainingAdvancedToggle"));
    m_advancedToggle->setCheckable(true);
    m_advancedToggle->setProperty("secondary", true);
    parameterLayout->addWidget(m_advancedToggle, 0, Qt::AlignLeft);

    m_advancedTrainingPanel = new QWidget(parameters);
    m_advancedTrainingPanel->setObjectName(QStringLiteral("TrainingAdvancedPanel"));
    auto *advancedParameters = new QFormLayout(m_advancedTrainingPanel);
    advancedParameters->setContentsMargins(0, 0, 0, 0);
    advancedParameters->setHorizontalSpacing(14);
    advancedParameters->setVerticalSpacing(8);
    advancedParameters->addRow(QStringLiteral("数据模式"), m_trainDataModeCombo);
    advancedParameters->addRow(QStringLiteral("模型规模"), m_modelSizeCombo);
    advancedParameters->addRow(QStringLiteral("特征模式"), m_featureModeCombo);
    advancedParameters->addRow(QString(), m_featureDescriptionLabel);
    advancedParameters->addRow(QStringLiteral("缓存批量"), m_precomputeBatchSpin);
    advancedParameters->addRow(QString(), m_overwriteCacheCheck);
    advancedParameters->addRow(QString(), m_deterministicCheck);
    m_advancedTrainingPanel->setVisible(false);
    parameterLayout->addWidget(m_advancedTrainingPanel);
    contentLayout()->addWidget(parameters);

    auto *actions = new QHBoxLayout();
    auto *refreshButton = new QPushButton(QStringLiteral("刷新版本"), this);
    refreshButton->setObjectName(QStringLiteral("RefreshDatasetVersionsButton"));
    refreshButton->setProperty("secondary", true);
    auto *selectDatasetButton = new QPushButton(QStringLiteral("选择版本"), this);
    selectDatasetButton->setObjectName(QStringLiteral("SelectDatasetVersionButton"));
    selectDatasetButton->setProperty("secondary", true);
    m_startButton = new QPushButton(QStringLiteral("开始训练"), this);
    m_startButton->setObjectName(QStringLiteral("StartTrainingButton"));
    m_startButton->setProperty("primary", true);
    m_precomputeCacheButton = new QPushButton(QStringLiteral("生成预计算缓存"), this);
    m_precomputeCacheButton->setObjectName(QStringLiteral("PrecomputeTrainingCacheButton"));
    m_precomputeCacheButton->setProperty("secondary", true);
    m_cancelButton = new QPushButton(QStringLiteral("停止训练"), this);
    m_cancelButton->setObjectName(QStringLiteral("CancelTrainingButton"));
    m_cancelButton->setProperty("secondary", true);
    m_cancelButton->setEnabled(false);
    m_openOutputButton = new QPushButton(QStringLiteral("打开训练目录"), this);
    m_openOutputButton->setObjectName(QStringLiteral("ExportModelButton"));
    m_openOutputButton->setProperty("secondary", true);
    m_openOutputButton->setEnabled(false);
    m_useModelButton = new QPushButton(QStringLiteral("使用此模型进行识别"), this);
    m_useModelButton->setObjectName(QStringLiteral("UseModelForRecognitionButton"));
    m_useModelButton->setProperty("primary", true);
    m_useModelButton->setEnabled(false);
    actions->addWidget(refreshButton);
    actions->addWidget(selectDatasetButton);
    actions->addWidget(m_precomputeCacheButton);
    actions->addWidget(m_startButton);
    actions->addWidget(m_cancelButton);
    actions->addWidget(m_openOutputButton);
    actions->addWidget(m_useModelButton);
    actions->addStretch(1);
    contentLayout()->addLayout(actions);

    m_selectedSummary = new QLabel(QStringLiteral("尚未选择数据集版本。"), this);
    m_selectedSummary->setObjectName(QStringLiteral("TrainDatasetSummary"));
    m_selectedSummary->setWordWrap(true);
    contentLayout()->addWidget(m_selectedSummary);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("TrainingProgressBar"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    contentLayout()->addWidget(m_progressBar);
    m_progressDetail = new QLabel(QStringLiteral("等待选择数据集版本。"), this);
    m_progressDetail->setObjectName(QStringLiteral("TrainingProgressDetail"));
    m_progressDetail->setProperty("muted", true);
    m_progressDetail->setWordWrap(true);
    contentLayout()->addWidget(m_progressDetail);
    m_outputSummary = new QLabel(QStringLiteral("训练输出将保存到项目数据目录。"), this);
    m_outputSummary->setObjectName(QStringLiteral("TrainingOutputSummary"));
    m_outputSummary->setProperty("muted", true);
    m_outputSummary->setWordWrap(true);
    contentLayout()->addWidget(m_outputSummary);

    m_emptyState = new QLabel(QStringLiteral("暂无数据集版本。请先在数据集管理页选择类别并生成训练集版本。"), this);
    m_emptyState->setObjectName(QStringLiteral("EmptyState"));
    contentLayout()->addWidget(m_emptyState);

    m_versionTable = new QTableWidget(0, 6, this);
    m_versionTable->setObjectName(QStringLiteral("TrainDatasetVersionTable"));
    m_versionTable->setAlternatingRowColors(true);
    m_versionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_versionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_versionTable->setHorizontalHeaderLabels({
        QStringLiteral("版本"),
        QStringLiteral("类别"),
        QStringLiteral("样本数"),
        QStringLiteral("创建时间"),
        QStringLiteral("划分/策略"),
        QStringLiteral("清单"),
    });
    m_versionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_versionTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_versionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_versionTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_versionTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_versionTable->verticalHeader()->setVisible(false);
    contentLayout()->addWidget(m_versionTable, 1);

    auto *historyTitle = new QLabel(QStringLiteral("训练任务记录"));
    historyTitle->setObjectName(QStringLiteral("TrainingHistorySectionTitle"));
    contentLayout()->addWidget(historyTitle);
    m_trainingTable = new QTableWidget(0, 7, this);
    m_trainingTable->setObjectName(QStringLiteral("TrainingRunTable"));
    m_trainingTable->setAlternatingRowColors(true);
    m_trainingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_trainingTable->setHorizontalHeaderLabels({
        QStringLiteral("任务"),
        QStringLiteral("数据集版本"),
        QStringLiteral("状态"),
        QStringLiteral("轮数"),
        QStringLiteral("最佳准确率"),
        QStringLiteral("创建时间"),
        QStringLiteral("输出目录"),
    });
    m_trainingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_trainingTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_trainingTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_trainingTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_trainingTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_trainingTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_trainingTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_trainingTable->verticalHeader()->setVisible(false);
    m_trainingTable->setMinimumHeight(150);
    contentLayout()->addWidget(m_trainingTable);

    m_logs = new QPlainTextEdit(this);
    m_logs->setObjectName(QStringLiteral("TrainingLogOutput"));
    m_logs->setReadOnly(true);
    m_logs->setMaximumHeight(230);
    m_logs->setPlainText(QStringLiteral("当前阶段：等待选择数据集版本。"));
    contentLayout()->addWidget(m_logs);

    m_trainingRunner = new TrainingProcessRunner(this);
    connect(refreshButton, &QPushButton::clicked, this, &TrainPage::loadDatasetVersions);
    connect(selectDatasetButton, &QPushButton::clicked, this, &TrainPage::updateSelectedVersion);
    connect(m_precomputeCacheButton, &QPushButton::clicked, this, &TrainPage::startCachePrecompute);
    connect(m_startButton, &QPushButton::clicked, this, &TrainPage::startTraining);
    connect(m_cancelButton, &QPushButton::clicked, this, &TrainPage::cancelTraining);
    connect(m_advancedToggle, &QPushButton::toggled, this, [this](bool checked) {
        m_advancedTrainingPanel->setVisible(checked);
        m_advancedToggle->setText(checked ? QStringLiteral("收起专家设置") : QStringLiteral("展开专家设置"));
    });
    connect(m_featureModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                updateFeatureDescription();
                updateCacheStatus();
            });
    connect(m_trainDataModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                updateCacheStatus();
            });
    connect(m_sampleLengthCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                m_startButton->setToolTip(QStringLiteral("v5 会将样本长度转换为连续 4096 点窗口的拼接数。"));
                updateCacheStatus();
            });
    connect(m_versionTable, &QTableWidget::itemDoubleClicked, this, [this]() {
        updateSelectedVersion();
    });
    connect(m_trainingTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const int row = m_trainingTable ? m_trainingTable->currentRow() : -1;
        const bool valid = row >= 0 && row < m_trainingRuns.size()
            && QFileInfo::exists(m_trainingRuns.at(row).outputDirectory);
        if (m_openOutputButton) {
            m_openOutputButton->setEnabled(valid);
        }
    });
    connect(m_trainingRunner, &TrainingProcessRunner::progressChanged,
            this, &TrainPage::handleTrainingProgress);
    connect(m_trainingRunner, &TrainingProcessRunner::finished,
            this, &TrainPage::handleTrainingFinished);
    connect(m_trainingRunner, &TrainingProcessRunner::failed,
            this, &TrainPage::handleTrainingFailed);
    connect(m_openOutputButton, &QPushButton::clicked, this, [this]() {
        const int row = m_trainingTable ? m_trainingTable->currentRow() : -1;
        if (row < 0 || row >= m_trainingRuns.size()) {
            if (m_logs) {
                m_logs->appendPlainText(QStringLiteral("请先在训练任务记录中选择一行。"));
            }
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_trainingRuns.at(row).outputDirectory));
    });
    connect(m_useModelButton, &QPushButton::clicked, this, [this]() {
        if (!m_latestModelPath.isEmpty()) {
            emit modelReadyForRecognition(m_latestModelPath, m_latestModelDatasetVersionId);
        }
    });

    loadDatasetVersions();
    updateFeatureDescription();
}

void TrainPage::showEvent(QShowEvent *event)
{
    WorkflowPage::showEvent(event);
    loadDatasetVersions();
}

void TrainPage::loadDatasetVersions()
{
    AppStateStore store;
    const auto state = store.load();
    m_versions = state.has_value() ? state->datasetVersions : QVector<DatasetVersionRecord>();
    m_versionTable->setRowCount(0);
    for (int index = 0; index < m_versions.size(); ++index) {
        const DatasetVersionRecord &version = m_versions.at(index);
        const int row = m_versionTable->rowCount();
        m_versionTable->insertRow(row);
        auto *versionItem = readOnlyItem(version.versionId);
        versionItem->setData(Qt::UserRole, index);
        m_versionTable->setItem(row, 0, versionItem);
        m_versionTable->setItem(row, 1, readOnlyItem(version.selectedLabels.join(QStringLiteral("、"))));
        m_versionTable->setItem(row, 2, readOnlyItem(QString::number(version.sampleCount)));
        m_versionTable->setItem(row, 3, readOnlyItem(version.createdAt));
        m_versionTable->setItem(row, 4, readOnlyItem(version.sourceSummary.isEmpty() ? version.strategy : version.sourceSummary));
        m_versionTable->setItem(row, 5, readOnlyItem(version.manifestPath));
    }
    if (m_selectedVersionIndex < 0 && !m_versions.isEmpty()) {
        m_versionTable->selectRow(m_versionTable->rowCount() - 1);
        updateSelectedVersion();
    }
    loadTrainingRuns();

    const bool hasVersions = !m_versions.isEmpty();
    m_emptyState->setVisible(!hasVersions);
    m_versionTable->setVisible(hasVersions);
    if (!hasVersions) {
        m_selectedVersionIndex = -1;
        m_datasetMetric->setValue(QStringLiteral("未选择"));
        m_sampleMetric->setValue(QStringLiteral("-"));
        m_trainStateMetric->setValue(QStringLiteral("待数据集"));
        m_selectedSummary->setText(QStringLiteral("尚未选择数据集版本。"));
    } else if (m_selectedVersionIndex >= m_versions.size()) {
        m_selectedVersionIndex = -1;
        m_datasetMetric->setValue(QStringLiteral("未选择"));
        m_sampleMetric->setValue(QStringLiteral("-"));
        m_trainStateMetric->setValue(QStringLiteral("待选择"));
        m_selectedSummary->setText(QStringLiteral("请选择一个数据集版本后开始训练。"));
    }
    updateFeatureDescription();
    updateCacheStatus();
    setTrainingBusy(m_trainingRunner && m_trainingRunner->isRunning());
}

void TrainPage::loadTrainingRuns()
{
    AppStateStore store;
    const auto state = store.load();
    m_trainingRuns = state.has_value() ? state->trainingRuns : QVector<TrainingRunRecord>();
    m_trainingTable->setRowCount(0);
    for (const TrainingRunRecord &run : m_trainingRuns) {
        const int row = m_trainingTable->rowCount();
        m_trainingTable->insertRow(row);
        m_trainingTable->setItem(row, 0, readOnlyItem(run.runId));
        m_trainingTable->setItem(row, 1, readOnlyItem(run.datasetVersionId));
        m_trainingTable->setItem(row, 2, readOnlyItem(trainingStateText(run)));
        m_trainingTable->setItem(row, 3, readOnlyItem(QStringLiteral("%1 / %2").arg(run.epoch).arg(run.epochs)));
        m_trainingTable->setItem(row, 4, readOnlyItem(percentText(run.bestValAccuracy)));
        m_trainingTable->setItem(row, 5, readOnlyItem(run.createdAt));
        m_trainingTable->setItem(row, 6, readOnlyItem(run.outputDirectory));
    }
}

void TrainPage::updateSelectedVersion()
{
    const int row = m_versionTable ? m_versionTable->currentRow() : -1;
    if (row < 0 || !m_versionTable->item(row, 0)) {
        if (m_logs) {
            m_logs->appendPlainText(QStringLiteral("请先在版本表中选择一行。"));
        }
        return;
    }
    bool ok = false;
    const int index = m_versionTable->item(row, 0)->data(Qt::UserRole).toInt(&ok);
    if (!ok || index < 0 || index >= m_versions.size()) {
        return;
    }
    m_selectedVersionIndex = index;
    const DatasetVersionRecord &version = m_versions.at(index);
    m_datasetMetric->setValue(version.versionId);
    m_sampleMetric->setValue(QString::number(version.sampleCount));
    m_trainStateMetric->setValue(QStringLiteral("待训练"));
    m_selectedSummary->setText(QStringLiteral("已选择 %1，样本 %2 个，manifest：%3")
                                   .arg(version.versionId)
                                   .arg(version.sampleCount)
                                   .arg(QDir::toNativeSeparators(version.manifestPath)));
    if (!version.labelCounts.isEmpty()) {
        QStringList counts;
        for (auto it = version.labelCounts.constBegin(); it != version.labelCounts.constEnd(); ++it) {
            counts.append(QStringLiteral("%1：%2").arg(it.key()).arg(it.value()));
        }
        m_selectedSummary->setText(m_selectedSummary->text()
                                   + QStringLiteral("\n类别分布：%1").arg(counts.join(QStringLiteral("；"))));
    }
    m_progressBar->setValue(0);
    m_progressDetail->setText(QStringLiteral("已选择数据集版本，等待开始训练。"));
    updateCacheStatus();
    m_startButton->setEnabled(!m_trainingRunner->isRunning());
    if (m_logs) {
        m_logs->appendPlainText(QStringLiteral("已选择数据集版本：%1").arg(version.versionId));
        if (!QFileInfo::exists(version.manifestPath)) {
            m_logs->appendPlainText(QStringLiteral("警告：manifest 文件不存在，请回到数据集页重新生成版本。"));
        }
    }
}

QString TrainPage::cacheDirectoryForSelection() const
{
    if (m_selectedVersionIndex < 0 || m_selectedVersionIndex >= m_versions.size()) {
        return QString();
    }
    const QString versionId = m_versions.at(m_selectedVersionIndex).versionId;
    const QString featureMode = m_featureModeCombo
        ? m_featureModeCombo->currentData().toString()
        : QStringLiteral("legacy");
    const int sampleLength = m_sampleLengthCombo
        ? m_sampleLengthCombo->currentData().toInt()
        : 32768;
    const int stitchWindows = qMax(1, sampleLength / 4096);
    return QDir(ProjectPaths::trainingCacheDirectory())
        .filePath(QStringLiteral("%1/stitch%2/%3").arg(versionId).arg(stitchWindows).arg(featureMode));
}

bool TrainPage::cacheReadyForSelection(QString *reason) const
{
    const QString cacheDir = cacheDirectoryForSelection();
    const QString summaryPath = QDir(cacheDir).filePath(QStringLiteral("cache_summary.json"));
    if (!QFileInfo::exists(summaryPath)) {
        if (reason) {
            *reason = QStringLiteral("尚未生成 cache_summary.json");
        }
        return false;
    }
    QFile file(summaryPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (reason) {
            *reason = QStringLiteral("无法读取 cache_summary.json");
        }
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (!document.isObject()) {
        if (reason) {
            *reason = QStringLiteral("cache_summary.json 格式无效");
        }
        return false;
    }
    const QJsonObject summary = document.object();
    if (summary.value(QStringLiteral("status")).toString() != QStringLiteral("complete")) {
        if (reason) {
            *reason = QStringLiteral("缓存状态不是 complete，可能只生成了部分文件");
        }
        return false;
    }
    const QString expectedFeatureMode = m_featureModeCombo->currentData().toString();
    const QString actualFeatureMode = summary.value(QStringLiteral("feature_mode"))
                                          .toString(summary.value(QStringLiteral("feature_key")).toString());
    if (actualFeatureMode.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("缓存摘要缺少 feature_mode");
        }
        return false;
    }
    const int expectedStitchWindows = qMax(1, m_sampleLengthCombo->currentData().toInt() / 4096);
    const QString expectedFeatureId = expectedFeatureMode == QStringLiteral("legacy")
        ? QStringLiteral("legacy_iq_stft_2ch")
        : QStringLiteral("complex_signed_stft_signed_mag_2ch");
    if (actualFeatureMode != expectedFeatureMode && actualFeatureMode != expectedFeatureId) {
        if (reason) {
            *reason = QStringLiteral("缓存特征模式不匹配，当前选择为 %1")
                          .arg(expectedFeatureMode);
        }
        return false;
    }
    if (summary.value(QStringLiteral("stitch_windows")).toInt() != expectedStitchWindows) {
        if (reason) {
            *reason = QStringLiteral("缓存拼接长度不匹配，当前需要 4096 × %1")
                          .arg(expectedStitchWindows);
        }
        return false;
    }
    if (summary.value(QStringLiteral("npz_files")).toInt() <= 0) {
        if (reason) {
            *reason = QStringLiteral("缓存摘要中没有可用 NPZ 文件");
        }
        return false;
    }
    const QString datasetDirectory = m_versions.at(m_selectedVersionIndex).outputDirectory.isEmpty()
        ? QFileInfo(m_versions.at(m_selectedVersionIndex).manifestPath).absolutePath()
        : m_versions.at(m_selectedVersionIndex).outputDirectory;
    qint64 manifestRecords = 0;
    const QString currentManifestHash = manifestSha256(datasetDirectory, &manifestRecords);
    const QString cachedManifestHash = summary.value(QStringLiteral("manifest_sha256")).toString();
    if (currentManifestHash.isEmpty() || cachedManifestHash.isEmpty()
        || currentManifestHash != cachedManifestHash) {
        if (reason) {
            *reason = QStringLiteral("缓存对应的 manifest 已变化，必须重新生成 cache");
        }
        return false;
    }
    if (summary.value(QStringLiteral("manifest_records")).toVariant().toLongLong()
        != manifestRecords) {
        if (reason) {
            *reason = QStringLiteral("缓存记录数与当前数据集不一致，必须重新生成 cache");
        }
        return false;
    }
    const int expectedFileCount = summary.value(QStringLiteral("npz_files")).toInt();
    const QString specDirectory = QDir(cacheDir).filePath(QStringLiteral("spec_npy"));
    if (!QFileInfo(specDirectory).isDir()) {
        if (reason) {
            *reason = QStringLiteral("缓存缺少 spec_npy 目录");
        }
        return false;
    }
    int actualSpecFiles = 0;
    QDirIterator specIterator(specDirectory,
                              QStringList() << QStringLiteral("*.spec.npy"),
                              QDir::Files,
                              QDirIterator::Subdirectories);
    while (specIterator.hasNext()) {
        specIterator.next();
        ++actualSpecFiles;
    }
    if (actualSpecFiles < expectedFileCount) {
        if (reason) {
            *reason = QStringLiteral("缓存文件不完整：摘要 %1 个，实际 %2 个")
                          .arg(expectedFileCount)
                          .arg(actualSpecFiles);
        }
        return false;
    }
    const QJsonArray files = summary.value(QStringLiteral("files")).toArray();
    if (files.size() != expectedFileCount) {
        if (reason) {
            *reason = QStringLiteral("缓存摘要缺少逐文件契约：记录 %1 个，期望 %2 个")
                          .arg(files.size())
                          .arg(expectedFileCount);
        }
        return false;
    }
    for (const QJsonValue &value : files) {
        const QJsonObject contract = value.toObject();
        const QString sourcePath = contract.value(QStringLiteral("source_npz")).toString();
        const QString specPath = contract.value(QStringLiteral("spec_path")).toString();
        const QString indexPath = contract.value(QStringLiteral("index_path")).toString();
        const QString metadataPath = contract.value(QStringLiteral("metadata_path")).toString();
        if (sourcePath.isEmpty() || specPath.isEmpty() || indexPath.isEmpty() || metadataPath.isEmpty()) {
            if (reason) {
                *reason = QStringLiteral("缓存逐文件契约缺少路径字段");
            }
            return false;
        }
        const QFileInfo sourceInfo(sourcePath);
        if (!sourceInfo.isFile()
            || !QFileInfo::exists(specPath)
            || !QFileInfo::exists(indexPath)
            || !QFileInfo::exists(metadataPath)) {
            if (reason) {
                *reason = QStringLiteral("缓存文件不完整：%1").arg(QDir::toNativeSeparators(sourcePath));
            }
            return false;
        }
        const qint64 recordedMtimeNs = contract.value(QStringLiteral("source_mtime_ns"))
                                           .toVariant()
                                           .toLongLong();
        const qint64 currentMtimeNs = sourceInfo.lastModified().toMSecsSinceEpoch() * 1000000LL;
        if (contract.value(QStringLiteral("source_size")).toVariant().toLongLong() != sourceInfo.size()
            || recordedMtimeNs <= 0
            || qAbs(recordedMtimeNs - currentMtimeNs) > 1000000000LL) {
            if (reason) {
                *reason = QStringLiteral("源 NPZ 已变化或缺少时间戳：%1")
                              .arg(QDir::toNativeSeparators(sourcePath));
            }
            return false;
        }
        QFile metadataFile(metadataPath);
        if (!metadataFile.open(QIODevice::ReadOnly)) {
            if (reason) {
                *reason = QStringLiteral("无法读取缓存元数据：%1")
                              .arg(QDir::toNativeSeparators(metadataPath));
            }
            return false;
        }
        QJsonParseError metadataError;
        const QJsonDocument metadataDocument = QJsonDocument::fromJson(metadataFile.readAll(), &metadataError);
        const QJsonObject metadata = metadataDocument.object();
        if (!metadataDocument.isObject()
            || metadata.value(QStringLiteral("feature_mode")).toString() != actualFeatureMode
            || metadata.value(QStringLiteral("stitch_windows")).toInt() != expectedStitchWindows
            || metadata.value(QStringLiteral("source_size")).toVariant().toLongLong() != sourceInfo.size()
            || metadata.value(QStringLiteral("windows")).toInt()
                != contract.value(QStringLiteral("windows")).toInt()
            || metadata.value(QStringLiteral("dtype")).toString()
                != contract.value(QStringLiteral("dtype")).toString()
            || jsonShape(contract.value(QStringLiteral("shape"))).isEmpty()
            || !shapeMatches(
                jsonShape(contract.value(QStringLiteral("shape"))),
                metadata.value(QStringLiteral("shape")))) {
            if (reason) {
                *reason = QStringLiteral("缓存元数据与当前参数或源文件不匹配：%1")
                              .arg(QDir::toNativeSeparators(metadataPath));
            }
            return false;
        }
        NpyHeader specHeader;
        NpyHeader indexHeader;
        QString npyError;
        if (!readNpyHeader(specPath, &specHeader, &npyError)
            || !readNpyHeader(indexPath, &indexHeader, &npyError)) {
            if (reason) {
                *reason = npyError;
            }
            return false;
        }
        if (specHeader.dtype != QStringLiteral("<f2")
            || !shapeMatches(specHeader.shape, contract.value(QStringLiteral("shape")))) {
            if (reason) {
                *reason = QStringLiteral("时频缓存数组 dtype 或 shape 不匹配：%1")
                              .arg(QDir::toNativeSeparators(specPath));
            }
            return false;
        }
        if ((indexHeader.dtype != QStringLiteral("<u4")
                && indexHeader.dtype != QStringLiteral("|u4"))
            || indexHeader.shape.size() != 1
            || indexHeader.shape.first()
                != contract.value(QStringLiteral("windows")).toVariant().toLongLong()) {
            if (reason) {
                *reason = QStringLiteral("缓存索引数组 dtype、shape 或窗口数不匹配：%1")
                              .arg(QDir::toNativeSeparators(indexPath));
            }
            return false;
        }
    }
    return true;
}

void TrainPage::updateFeatureDescription()
{
    if (!m_featureDescriptionLabel || !m_featureModeCombo) {
        return;
    }
    const QString mode = m_featureModeCombo->currentData().toString();
    QString description;
    if (mode == QStringLiteral("legacy")) {
        description = QStringLiteral("legacy：I/Q 两路分别做 STFT，兼容当前稳定模型，显存和缓存体积较小。");
    } else {
        description = QStringLiteral("complex_signed_mag：保留复数 IQ 的正负频谱信息，适合 v5 长窗口指纹识别，当前为推荐模式。");
    }
    m_featureDescriptionLabel->setText(description);
}

void TrainPage::updateCacheStatus()
{
    if (!m_cacheStatusLabel) {
        return;
    }
    if (m_selectedVersionIndex < 0 || m_selectedVersionIndex >= m_versions.size()) {
        m_cacheStatusLabel->setText(QStringLiteral("预计算缓存：请先选择数据集版本。"));
        return;
    }
    QString reason;
    const QString cacheDir = cacheDirectoryForSelection();
    const bool ready = cacheReadyForSelection(&reason);
    m_cacheStatusLabel->setText(
        ready
            ? QStringLiteral("预计算缓存：已就绪（%1）\n%2")
                  .arg(m_featureModeCombo->currentText())
                  .arg(QDir::toNativeSeparators(cacheDir))
            : QStringLiteral("预计算缓存：未就绪（%1）\n点击“生成预计算缓存”，输出到：%2")
                  .arg(reason)
                  .arg(QDir::toNativeSeparators(cacheDir)));
}

void TrainPage::startCachePrecompute()
{
    if (m_trainingRunner->isRunning()) {
        return;
    }
    if (m_selectedVersionIndex < 0 || m_selectedVersionIndex >= m_versions.size()) {
        m_logs->appendPlainText(QStringLiteral("请先选择一个数据集版本。"));
        return;
    }
    const DatasetVersionRecord &version = m_versions.at(m_selectedVersionIndex);
    const QString runId = QStringLiteral("cache_%1")
                              .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    TrainingRunRequest request;
    request.runId = runId;
    request.datasetVersionId = version.versionId;
    request.datasetVersionDirectory = version.outputDirectory.isEmpty()
        ? QFileInfo(version.manifestPath).absolutePath()
        : version.outputDirectory;
    request.outputDirectory = createTrainingOutputDirectory(runId);
    request.trainDataMode = QStringLiteral("raw");
    request.featureMode = m_featureModeCombo->currentData().toString();
    request.cacheDirectory = cacheDirectoryForSelection();
    request.precomputeBatchSize = m_precomputeBatchSpin->value();
    request.overwriteCache = m_overwriteCacheCheck->isChecked();
    request.precomputeCacheOnly = true;
    request.sampleLength = m_sampleLengthCombo->currentData().toInt();
    request.stitchWindows = qMax(1, request.sampleLength / 4096);
    request.stitchStride = qMin(4, request.stitchWindows);
    // Cache is a shared frontend artifact; generate it with the closed-set
    // trainer contract regardless of the later model evaluation mode.
    request.modelMode = QStringLiteral("closed_set");

    setTrainingBusy(true);
    m_progressBar->setValue(0);
    m_trainStateMetric->setValue(QStringLiteral("生成缓存中"));
    m_progressDetail->setText(QStringLiteral("正在使用 CUDA 预计算时频缓存。"));
    m_outputSummary->setText(QStringLiteral("缓存目录：%1").arg(QDir::toNativeSeparators(request.cacheDirectory)));
    m_logs->clear();
    m_logs->appendPlainText(QStringLiteral("缓存任务：%1").arg(runId));
    m_logs->appendPlainText(QStringLiteral("特征：%1").arg(m_featureModeCombo->currentText()));
    if (!m_trainingRunner->start(request)) {
        setTrainingBusy(false);
        m_trainStateMetric->setValue(QStringLiteral("启动失败"));
        return;
    }
}

void TrainPage::startTraining()
{
    if (m_trainingRunner->isRunning()) {
        return;
    }
    if (m_selectedVersionIndex < 0 || m_selectedVersionIndex >= m_versions.size()) {
        if (m_logs) {
            m_logs->appendPlainText(QStringLiteral("请先选择一个数据集版本。"));
        }
        return;
    }
    const DatasetVersionRecord &version = m_versions.at(m_selectedVersionIndex);
    if (!QFileInfo::exists(version.manifestPath)) {
        m_trainStateMetric->setValue(QStringLiteral("清单缺失"));
        if (m_logs) {
            m_logs->appendPlainText(QStringLiteral("无法开始训练：manifest 不存在：%1")
                                        .arg(QDir::toNativeSeparators(version.manifestPath)));
        }
        return;
    }

    const QString runId = QStringLiteral("train_%1")
                              .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    TrainingRunRequest request;
    request.runId = runId;
    request.datasetVersionId = version.versionId;
    request.datasetVersionDirectory = version.outputDirectory.isEmpty()
        ? QFileInfo(version.manifestPath).absolutePath()
        : version.outputDirectory;
    request.outputDirectory = createTrainingOutputDirectory(runId);
    request.epochs = m_epochsSpin->value();
    request.batchSize = m_batchSizeSpin->value();
    request.trainDataMode = m_trainDataModeCombo->currentData().toString();
    request.modelSize = m_modelSizeCombo->currentData().toString();
    request.featureMode = m_featureModeCombo->currentData().toString();
    request.cacheDirectory = cacheDirectoryForSelection();
    request.precomputeBatchSize = m_precomputeBatchSpin->value();
    request.sampleLength = m_sampleLengthCombo->currentData().toInt();
    request.stitchWindows = qMax(1, request.sampleLength / 4096);
    request.stitchStride = qMin(4, request.stitchWindows);
    request.modelMode = m_modelModeCombo->currentData().toString();
    request.deterministic = m_deterministicCheck->isChecked();
    if (request.sampleLength < 4096 || request.sampleLength % 4096 != 0) {
        m_trainStateMetric->setValue(QStringLiteral("长度未支持"));
        m_logs->appendPlainText(QStringLiteral("样本长度必须是 4096 × n，当前值为 %1。").arg(request.sampleLength));
        return;
    }
    if (request.modelMode != QStringLiteral("closed_set")) {
        request.openSetAction = QStringLiteral("train");
        m_logs->appendPlainText(QStringLiteral("将启动独立 open-set 基线训练；输出用于未知类评估，不会覆盖闭集识别模型。"));
    }
    if (request.trainDataMode == QStringLiteral("cache")) {
        QString reason;
        if (!cacheReadyForSelection(&reason)) {
            m_trainStateMetric->setValue(QStringLiteral("缓存未就绪"));
            m_logs->appendPlainText(QStringLiteral("无法使用 cache 训练：%1。请先生成预计算缓存。").arg(reason));
            updateCacheStatus();
            return;
        }
    }

    setTrainingBusy(true);
    m_progressBar->setValue(0);
    m_progressDetail->setText(QStringLiteral("正在启动训练后端。"));
    m_outputSummary->setText(QStringLiteral("本次训练输出：%1").arg(QDir::toNativeSeparators(request.outputDirectory)));
    m_trainStateMetric->setValue(QStringLiteral("准备中"));
    m_logs->clear();
    m_logs->appendPlainText(QStringLiteral("任务：%1").arg(runId));
    m_logs->appendPlainText(QStringLiteral("数据集：%1").arg(version.versionId));
    m_logs->appendPlainText(QStringLiteral("后端：%1").arg(TrainingProcessRunner::backendScriptPath()));
    if (!m_trainingRunner->start(request)) {
        setTrainingBusy(false);
        m_trainStateMetric->setValue(QStringLiteral("启动失败"));
        return;
    }
}

void TrainPage::cancelTraining()
{
    if (m_trainingRunner) {
        m_trainingRunner->cancel();
    }
}

void TrainPage::handleTrainingProgress(const TrainingProgress &progress)
{
    if (!m_progressBar || !m_progressDetail) {
        return;
    }
    m_progressBar->setValue(progress.percent);
    m_trainStateMetric->setValue(progress.phase.isEmpty() ? QStringLiteral("训练中") : progress.phase);
    m_progressDetail->setText(QStringLiteral("%1\n第 %2 / %3 轮；训练窗口 %4；验证窗口 %5；训练准确率 %6；验证准确率 %7；最佳 %8")
                                  .arg(progress.message)
                                  .arg(progress.epoch)
                                  .arg(progress.epochs)
                                  .arg(progress.trainWindows)
                                  .arg(progress.valWindows)
                                  .arg(percentText(progress.trainAccuracy))
                                  .arg(percentText(progress.valAccuracy))
                                  .arg(percentText(progress.bestAccuracy)));
}

void TrainPage::handleTrainingFinished(const TrainingRunResult &result)
{
    setTrainingBusy(false);
    m_progressBar->setValue(result.success ? 100 : m_progressBar->value());
    m_trainStateMetric->setValue(trainingTaskStateToString(result.state));
    m_progressDetail->setText(result.message);
    if (result.precomputeCacheOnly) {
        m_outputSummary->setText(QStringLiteral("缓存输出：%1\n摘要：%2")
                                     .arg(QDir::toNativeSeparators(result.cacheDirectory))
                                     .arg(QDir::toNativeSeparators(result.cacheSummaryPath)));
    } else {
        m_outputSummary->setText(QStringLiteral("训练输出：%1\n最佳模型：%2\n最终模型：%3")
                                     .arg(QDir::toNativeSeparators(result.outputDirectory))
                                     .arg(QDir::toNativeSeparators(result.bestModelPath))
                                     .arg(QDir::toNativeSeparators(result.finalModelPath)));
    }
    if (auto *accuracy = findChild<MetricCard *>(QStringLiteral("TrainingBestAccuracyMetric"))) {
        accuracy->setValue(percentText(result.bestValAccuracy));
    }
    if (!result.precomputeCacheOnly
        && result.success
        && result.modelMode == QStringLiteral("closed_set")) {
        m_latestModelPath = result.bestModelPath;
        m_latestModelDatasetVersionId = result.datasetVersionId;
    }
    if (m_gpuStatusLabel) {
        m_gpuStatusLabel->setText(QStringLiteral("GPU 状态：%1")
                                  .arg(result.gpuDeviceName.isEmpty() ? QStringLiteral("CUDA GPU 已完成训练")
                                                                      : result.gpuDeviceName));
    }
    if (m_openOutputButton) {
        m_openOutputButton->setEnabled(QFileInfo::exists(result.outputDirectory));
    }
    const bool closedSetModel = result.modelMode == QStringLiteral("closed_set");
    if (m_useModelButton) {
        m_useModelButton->setEnabled(!result.precomputeCacheOnly && result.success
                                     && result.modelAvailable && closedSetModel);
    }
    for (const QString &line : result.logs) {
        if (m_logs) {
            m_logs->appendPlainText(line);
        }
    }
    if (m_logs) {
        m_logs->appendPlainText(result.message);
        if (result.success && !result.precomputeCacheOnly && !closedSetModel) {
            m_logs->appendPlainText(
                QStringLiteral("开集训练结果已保留为评估输出，不会自动进入闭集识别页。"));
        }
    }
    updateCacheStatus();
    if (!result.precomputeCacheOnly) {
        persistTrainingRun(result);
    }
}

void TrainPage::handleTrainingFailed(const QString &message)
{
    m_trainStateMetric->setValue(QStringLiteral("启动失败"));
    m_progressDetail->setText(message);
    if (m_logs) {
        m_logs->appendPlainText(message);
    }
}

void TrainPage::setTrainingBusy(bool busy)
{
    m_startButton->setEnabled(!busy && m_selectedVersionIndex >= 0
                              && m_selectedVersionIndex < m_versions.size());
    m_cancelButton->setEnabled(busy);
    m_versionTable->setEnabled(!busy);
    m_epochsSpin->setEnabled(!busy);
    m_batchSizeSpin->setEnabled(!busy);
    m_trainDataModeCombo->setEnabled(!busy);
    m_modelSizeCombo->setEnabled(!busy);
    m_featureModeCombo->setEnabled(!busy);
    m_sampleLengthCombo->setEnabled(!busy);
    m_modelModeCombo->setEnabled(!busy);
    m_precomputeCacheButton->setEnabled(!busy && m_selectedVersionIndex >= 0
                                        && m_selectedVersionIndex < m_versions.size());
    m_precomputeBatchSpin->setEnabled(!busy);
    m_overwriteCacheCheck->setEnabled(!busy);
    m_deterministicCheck->setEnabled(!busy);
    if (m_advancedToggle) {
        m_advancedToggle->setEnabled(!busy);
    }
}

QString TrainPage::createTrainingOutputDirectory(const QString &runId) const
{
    const QString directory = QDir(ProjectPaths::exportsDirectory())
                                  .filePath(QStringLiteral("training_runs/%1").arg(runId));
    QDir().mkpath(directory);
    return directory;
}

void TrainPage::persistTrainingRun(const TrainingRunResult &result)
{
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    TrainingRunRecord run;
    run.runId = result.runId;
    run.datasetVersionId = result.datasetVersionId;
    for (const DatasetVersionRecord &version : m_versions) {
        if (version.versionId == result.datasetVersionId) {
            run.datasetVersionDirectory = version.outputDirectory;
            break;
        }
    }
    run.outputDirectory = result.outputDirectory;
    run.state = trainingTaskStateToString(result.state);
    run.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    run.completedAt = run.createdAt;
    run.summaryPath = result.summaryPath;
    run.bestModelPath = result.bestModelPath;
    run.finalModelPath = result.finalModelPath;
    run.message = result.message;
    run.epoch = result.epoch;
    run.epochs = result.epochs;
    run.trainWindows = result.trainWindows;
    run.valWindows = result.valWindows;
    run.bestValAccuracy = result.bestValAccuracy;
    run.finalValAccuracy = result.finalValAccuracy;
    run.cancelled = result.cancelled;
    run.partial = result.partial;
    run.timedOut = result.timedOut;
    run.requireCuda = true;
    run.gpuDeviceName = result.gpuDeviceName;
    run.device = result.device;
    run.pipelineVersion = result.pipelineVersion;
    run.featureMode = result.featureMode;
    run.modelSize = result.modelSize;
    run.cacheDirectory = result.cacheDirectory;
    run.cacheSummaryPath = result.cacheSummaryPath;
    run.sampleLength = result.sampleLength;
    run.modelMode = result.modelMode;
    run.modelAvailable = result.success
        && result.modelMode == QStringLiteral("closed_set")
        && result.modelAvailable;
    run.splitSummaryPath = result.splitSummaryPath;

    bool replaced = false;
    for (TrainingRunRecord &existing : state.trainingRuns) {
        if (existing.runId == run.runId) {
            existing = run;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        state.trainingRuns.append(run);
    }
    if (result.success && result.modelAvailable
        && result.modelMode == QStringLiteral("closed_set")) {
        state.activeTrainingRunId = run.runId;
    }
    if (!store.save(state) && m_logs) {
        m_logs->appendPlainText(QStringLiteral("训练完成，但训练记录保存失败：%1").arg(store.lastError()));
    }
    m_trainingRuns = state.trainingRuns;
    loadTrainingRuns();
    if (result.success && result.modelAvailable
        && result.modelMode == QStringLiteral("closed_set")) {
        emit modelReadyForRecognition(result.bestModelPath, result.datasetVersionId);
    }
}
