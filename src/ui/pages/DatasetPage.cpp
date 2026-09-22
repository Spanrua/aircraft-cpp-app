#include "ui/pages/DatasetPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "ui/widgets/IllustrationPanel.h"
#include "ui/widgets/MetricCard.h"
#include "ui/widgets/SpectrogramReviewWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
struct CategorySummary
{
    int total = 0;
    int available = 0;
    int partial = 0;
    int pending = 0;
};

const QStringList kTrainingLabels = {
    QStringLiteral("air2_01"),
    QStringLiteral("air2_02"),
    QStringLiteral("air2s_01"),
    QStringLiteral("air2s_02"),
    QStringLiteral("air3_01"),
    QStringLiteral("air3_02"),
    QStringLiteral("air3s_01"),
    QStringLiteral("air3s_02"),
    QStringLiteral("mini3pro_01"),
    QStringLiteral("mini3pro_02"),
    QStringLiteral("mini4pro_01"),
    QStringLiteral("mini4pro_02"),
    QStringLiteral("mini5pro_01"),
    QStringLiteral("mini5pro_02"),
    QStringLiteral("yu4pro_01"),
    QStringLiteral("yu4pro_02"),
};

QString normalizedLabelToken(const QString &text)
{
    QString token;
    for (const QChar character : text) {
        if (character.isLetterOrNumber()) {
            token.append(character.toLower());
        }
    }
    return token;
}

QString trainingLabelFromText(const QString &text)
{
    const QString token = normalizedLabelToken(text);
    if (token.isEmpty()) {
        return QString();
    }
    for (const QString &trainingLabel : kTrainingLabels) {
        if (normalizedLabelToken(trainingLabel) == token) {
            return trainingLabel;
        }
    }
    return QString();
}

QString sampleLabel(const SampleRecord &sample)
{
    const QString rawLabel = sample.labelType.trimmed();
    if (!rawLabel.isEmpty()) {
        const QString explicitTrainingLabel = trainingLabelFromText(rawLabel);
        if (!explicitTrainingLabel.isEmpty()) {
            return explicitTrainingLabel;
        }
        // Older preprocess output uses labels such as B_Yu4Pro; resolve those
        // through the capture path, while preserving an intentional custom label.
        if (!rawLabel.startsWith(QStringLiteral("B_"), Qt::CaseInsensitive)) {
            return rawLabel;
        }
    }

    const QStringList pathCandidates = {
        sample.rawFilePath.trimmed(),
        sample.sampleFilePath.trimmed(),
    };
    for (const QString &candidate : pathCandidates) {
        const QString token = normalizedLabelToken(candidate);
        for (const QString &trainingLabel : kTrainingLabels) {
            if (token.contains(normalizedLabelToken(trainingLabel))) {
                return trainingLabel;
            }
        }
    }
    return rawLabel.isEmpty() ? QStringLiteral("未分类") : rawLabel;
}

QString processingState(const SampleRecord &sample)
{
    return sample.processingState.trimmed().isEmpty()
        ? QStringLiteral("已完成")
        : sample.processingState.trimmed();
}

QString qualityState(const SampleRecord &sample)
{
    return sample.qualityStatus.trimmed().isEmpty()
        ? sample.status
        : sample.qualityStatus;
}

QString splitGroupKey(const SampleRecord &sample)
{
    const QString source = sample.rawFilePath.trimmed().isEmpty()
        ? sample.sampleFilePath.trimmed()
        : sample.rawFilePath.trimmed();
    return sampleLabel(sample) + QChar(0x1f) + source + QChar(0x1f) + sample.batchId;
}

QString splitForGroupIndex(int index, int total)
{
    if (total <= 1) {
        return QStringLiteral("train");
    }
    const int trainCount = qMax(1, static_cast<int>(std::round(total * 0.8)));
    const int validationCount = total >= 3 ? qMax(1, static_cast<int>(std::round(total * 0.1))) : 0;
    if (index < qMin(trainCount, total)) {
        return QStringLiteral("train");
    }
    if (index < qMin(trainCount + validationCount, total)) {
        return QStringLiteral("val");
    }
    return QStringLiteral("test");
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
} // namespace

DatasetPage::DatasetPage(QWidget *parent)
    : WorkflowPage(
        QStringLiteral("数据集管理"),
        QStringLiteral("按类别挑选样本，生成供训练使用的数据集版本。"),
        parent,
        true)
{
    auto *context = new QLabel(QStringLiteral("先选择要参加本次训练的类别，再复核异常样本。生成版本只读保存，不会覆盖历史数据。"), this);
    context->setObjectName(QStringLiteral("DatasetContext"));
    context->setProperty("muted", true);
    context->setWordWrap(true);
    contentLayout()->addWidget(context);

    auto *metricsContainer = new QWidget();
    auto *metrics = new QHBoxLayout(metricsContainer);
    metrics->setContentsMargins(0, 0, 0, 0);
    m_totalSamplesMetric = new MetricCard(QStringLiteral("样本总数"), metricsContainer);
    m_totalSamplesMetric->setObjectName(QStringLiteral("DatasetTotalSamplesMetric"));
    m_selectedSamplesMetric = new MetricCard(QStringLiteral("已选训练样本"), metricsContainer);
    m_selectedSamplesMetric->setObjectName(QStringLiteral("DatasetSelectedSamplesMetric"));
    m_selectedCategoriesMetric = new MetricCard(QStringLiteral("已选类别"), metricsContainer);
    m_selectedCategoriesMetric->setObjectName(QStringLiteral("DatasetSelectedCategoriesMetric"));
    m_versionCountMetric = new MetricCard(QStringLiteral("数据集版本"), metricsContainer);
    m_versionCountMetric->setObjectName(QStringLiteral("DatasetVersionCountMetric"));
    metrics->addWidget(m_totalSamplesMetric);
    metrics->addWidget(m_selectedSamplesMetric);
    metrics->addWidget(m_selectedCategoriesMetric);
    metrics->addWidget(m_versionCountMetric);
    contentLayout()->addWidget(metricsContainer);

    auto *selectionPanel = new QWidget();
    selectionPanel->setObjectName(QStringLiteral("DatasetCategorySelectionPanel"));
    auto *selectionLayout = new QVBoxLayout(selectionPanel);
    selectionLayout->setContentsMargins(14, 12, 14, 12);
    selectionLayout->setSpacing(8);

    auto *selectionHeader = new QHBoxLayout();
    auto *selectionTitle = new QLabel(QStringLiteral("选择训练类别"), selectionPanel);
    selectionTitle->setObjectName(QStringLiteral("DatasetCategorySelectionTitle"));
    selectionHeader->addWidget(selectionTitle);
    selectionHeader->addStretch(1);

    auto *selectAllButton = new QPushButton(QStringLiteral("全选类别"), selectionPanel);
    selectAllButton->setObjectName(QStringLiteral("DatasetSelectAllCategoriesButton"));
    selectAllButton->setProperty("secondary", true);
    auto *clearButton = new QPushButton(QStringLiteral("清空选择"), selectionPanel);
    clearButton->setObjectName(QStringLiteral("DatasetClearCategoriesButton"));
    clearButton->setProperty("secondary", true);
    selectionHeader->addWidget(selectAllButton);
    selectionHeader->addWidget(clearButton);
    selectionLayout->addLayout(selectionHeader);

    m_categoryTable = new QTableWidget(0, 6, selectionPanel);
    m_categoryTable->setObjectName(QStringLiteral("DatasetCategoryTable"));
    m_categoryTable->setHorizontalHeaderLabels({
        QStringLiteral("纳入"),
        QStringLiteral("类别"),
        QStringLiteral("总样本"),
        QStringLiteral("可用样本"),
        QStringLiteral("部分完成"),
        QStringLiteral("当前纳入"),
    });
    m_categoryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_categoryTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_categoryTable->setAlternatingRowColors(true);
    m_categoryTable->verticalHeader()->setVisible(false);
    m_categoryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_categoryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column = 2; column < m_categoryTable->columnCount(); ++column) {
        m_categoryTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_categoryTable->setMinimumHeight(150);
    selectionLayout->addWidget(m_categoryTable);

    auto *selectionFooter = new QHBoxLayout();
    m_selectionSummaryLabel = new QLabel(selectionPanel);
    m_selectionSummaryLabel->setObjectName(QStringLiteral("DatasetSelectionSummary"));
    m_selectionSummaryLabel->setProperty("muted", true);
    selectionFooter->addWidget(m_selectionSummaryLabel, 1);
    m_includePartialCheck = new QCheckBox(QStringLiteral("包含部分完成样本"), selectionPanel);
    m_includePartialCheck->setObjectName(QStringLiteral("DatasetIncludePartialCheck"));
    m_includePartialCheck->setToolTip(QStringLiteral("默认排除取消、超时或失败任务留下的部分样本。"));
    selectionFooter->addWidget(m_includePartialCheck);
    m_includePendingReviewCheck = new QCheckBox(QStringLiteral("包含待复核样本"), selectionPanel);
    m_includePendingReviewCheck->setObjectName(QStringLiteral("DatasetIncludePendingReviewCheck"));
    m_includePendingReviewCheck->setToolTip(QStringLiteral("默认只纳入复核通过的样本；勾选后待复核样本也可进入训练集。"));
    selectionFooter->addWidget(m_includePendingReviewCheck);
    m_generateVersionButton = new QPushButton(QStringLiteral("生成训练集版本"), selectionPanel);
    m_generateVersionButton->setObjectName(QStringLiteral("GenerateDatasetVersionButton"));
    m_generateVersionButton->setProperty("primary", true);
    selectionFooter->addWidget(m_generateVersionButton);
    m_nextTrainingButton = new QPushButton(QStringLiteral("去模型训练"), selectionPanel);
    m_nextTrainingButton->setObjectName(QStringLiteral("GoToTrainingButton"));
    m_nextTrainingButton->setProperty("secondary", true);
    m_nextTrainingButton->setEnabled(false);
    selectionFooter->addWidget(m_nextTrainingButton);
    selectionLayout->addLayout(selectionFooter);
    contentLayout()->addWidget(selectionPanel);

    m_readinessLabel = new QLabel(QStringLiteral("训练准备度：等待选择类别。"), selectionPanel);
    m_readinessLabel->setObjectName(QStringLiteral("DatasetReadinessSummary"));
    m_readinessLabel->setWordWrap(true);
    m_readinessLabel->setProperty("summary", true);
    selectionLayout->addWidget(m_readinessLabel);

    auto *actionsContainer = new QWidget();
    auto *actions = new QHBoxLayout(actionsContainer);
    actions->setContentsMargins(0, 0, 0, 0);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新样本"), actionsContainer);
    refreshButton->setObjectName(QStringLiteral("RefreshDatasetSamplesButton"));
    refreshButton->setProperty("secondary", true);
    actions->addWidget(refreshButton);
    m_stateFilter = new QComboBox(actionsContainer);
    m_stateFilter->setObjectName(QStringLiteral("DatasetSampleStateFilter"));
    m_stateFilter->addItem(QStringLiteral("全部候选"), QStringLiteral("all"));
    m_stateFilter->addItem(QStringLiteral("待复核"), QStringLiteral("待复核"));
    m_stateFilter->addItem(QStringLiteral("规则建议通过"), QStringLiteral("规则建议通过"));
    m_stateFilter->addItem(QStringLiteral("规则建议剔除"), QStringLiteral("规则建议剔除"));
    m_stateFilter->addItem(QStringLiteral("复核通过"), QStringLiteral("复核通过"));
    m_stateFilter->addItem(QStringLiteral("已剔除"), QStringLiteral("已剔除"));
    m_stateFilter->setToolTip(QStringLiteral("按候选事件状态或规则建议筛选，连续窗口会合并显示。"));
    actions->addWidget(m_stateFilter);
    m_ruleTriageButton = new QPushButton(QStringLiteral("应用高置信规则建议"), actionsContainer);
    m_ruleTriageButton->setObjectName(QStringLiteral("DatasetApplyRuleTriageButton"));
    m_ruleTriageButton->setProperty("secondary", true);
    m_ruleTriageButton->setToolTip(QStringLiteral("只应用严格规则通过建议；未经过负样本校准的边界样本仍需人工复核。"));
    actions->addWidget(m_ruleTriageButton);
    actions->addStretch(1);
    contentLayout()->addWidget(actionsContainer);

    auto *reviewPanel = new QWidget();
    reviewPanel->setObjectName(QStringLiteral("DatasetReviewPanel"));
    auto *reviewLayout = new QVBoxLayout(reviewPanel);
    reviewLayout->setContentsMargins(14, 12, 14, 12);
    reviewLayout->setSpacing(8);
    auto *reviewHeader = new QHBoxLayout();
    auto *reviewTitle = new QLabel(QStringLiteral("候选事件复核与类别修正"), reviewPanel);
    reviewTitle->setObjectName(QStringLiteral("DatasetReviewTitle"));
    reviewHeader->addWidget(reviewTitle);
    reviewHeader->addStretch(1);
    m_reviewSelectionLabel = new QLabel(QStringLiteral("请选择样本"), reviewPanel);
    m_reviewSelectionLabel->setObjectName(QStringLiteral("DatasetReviewSelectionLabel"));
    m_reviewSelectionLabel->setProperty("muted", true);
    reviewHeader->addWidget(m_reviewSelectionLabel);
    reviewLayout->addLayout(reviewHeader);

    auto *reviewActions = new QHBoxLayout();
    m_labelCombo = new QComboBox(reviewPanel);
    m_labelCombo->setObjectName(QStringLiteral("DatasetReviewLabelCombo"));
    m_labelCombo->addItem(QStringLiteral("选择标准类别"), QString());
    for (const QString &label : kTrainingLabels) {
        m_labelCombo->addItem(label, label);
    }
    m_applyLabelButton = new QPushButton(QStringLiteral("应用类别"), reviewPanel);
    m_applyLabelButton->setObjectName(QStringLiteral("DatasetApplyLabelButton"));
    m_applyLabelButton->setProperty("secondary", true);
    m_approveButton = new QPushButton(QStringLiteral("复核通过"), reviewPanel);
    m_approveButton->setObjectName(QStringLiteral("DatasetMarkApprovedButton"));
    m_approveButton->setProperty("primary", true);
    m_rejectButton = new QPushButton(QStringLiteral("剔除样本"), reviewPanel);
    m_rejectButton->setObjectName(QStringLiteral("DatasetMarkRejectedButton"));
    m_rejectButton->setProperty("secondary", true);
    m_pendingButton = new QPushButton(QStringLiteral("设为待复核"), reviewPanel);
    m_pendingButton->setObjectName(QStringLiteral("DatasetMarkPendingButton"));
    m_pendingButton->setProperty("secondary", true);
    reviewActions->addWidget(m_labelCombo, 1);
    reviewActions->addWidget(m_applyLabelButton);
    reviewActions->addWidget(m_approveButton);
    reviewActions->addWidget(m_rejectButton);
    reviewActions->addWidget(m_pendingButton);
    reviewLayout->addLayout(reviewActions);

    auto *spectrogramTitle = new QLabel(QStringLiteral("候选框选时频图（专家复核）"), reviewPanel);
    spectrogramTitle->setObjectName(QStringLiteral("DatasetSpectrogramTitle"));
    reviewLayout->addWidget(spectrogramTitle);
    m_spectrogramReviewWidget = new SpectrogramReviewWidget(reviewPanel);
    reviewLayout->addWidget(m_spectrogramReviewWidget);
    m_spectrogramPathLabel = new QLabel(QStringLiteral("选择候选事件后加载候选总览图。"), reviewPanel);
    m_spectrogramPathLabel->setObjectName(QStringLiteral("DatasetSpectrogramPathLabel"));
    m_spectrogramPathLabel->setProperty("muted", true);
    m_spectrogramPathLabel->setWordWrap(true);
    reviewLayout->addWidget(m_spectrogramPathLabel);
    contentLayout()->addWidget(reviewPanel);

    m_pageMessage = new QLabel();
    m_pageMessage->setObjectName(QStringLiteral("DatasetPageMessage"));
    m_pageMessage->setWordWrap(true);
    m_pageMessage->setVisible(false);
    contentLayout()->addWidget(m_pageMessage);

    m_emptyState = new QLabel(QStringLiteral("暂无样本。请先在预处理页生成有效样本。"));
    m_emptyState->setObjectName(QStringLiteral("EmptyState"));
    contentLayout()->addWidget(m_emptyState);

    m_table = new QTableWidget(0, 9);
    m_table->setObjectName(QStringLiteral("DatasetSampleTable"));
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("候选事件"),
        QStringLiteral("类别"),
        QStringLiteral("窗口数"),
        QStringLiteral("规则分"),
        QStringLiteral("平均SNR"),
        QStringLiteral("占用率"),
        QStringLiteral("规则建议"),
        QStringLiteral("复核状态"),
        QStringLiteral("来源文件"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(6, 120);
    m_table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setMinimumHeight(240);
    contentLayout()->addWidget(m_table);

    m_sampleDetailLabel = new QLabel(QStringLiteral("选择样本后显示完整路径、时频图和指标。"), this);
    m_sampleDetailLabel->setObjectName(QStringLiteral("DatasetSampleDetail"));
    m_sampleDetailLabel->setWordWrap(true);
    m_sampleDetailLabel->setProperty("muted", true);
    contentLayout()->addWidget(m_sampleDetailLabel);

    auto *versionTitle = new QLabel(QStringLiteral("已生成的数据集版本"));
    versionTitle->setObjectName(QStringLiteral("DatasetVersionSectionTitle"));
    contentLayout()->addWidget(versionTitle);

    m_versionTable = new QTableWidget(0, 6);
    m_versionTable->setObjectName(QStringLiteral("DatasetVersionTable"));
    m_versionTable->setHorizontalHeaderLabels({
        QStringLiteral("版本"),
        QStringLiteral("类别"),
        QStringLiteral("样本数"),
        QStringLiteral("创建时间"),
        QStringLiteral("清单"),
        QStringLiteral("部分完成"),
    });
    m_versionTable->setAlternatingRowColors(true);
    m_versionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_versionTable->verticalHeader()->setVisible(false);
    m_versionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column = 2; column < m_versionTable->columnCount(); ++column) {
        m_versionTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_versionTable->setMinimumHeight(130);
    contentLayout()->addWidget(m_versionTable);

    Q_UNUSED(refreshButton)
    Q_UNUSED(selectAllButton)
    Q_UNUSED(clearButton)
    connect(refreshButton, &QPushButton::clicked, this, &DatasetPage::loadSamples);
    connect(selectAllButton, &QPushButton::clicked, this, [this]() {
        setAllCategoryChecks(Qt::Checked);
    });
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        setAllCategoryChecks(Qt::Unchecked);
    });
    connect(m_includePartialCheck, &QCheckBox::toggled, this, [this]() {
        updateSelectionSummary();
        rebuildCategoryTable();
    });
    connect(m_includePendingReviewCheck, &QCheckBox::toggled, this, [this]() {
        updateSelectionSummary();
        rebuildCategoryTable();
    });
    connect(m_generateVersionButton, &QPushButton::clicked, this, &DatasetPage::generateDatasetVersion);
    connect(m_nextTrainingButton, &QPushButton::clicked, this, &DatasetPage::requestOpenTraining);
    connect(m_stateFilter, &QComboBox::currentTextChanged, this, [this]() {
        loadSamples();
    });
    connect(m_ruleTriageButton, &QPushButton::clicked, this, &DatasetPage::applyRuleTriage);
    connect(m_categoryTable, &QTableWidget::itemChanged, this, &DatasetPage::handleCategoryItemChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &DatasetPage::updateReviewPanel);
    connect(m_approveButton, &QPushButton::clicked, this, [this]() {
        applyReviewToSelection(QStringLiteral("复核通过"));
    });
    connect(m_rejectButton, &QPushButton::clicked, this, [this]() {
        applyReviewToSelection(QStringLiteral("已剔除"));
    });
    connect(m_pendingButton, &QPushButton::clicked, this, [this]() {
        applyReviewToSelection(QStringLiteral("待复核"));
    });
    connect(m_applyLabelButton, &QPushButton::clicked, this, &DatasetPage::applyLabelToSelection);
}

bool DatasetPage::hasSamples() const
{
    return !m_samples.isEmpty();
}

bool DatasetPage::hasDatasetVersion() const
{
    return !m_versions.isEmpty();
}

void DatasetPage::showEvent(QShowEvent *event)
{
    WorkflowPage::showEvent(event);
    loadSamples();
}

void DatasetPage::loadSamples()
{
    AppStateStore store;
    const auto state = store.load();
    m_samples = state.has_value() ? state->samples : QVector<SampleRecord>();
    m_versions = state.has_value() ? state->datasetVersions : QVector<DatasetVersionRecord>();
    const QVector<CandidateReviewRecord> persistedReviews = state.has_value()
        ? state->candidateReviews
        : QVector<CandidateReviewRecord>();
    m_candidateGroups = CandidateReviewService::buildGroups(m_samples, persistedReviews);

    const QSignalBlocker sampleTableBlocker(m_table);
    m_table->setRowCount(0);
    const QString filter = m_stateFilter ? m_stateFilter->currentData().toString() : QStringLiteral("all");
    for (int candidateIndex = 0; candidateIndex < m_candidateGroups.size(); ++candidateIndex) {
        const CandidateReviewGroup &group = m_candidateGroups.at(candidateIndex);
        if (!CandidateReviewService::matchesFilter(group, filter)) {
            continue;
        }

        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *candidateItem = readOnlyItem(group.review.candidateId);
        candidateItem->setData(Qt::UserRole, candidateIndex);
        candidateItem->setToolTip(QStringLiteral("事件键：%1").arg(group.review.reviewKey));
        m_table->setItem(row, 0, candidateItem);
        auto *labelItem = readOnlyItem(sampleLabel(m_samples.at(group.sampleIndices.first())));
        const QString rawLabel = group.review.labelType.trimmed();
        if (!rawLabel.isEmpty() && rawLabel != labelItem->text()) {
            labelItem->setToolTip(QStringLiteral("原始标签：%1").arg(rawLabel));
        }
        m_table->setItem(row, 1, labelItem);
        m_table->setItem(row, 2, readOnlyItem(QString::number(group.review.windowCount)));
        m_table->setItem(row, 3, readOnlyItem(QString::number(group.review.ruleScore, 'f', 3)));
        m_table->setItem(row, 4, readOnlyItem(QString::number(group.review.averageSnrDb, 'f', 1)));
        m_table->setItem(row, 5, readOnlyItem(QString::number(group.review.averageOccupancy * 100.0, 'f', 1) + QLatin1Char('%')));
        auto *recommendationItem = readOnlyItem(group.review.recommendation);
        recommendationItem->setToolTip(group.review.reason);
        m_table->setItem(row, 6, recommendationItem);
        auto *reviewStateItem = readOnlyItem(group.review.reviewState);
        reviewStateItem->setToolTip(QStringLiteral("算法版本：%1").arg(group.review.algorithmVersion));
        m_table->setItem(row, 7, reviewStateItem);
        const int representativeIndex = group.representativeSampleIndices.isEmpty()
            ? group.sampleIndices.first()
            : group.representativeSampleIndices.first();
        const SampleRecord &representative = m_samples.at(representativeIndex);
        auto *pathItem = readOnlyItem(QFileInfo(representative.rawFilePath).fileName());
        pathItem->setToolTip(representative.rawFilePath);
        m_table->setItem(row, 8, pathItem);
    }

    const bool hasSamples = !m_samples.isEmpty();
    m_emptyState->setVisible(!hasSamples);
    m_table->setVisible(hasSamples);
    m_generateVersionButton->setEnabled(hasSamples);
    if (m_nextTrainingButton) {
        m_nextTrainingButton->setEnabled(!m_versions.isEmpty());
    }
    rebuildCategoryTable();
    loadVersionTable();
    updateSelectionSummary();
    updateReviewPanel();
}

void DatasetPage::rebuildCategoryTable()
{
    if (!m_categoryTable) {
        return;
    }

    QStringList previouslySelected = selectedLabels();
    const bool firstBuild = m_categoryTable->rowCount() == 0;
    const QSignalBlocker categoryTableBlocker(m_categoryTable);
    QMap<QString, CategorySummary> categories;
    for (const SampleRecord &sample : m_samples) {
        CategorySummary &summary = categories[sampleLabel(sample)];
        ++summary.total;
        if (qualityState(sample) == QStringLiteral("已剔除")) {
            continue;
        }
        if (isPartialSample(sample)) {
            ++summary.partial;
        } else if (isSampleEligible(sample) && isReviewedSample(sample)) {
            ++summary.available;
        } else if (isSampleEligible(sample)) {
            ++summary.pending;
        }
    }

    m_loadingCategories = true;
    m_categoryTable->setRowCount(0);
    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        const int row = m_categoryTable->rowCount();
        m_categoryTable->insertRow(row);

        auto *checkItem = new QTableWidgetItem();
        checkItem->setFlags(checkItem->flags() | Qt::ItemIsUserCheckable);
        const bool checked = firstBuild
            ? kTrainingLabels.contains(it.key())
            : previouslySelected.contains(it.key());
        checkItem->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        checkItem->setData(Qt::UserRole, it.value().pending);
        m_categoryTable->setItem(row, 0, checkItem);
        m_categoryTable->setItem(row, 1, readOnlyItem(it.key()));
        m_categoryTable->setItem(row, 2, readOnlyItem(QString::number(it.value().total)));
        m_categoryTable->setItem(row, 3, readOnlyItem(QString::number(it.value().available)));
        m_categoryTable->setItem(row, 4, readOnlyItem(QString::number(it.value().partial)));

        int included = it.value().available;
        if (m_includePartialCheck && m_includePartialCheck->isChecked()) {
            included += it.value().partial;
        }
        if (m_includePendingReviewCheck && m_includePendingReviewCheck->isChecked()) {
            included += it.value().pending;
        }
        auto *includedItem = readOnlyItem(QString::number(checked ? included : 0));
        m_categoryTable->setItem(row, 5, includedItem);
    }
    m_loadingCategories = false;
    updateSelectionSummary();
}

void DatasetPage::loadVersionTable()
{
    m_versionTable->setRowCount(0);
    for (const DatasetVersionRecord &version : m_versions) {
        const int row = m_versionTable->rowCount();
        m_versionTable->insertRow(row);
        m_versionTable->setItem(row, 0, readOnlyItem(version.versionId));
        m_versionTable->setItem(row, 1, readOnlyItem(version.selectedLabels.join(QStringLiteral("、"))));
        m_versionTable->setItem(row, 2, readOnlyItem(QString::number(version.sampleCount)));
        m_versionTable->setItem(row, 3, readOnlyItem(version.createdAt));
        m_versionTable->setItem(row, 4, readOnlyItem(version.manifestPath));
        m_versionTable->setItem(row, 5, readOnlyItem(version.includePartial ? QStringLiteral("是") : QStringLiteral("否")));
    }
}

void DatasetPage::updateSelectionSummary()
{
    const QStringList labels = selectedLabels();
    int selectedCount = 0;
    int partialCount = 0;
    int pendingCount = 0;
    int rejectedCount = 0;
    for (const SampleRecord &sample : m_samples) {
        if (!labels.contains(sampleLabel(sample))) {
            continue;
        }
        if (qualityState(sample) == QStringLiteral("已剔除")) {
            ++rejectedCount;
        } else if (isPartialSample(sample)) {
            ++partialCount;
        } else if (isSampleEligible(sample) && isReviewedSample(sample)) {
            ++selectedCount;
        } else if (isSampleEligible(sample)) {
            ++pendingCount;
        }
    }
    if (m_includePartialCheck && m_includePartialCheck->isChecked()) {
        selectedCount += partialCount;
    }
    if (m_includePendingReviewCheck && m_includePendingReviewCheck->isChecked()) {
        selectedCount += pendingCount;
    }

    if (m_totalSamplesMetric) {
        m_totalSamplesMetric->setValue(QString::number(m_samples.size()));
    }
    if (m_selectedSamplesMetric) {
        m_selectedSamplesMetric->setValue(QString::number(selectedCount));
    }
    if (m_selectedCategoriesMetric) {
        m_selectedCategoriesMetric->setValue(QString::number(labels.size()));
    }
    if (m_versionCountMetric) {
        m_versionCountMetric->setValue(QString::number(m_versions.size()));
    }
    m_selectionSummaryLabel->setText(
        labels.isEmpty()
            ? QStringLiteral("尚未选择类别。请选择至少一个类别后生成训练集版本。")
            : QStringLiteral("已选：%1；将纳入 %2 个样本%3%4%5。")
                  .arg(labels.join(QStringLiteral("、")))
                  .arg(selectedCount)
                  .arg(partialCount > 0 && (!m_includePartialCheck || !m_includePartialCheck->isChecked())
                           ? QStringLiteral("，另有 %1 个部分完成样本默认排除").arg(partialCount)
                           : QString())
                  .arg(pendingCount > 0 && (!m_includePendingReviewCheck || !m_includePendingReviewCheck->isChecked())
                           ? QStringLiteral("，%1 个待复核样本未纳入").arg(pendingCount)
                           : QString())
                  .arg(rejectedCount > 0
                           ? QStringLiteral("，%1 个已剔除样本不会纳入").arg(rejectedCount)
                           : QString()));

    if (m_readinessLabel) {
        QMap<QString, int> includedByLabel;
        int missingCount = 0;
        for (const SampleRecord &sample : m_samples) {
            const QString label = sampleLabel(sample);
            if (!labels.contains(label) || qualityState(sample) == QStringLiteral("已剔除")) {
                continue;
            }
            if (!isSampleEligible(sample)) {
                ++missingCount;
                continue;
            }
            if (isPartialSample(sample) && (!m_includePartialCheck || !m_includePartialCheck->isChecked())) {
                continue;
            }
            if (!isReviewedSample(sample) && (!m_includePendingReviewCheck || !m_includePendingReviewCheck->isChecked())) {
                continue;
            }
            ++includedByLabel[label];
        }
        int minimum = 0;
        int maximum = 0;
        for (auto it = includedByLabel.constBegin(); it != includedByLabel.constEnd(); ++it) {
            minimum = minimum == 0 ? it.value() : qMin(minimum, it.value());
            maximum = qMax(maximum, it.value());
        }
        const bool imbalanced = minimum > 0 && maximum >= minimum * 3;
        m_readinessLabel->setText(
            labels.isEmpty()
                ? QStringLiteral("训练准备度：请选择至少一个类别。")
                : QStringLiteral("训练准备度：%1 个类别，%2 个样本；最少类别 %3 个；%4%5")
                      .arg(labels.size())
                      .arg(selectedCount)
                      .arg(minimum)
                      .arg(imbalanced ? QStringLiteral("类别数量差异较大，训练结果可能偏向大类")
                                      : QStringLiteral("类别数量分布正常"))
                      .arg(missingCount > 0 ? QStringLiteral("；%1 个样本文件不存在，已排除").arg(missingCount)
                                            : QString()));
        m_readinessLabel->setProperty("kind", imbalanced ? "warning" : (labels.isEmpty() ? "neutral" : "good"));
        m_readinessLabel->style()->unpolish(m_readinessLabel);
        m_readinessLabel->style()->polish(m_readinessLabel);
    }
}

void DatasetPage::generateDatasetVersion()
{
    const QStringList labels = selectedLabels();
    if (labels.isEmpty()) {
        setPageMessage(QStringLiteral("请先选择至少一个类别。"), QStringLiteral("error"));
        return;
    }

    const bool includePartial = m_includePartialCheck && m_includePartialCheck->isChecked();
    const bool includePendingReview = m_includePendingReviewCheck && m_includePendingReviewCheck->isChecked();
    QVector<SampleRecord> selectedSamples;
    QMap<QString, int> labelCounts;
    for (const SampleRecord &sample : m_samples) {
        if (!labels.contains(sampleLabel(sample))
            || !isSampleEligible(sample)
            || qualityState(sample) == QStringLiteral("已剔除")) {
            continue;
        }
        if (isPartialSample(sample) && !includePartial) {
            continue;
        }
        if (!isReviewedSample(sample) && !includePendingReview) {
            continue;
        }
        selectedSamples.append(sample);
        ++labelCounts[sampleLabel(sample)];
    }
    if (selectedSamples.isEmpty()) {
        setPageMessage(QStringLiteral("所选类别没有可用于训练的样本。请先复核通过样本，或明确开启待复核/部分完成样本。"),
                       QStringLiteral("error"));
        return;
    }
    QStringList countSummary;
    for (auto it = labelCounts.constBegin(); it != labelCounts.constEnd(); ++it) {
        countSummary.append(QStringLiteral("%1：%2").arg(it.key()).arg(it.value()));
    }
    QStringList riskTypes;
    if (includePartial) {
        riskTypes.append(QStringLiteral("部分完成"));
    }
    if (includePendingReview) {
        riskTypes.append(QStringLiteral("待复核"));
    }
    const QString riskSummary = riskTypes.isEmpty()
        ? QString()
        : QStringLiteral("\n注意：本版本包含%1样本。").arg(riskTypes.join(QStringLiteral("、")));
    const auto answer = QMessageBox::question(
        this,
        QStringLiteral("确认生成训练集版本"),
        QStringLiteral("将创建一个新的只读版本，不会修改旧版本。\n类别：%1\n样本分布：%2\n总样本：%3%4")
            .arg(labels.join(QStringLiteral("、")))
            .arg(countSummary.join(QStringLiteral("；")))
            .arg(selectedSamples.size())
            .arg(riskSummary),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        return;
    }
    QStringList unsupportedLabels;
    for (const QString &label : labels) {
        if (!kTrainingLabels.contains(label)) {
            unsupportedLabels.append(label);
        }
    }
    if (!unsupportedLabels.isEmpty()) {
        setPageMessage(
            QStringLiteral("当前 CNNTRANS 模型只支持 16 个固定类别。请先把这些类别修正为模型类别：%1")
                .arg(unsupportedLabels.join(QStringLiteral("、"))),
            QStringLiteral("error"));
        return;
    }

    QMap<QString, QStringList> groupsByLabel;
    for (const SampleRecord &sample : selectedSamples) {
        groupsByLabel[sampleLabel(sample)].append(splitGroupKey(sample));
    }
    QMap<QString, QString> splitByGroup;
    for (auto it = groupsByLabel.begin(); it != groupsByLabel.end(); ++it) {
        QStringList groups = it.value();
        groups.removeDuplicates();
        groups.sort();
        for (int index = 0; index < groups.size(); ++index) {
            splitByGroup.insert(groups.at(index), splitForGroupIndex(index, groups.size()));
        }
    }
    QMap<QString, int> splitCounts;

    const QString versionId = QStringLiteral("dataset_v_%1")
                                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString versionDirectory = QDir(ProjectPaths::exportsDirectory())
                                         .filePath(QStringLiteral("dataset_versions/%1").arg(versionId));
    if (!QDir().mkpath(versionDirectory)) {
        setPageMessage(QStringLiteral("无法创建数据集版本目录：%1").arg(versionDirectory), QStringLiteral("error"));
        return;
    }

    const QString manifestPath = QDir(versionDirectory).filePath(QStringLiteral("manifest.jsonl"));
    QSaveFile manifest(manifestPath);
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setPageMessage(QStringLiteral("无法写入训练集清单：%1").arg(manifest.errorString()), QStringLiteral("error"));
        return;
    }
    for (const SampleRecord &sample : selectedSamples) {
        QJsonObject record = sample.toJson();
        const QString split = splitByGroup.value(splitGroupKey(sample), QStringLiteral("train"));
        const QString trainingLabel = sampleLabel(sample);
        record.insert(QStringLiteral("path"), sample.sampleFilePath);
        record.insert(QStringLiteral("label"), trainingLabel);
        record.insert(QStringLiteral("window_index"), sample.windowIndex);
        record.insert(QStringLiteral("source_file"), sample.rawFilePath);
        record.insert(QStringLiteral("session_id"), sample.batchId);
        record.insert(QStringLiteral("sample_id"), sample.sampleId);
        record.insert(QStringLiteral("candidate_id"), sample.candidateId);
        record.insert(QStringLiteral("datasetVersion"), versionId);
        record.insert(QStringLiteral("selectedLabel"), trainingLabel);
        record.insert(QStringLiteral("split"), split);
        ++splitCounts[split];
        manifest.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        manifest.write("\n");
    }
    if (!manifest.commit()) {
        setPageMessage(QStringLiteral("训练集清单写入失败：%1").arg(manifest.errorString()), QStringLiteral("error"));
        return;
    }

    const QString createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonObject summary;
    summary.insert(QStringLiteral("version_id"), versionId);
    summary.insert(QStringLiteral("task_type"), QStringLiteral("类型识别"));
    summary.insert(QStringLiteral("sample_count"), selectedSamples.size());
    summary.insert(QStringLiteral("created_at"), createdAt);
    summary.insert(QStringLiteral("include_partial"), includePartial);
    summary.insert(QStringLiteral("include_pending_review"), includePendingReview);
    summary.insert(QStringLiteral("manifest_path"), manifestPath);
    QJsonArray selectedLabelsJson;
    for (const QString &label : labels) {
        selectedLabelsJson.append(label);
    }
    summary.insert(QStringLiteral("selected_labels"), selectedLabelsJson);
    QJsonObject countsJson;
    for (auto it = labelCounts.constBegin(); it != labelCounts.constEnd(); ++it) {
        countsJson.insert(it.key(), it.value());
    }
    summary.insert(QStringLiteral("label_counts"), countsJson);
    QJsonObject splitCountsJson;
    for (auto it = splitCounts.constBegin(); it != splitCounts.constEnd(); ++it) {
        splitCountsJson.insert(it.key(), it.value());
    }
    summary.insert(QStringLiteral("split_counts"), splitCountsJson);

    const QString summaryPath = QDir(versionDirectory).filePath(QStringLiteral("summary.json"));
    QSaveFile summaryFile(summaryPath);
    if (!summaryFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setPageMessage(QStringLiteral("训练集摘要写入失败：%1").arg(summaryFile.errorString()), QStringLiteral("error"));
        return;
    }
    summaryFile.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    if (!summaryFile.commit()) {
        setPageMessage(QStringLiteral("训练集摘要提交失败：%1").arg(summaryFile.errorString()), QStringLiteral("error"));
        return;
    }

    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    DatasetVersionRecord version;
    version.versionId = versionId;
    version.taskType = QStringLiteral("类型识别");
    version.sampleCount = selectedSamples.size();
    version.strategy = QStringLiteral("按类别选择");
    version.createdAt = createdAt;
    version.sourceSummary = QStringLiteral("选择类别：%1；划分 train/val/test = %2/%3/%4")
                                .arg(labels.join(QStringLiteral("、")))
                                .arg(splitCounts.value(QStringLiteral("train")))
                                .arg(splitCounts.value(QStringLiteral("val")))
                                .arg(splitCounts.value(QStringLiteral("test")));
    version.outputDirectory = versionDirectory;
    version.manifestPath = manifestPath;
    version.selectedLabels = labels;
    version.includePartial = includePartial;
    version.labelCounts = labelCounts;
    state.datasetVersions.append(version);
    if (!store.save(state)) {
        setPageMessage(QStringLiteral("版本文件已生成，但应用状态保存失败：%1").arg(store.lastError()),
                       QStringLiteral("error"));
        return;
    }

    m_versions = state.datasetVersions;
    if (m_nextTrainingButton) {
        m_nextTrainingButton->setEnabled(true);
    }
    loadVersionTable();
    updateSelectionSummary();
    setPageMessage(QStringLiteral("已生成训练集版本 %1，共 %2 个样本，清单已保存到：%3")
                       .arg(versionId)
                       .arg(selectedSamples.size())
                       .arg(QDir::toNativeSeparators(manifestPath)),
                   QStringLiteral("good"));
}

void DatasetPage::setAllCategoryChecks(Qt::CheckState state)
{
    if (!m_categoryTable) {
        return;
    }
    const QSignalBlocker categoryTableBlocker(m_categoryTable);
    m_loadingCategories = true;
    for (int row = 0; row < m_categoryTable->rowCount(); ++row) {
        QTableWidgetItem *checkItem = m_categoryTable->item(row, 0);
        if (checkItem) {
            checkItem->setCheckState(state);
        }
        if (auto *included = m_categoryTable->item(row, 5)) {
            int value = 0;
            if (state == Qt::Checked) {
                value = m_categoryTable->item(row, 3)->text().toInt();
                if (m_includePartialCheck && m_includePartialCheck->isChecked()) {
                    value += m_categoryTable->item(row, 4)->text().toInt();
                }
                if (m_includePendingReviewCheck && m_includePendingReviewCheck->isChecked()) {
                    value += checkItem ? checkItem->data(Qt::UserRole).toInt() : 0;
                }
            }
            included->setText(QString::number(value));
        }
    }
    m_loadingCategories = false;
    updateSelectionSummary();
}

void DatasetPage::handleCategoryItemChanged(QTableWidgetItem *item)
{
    if (m_loadingCategories || !item || item->column() != 0) {
        return;
    }
    const int row = item->row();
    if (auto *included = m_categoryTable->item(row, 5)) {
        int value = item->checkState() == Qt::Checked
            ? m_categoryTable->item(row, 3)->text().toInt()
            : 0;
        if (item->checkState() == Qt::Checked
            && m_includePartialCheck
            && m_includePartialCheck->isChecked()) {
            value += m_categoryTable->item(row, 4)->text().toInt();
        }
        if (item->checkState() == Qt::Checked
            && m_includePendingReviewCheck
            && m_includePendingReviewCheck->isChecked()) {
            value += item->data(Qt::UserRole).toInt();
        }
        included->setText(QString::number(value));
    }
    updateSelectionSummary();
}

void DatasetPage::updateReviewPanel()
{
    const QVector<int> candidateIndices = selectedCandidateIndices();
    const QVector<int> indices = sampleIndicesForCandidates(candidateIndices);
    const bool hasSelection = !candidateIndices.isEmpty();
    if (m_reviewSelectionLabel) {
        m_reviewSelectionLabel->setText(hasSelection
                                            ? QStringLiteral("已选择 %1 个候选事件，覆盖 %2 个窗口")
                                                  .arg(candidateIndices.size())
                                                  .arg(indices.size())
                                            : QStringLiteral("请选择候选事件"));
    }
    if (m_labelCombo) {
        if (candidateIndices.size() == 1
            && candidateIndices.first() >= 0
            && candidateIndices.first() < m_candidateGroups.size()) {
            const CandidateReviewGroup &selectedGroup = m_candidateGroups.at(candidateIndices.first());
            const int sampleIndex = selectedGroup.representativeSampleIndices.isEmpty()
                ? selectedGroup.sampleIndices.first()
                : selectedGroup.representativeSampleIndices.first();
            const int labelIndex = m_labelCombo->findData(sampleLabel(m_samples.at(sampleIndex)));
            m_labelCombo->setCurrentIndex(labelIndex >= 0 ? labelIndex : 0);
        } else if (!hasSelection) {
            m_labelCombo->setCurrentIndex(0);
        }
    }
    if (m_sampleDetailLabel) {
        if (candidateIndices.size() == 1
            && candidateIndices.first() >= 0
            && candidateIndices.first() < m_candidateGroups.size()) {
            const CandidateReviewGroup &group = m_candidateGroups.at(candidateIndices.first());
            const int sampleIndex = group.representativeSampleIndices.isEmpty()
                ? group.sampleIndices.first()
                : group.representativeSampleIndices.first();
            const SampleRecord &sample = m_samples.at(sampleIndex);
            QString detail = QStringLiteral(
                "候选详情：%1\n窗口数：%2 · 规则分：%3 · 建议：%4\n原始文件：%5\n代表 NPZ：%6\n中心频率：%7 MHz · 带宽：%8 MHz · 平均 SNR：%9 dB · 候选局部图：%10\n原因：%11")
                .arg(group.review.candidateId)
                .arg(group.review.windowCount)
                .arg(group.review.ruleScore, 0, 'f', 3)
                .arg(group.review.recommendation);
            detail = detail.arg(QDir::toNativeSeparators(sample.rawFilePath));
            detail = detail.arg(QDir::toNativeSeparators(sample.sampleFilePath));
            detail = detail.arg(sample.centerFrequencyHz / 1000000.0, 0, 'f', 3);
            detail = detail.arg(sample.bandwidthHz / 1000000.0, 0, 'f', 3);
            detail = detail.arg(group.review.averageSnrDb, 0, 'f', 2);
            detail = detail.arg(group.review.candidatePreviewPath.isEmpty()
                                    ? QStringLiteral("未生成")
                                    : QDir::toNativeSeparators(group.review.candidatePreviewPath));
            detail = detail.arg(group.review.reason);
            m_sampleDetailLabel->setText(detail);
        } else if (candidateIndices.size() > 1) {
            m_sampleDetailLabel->setText(QStringLiteral("已选择 %1 个候选事件，可批量修改事件复核状态；类别修改请先选择单个事件。")
                                             .arg(candidateIndices.size()));
        } else {
            m_sampleDetailLabel->setText(QStringLiteral("选择候选事件后显示窗口数、规则建议、完整路径、时频图和指标。"));
        }
    }
    if (m_spectrogramReviewWidget && m_spectrogramPathLabel) {
        if (candidateIndices.size() == 1
            && candidateIndices.first() >= 0
            && candidateIndices.first() < m_candidateGroups.size()) {
            const CandidateReviewGroup &group = m_candidateGroups.at(candidateIndices.first());
            const int sampleIndex = group.representativeSampleIndices.isEmpty()
                ? group.sampleIndices.first()
                : group.representativeSampleIndices.first();
            const SampleRecord &sample = m_samples.at(sampleIndex);
            QString imagePath;
            if (!group.review.candidatePreviewPath.trimmed().isEmpty()
                && QFileInfo::exists(group.review.candidatePreviewPath)) {
                imagePath = group.review.candidatePreviewPath;
            } else if (!sample.candidatePreviewFilePath.trimmed().isEmpty()
                       && QFileInfo::exists(sample.candidatePreviewFilePath)) {
                imagePath = sample.candidatePreviewFilePath;
            } else {
                imagePath = sample.spectrogramFilePath;
            }
            m_spectrogramReviewWidget->setImagePath(imagePath);
            m_spectrogramPathLabel->setText(
                imagePath.isEmpty()
                    ? QStringLiteral("当前候选没有关联时频图。")
                    : QStringLiteral("图像路径：%1\n黄色框为预处理候选区域，供专家结合 SNR、占用率和原始窗口共同复核。")
                          .arg(QDir::toNativeSeparators(imagePath)));
        } else if (candidateIndices.size() > 1) {
            m_spectrogramReviewWidget->clearImage(QStringLiteral("已选择多个候选事件，请选择单个事件查看框选时频图"));
            m_spectrogramPathLabel->setText(QStringLiteral("批量复核时不展示单一候选图。"));
        } else {
            m_spectrogramReviewWidget->clearImage();
            m_spectrogramPathLabel->setText(QStringLiteral("选择候选事件后加载候选总览图。"));
        }
    }
    const QVector<QPushButton *> buttons = {m_approveButton, m_rejectButton, m_pendingButton, m_applyLabelButton};
    for (QPushButton *button : buttons) {
        if (button) {
            button->setEnabled(hasSelection);
        }
    }
}

void DatasetPage::applyReviewToSelection(const QString &reviewState)
{
    const QVector<int> candidateIndices = selectedCandidateIndices();
    const QVector<int> indices = sampleIndicesForCandidates(candidateIndices);
    if (candidateIndices.isEmpty()) {
        setPageMessage(QStringLiteral("请先选择要复核的候选事件。"), QStringLiteral("error"));
        return;
    }
    for (int index : indices) {
        if (index < 0 || index >= m_samples.size()) {
            continue;
        }
        SampleRecord &sample = m_samples[index];
        sample.qualityStatus = reviewState;
        sample.status = reviewState;
    }
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    for (const int candidateIndex : candidateIndices) {
        if (candidateIndex >= 0 && candidateIndex < m_candidateGroups.size()) {
            m_candidateGroups[candidateIndex].review.reviewState = reviewState;
            m_candidateGroups[candidateIndex].review.reviewedAt = now;
        }
    }
    saveSamplesToState(QStringLiteral("已更新 %1 个候选事件（%2 个窗口）的复核状态为：%3")
                           .arg(candidateIndices.size())
                           .arg(indices.size())
                           .arg(reviewState));
}

void DatasetPage::applyLabelToSelection()
{
    const QString label = m_labelCombo ? m_labelCombo->currentData().toString() : QString();
    if (label.isEmpty()) {
        setPageMessage(QStringLiteral("请选择模型支持的标准类别。"), QStringLiteral("error"));
        return;
    }
    const QVector<int> candidateIndices = selectedCandidateIndices();
    const QVector<int> indices = sampleIndicesForCandidates(candidateIndices);
    if (indices.isEmpty()) {
        setPageMessage(QStringLiteral("请先选择要修改类别的候选事件。"), QStringLiteral("error"));
        return;
    }
    for (int index : indices) {
        if (index < 0 || index >= m_samples.size()) {
            continue;
        }
        m_samples[index].labelType = label;
    }
    for (const int candidateIndex : candidateIndices) {
        if (candidateIndex >= 0 && candidateIndex < m_candidateGroups.size()) {
            m_candidateGroups[candidateIndex].review.labelType = label;
        }
    }
    saveSamplesToState(QStringLiteral("已将 %1 个候选事件（%2 个窗口）的类别更新为：%3")
                           .arg(candidateIndices.size())
                           .arg(indices.size())
                           .arg(label));
}

QStringList DatasetPage::selectedLabels() const
{
    QStringList labels;
    if (!m_categoryTable) {
        return labels;
    }
    for (int row = 0; row < m_categoryTable->rowCount(); ++row) {
        const QTableWidgetItem *checkItem = m_categoryTable->item(row, 0);
        const QTableWidgetItem *labelItem = m_categoryTable->item(row, 1);
        if (checkItem && labelItem && checkItem->checkState() == Qt::Checked) {
            labels.append(labelItem->text());
        }
    }
    return labels;
}

QVector<int> DatasetPage::selectedSampleIndices() const
{
    QVector<int> indices;
    if (!m_table) {
        return indices;
    }
    const QList<QTableWidgetItem *> items = m_table->selectedItems();
    QSet<int> rows;
    for (const QTableWidgetItem *item : items) {
        if (item) {
            rows.insert(item->row());
        }
    }
    for (int row : rows) {
        const QTableWidgetItem *idItem = m_table->item(row, 0);
        if (!idItem) {
            continue;
        }
        bool ok = false;
        const int index = idItem->data(Qt::UserRole).toInt(&ok);
        if (ok) {
            indices.append(index);
        }
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

QVector<int> DatasetPage::selectedCandidateIndices() const
{
    QVector<int> indices;
    if (!m_table) {
        return indices;
    }
    QSet<int> rows;
    for (const QTableWidgetItem *item : m_table->selectedItems()) {
        if (item) {
            rows.insert(item->row());
        }
    }
    for (const int row : rows) {
        const QTableWidgetItem *idItem = m_table->item(row, 0);
        if (!idItem) {
            continue;
        }
        bool ok = false;
        const int index = idItem->data(Qt::UserRole).toInt(&ok);
        if (ok && index >= 0 && index < m_candidateGroups.size()) {
            indices.append(index);
        }
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

QVector<int> DatasetPage::sampleIndicesForCandidates(const QVector<int> &candidateIndices) const
{
    QVector<int> indices;
    QSet<int> unique;
    for (const int candidateIndex : candidateIndices) {
        if (candidateIndex < 0 || candidateIndex >= m_candidateGroups.size()) {
            continue;
        }
        for (const int sampleIndex : m_candidateGroups.at(candidateIndex).sampleIndices) {
            if (unique.contains(sampleIndex)) {
                continue;
            }
            unique.insert(sampleIndex);
            indices.append(sampleIndex);
        }
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

void DatasetPage::rebuildCandidateGroups()
{
    AppStateStore store;
    const auto state = store.load();
    m_candidateGroups = CandidateReviewService::buildGroups(
        m_samples,
        state.has_value() ? state->candidateReviews : QVector<CandidateReviewRecord>());
}

void DatasetPage::applyRuleTriage()
{
    int rejected = 0;
    int spotCheck = 0;
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    for (CandidateReviewGroup &group : m_candidateGroups) {
        if (group.review.reviewState != QStringLiteral("待复核")) {
            continue;
        }
        if (group.review.recommendation == QStringLiteral("规则建议剔除")) {
            for (const int sampleIndex : group.sampleIndices) {
                if (sampleIndex < 0 || sampleIndex >= m_samples.size()) {
                    continue;
                }
                m_samples[sampleIndex].qualityStatus = QStringLiteral("已剔除");
                m_samples[sampleIndex].status = QStringLiteral("已剔除");
            }
            group.review.reviewState = QStringLiteral("已剔除");
            group.review.reviewedAt = now;
            ++rejected;
        } else if (group.review.recommendation == QStringLiteral("规则建议通过")) {
            group.review.reviewState = QStringLiteral("待抽检");
            group.review.reviewedAt = now;
            ++spotCheck;
        }
    }
    if (rejected == 0 && spotCheck == 0) {
        setPageMessage(QStringLiteral("当前没有可应用的高置信规则建议。规则建议通过会先进入待抽检，不会直接进入训练集。"));
        return;
    }
    saveSamplesToState(QStringLiteral("已处理 %1 个规则建议剔除事件，%2 个规则建议通过事件转为待抽检。")
                           .arg(rejected)
                           .arg(spotCheck));
}

bool DatasetPage::isSampleEligible(const SampleRecord &sample) const
{
    return !sample.sampleFilePath.trimmed().isEmpty() && QFileInfo::exists(sample.sampleFilePath);
}

bool DatasetPage::isPartialSample(const SampleRecord &sample) const
{
    const QString state = processingState(sample);
    return state == QStringLiteral("部分完成")
        || state == QStringLiteral("已取消")
        || state == QStringLiteral("已超时")
        || state == QStringLiteral("失败")
        || state.contains(QStringLiteral("部分"));
}

bool DatasetPage::isReviewedSample(const SampleRecord &sample) const
{
    const QString quality = qualityState(sample);
    return quality == QStringLiteral("复核通过") || quality == QStringLiteral("已标注");
}

bool DatasetPage::saveSamplesToState(const QString &successMessage)
{
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    state.samples = m_samples;
    state.candidateReviews.clear();
    state.candidateReviews.reserve(m_candidateGroups.size());
    for (const CandidateReviewGroup &group : m_candidateGroups) {
        state.candidateReviews.append(group.review);
    }
    if (!store.save(state)) {
        setPageMessage(QStringLiteral("样本状态保存失败：%1").arg(store.lastError()), QStringLiteral("error"));
        return false;
    }
    setPageMessage(successMessage, QStringLiteral("good"));
    rebuildCategoryTable();
    loadSamples();
    return true;
}

void DatasetPage::setPageMessage(const QString &message, const QString &kind)
{
    m_pageMessage->setText(message);
    m_pageMessage->setProperty("kind", kind);
    m_pageMessage->setVisible(!message.isEmpty());
    m_pageMessage->style()->unpolish(m_pageMessage);
    m_pageMessage->style()->polish(m_pageMessage);
}
