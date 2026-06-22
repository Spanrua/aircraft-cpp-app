#include "ui/pages/DatasetPage.h"

#include "services/AppStateStore.h"
#include "ui/widgets/IllustrationPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>

DatasetPage::DatasetPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("数据集管理"), QStringLiteral("复核样本、维护标签并生成数据集版本。"), parent)
{
    contentLayout()->addWidget(new IllustrationPanel(
        IllustrationPanel::Kind::Dataset,
        QStringLiteral("样本资产管理"),
        QStringLiteral("把预处理生成的片段整理成可复核、可追踪、可训练的数据集版本。"),
        this));

    auto *actions = new QHBoxLayout();
    auto *refreshButton = new QPushButton(QStringLiteral("刷新样本"), this);
    refreshButton->setProperty("primary", true);
    actions->addWidget(refreshButton);
    auto *labelButton = new QPushButton(QStringLiteral("自动标注"), this);
    labelButton->setProperty("secondary", true);
    auto *versionButton = new QPushButton(QStringLiteral("生成数据集版本"), this);
    versionButton->setProperty("secondary", true);
    actions->addWidget(labelButton);
    actions->addWidget(versionButton);
    actions->addStretch(1);
    contentLayout()->addLayout(actions);

    m_emptyState = new QLabel(QStringLiteral("暂无样本。请先在预处理页生成有效样本。"), this);
    m_emptyState->setObjectName(QStringLiteral("EmptyState"));
    contentLayout()->addWidget(m_emptyState);

    m_table = new QTableWidget(0, 6, this);
    m_table->setAlternatingRowColors(true);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("样本ID"),
        QStringLiteral("来源文件"),
        QStringLiteral("类型标签"),
        QStringLiteral("个体标签"),
        QStringLiteral("采样率"),
        QStringLiteral("状态"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    contentLayout()->addWidget(m_table);

    connect(refreshButton, &QPushButton::clicked, this, &DatasetPage::loadSamples);
    loadSamples();
}

void DatasetPage::showEvent(QShowEvent *event)
{
    WorkflowPage::showEvent(event);
    loadSamples();
}

void DatasetPage::loadSamples()
{
    m_table->setRowCount(0);

    AppStateStore store;
    const auto state = store.load();
    if (!state.has_value()) {
        m_emptyState->setVisible(true);
        m_table->setVisible(false);
        return;
    }

    for (const SampleRecord &sample : state->samples) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(sample.sampleId));
        m_table->setItem(row, 1, new QTableWidgetItem(sample.sampleFilePath));
        m_table->setItem(row, 2, new QTableWidgetItem(sample.labelType));
        m_table->setItem(row, 3, new QTableWidgetItem(sample.labelIndividual));
        m_table->setItem(row, 4, new QTableWidgetItem(QStringLiteral("%1 MHz").arg(sample.sampleRateHz / 1000000.0, 0, 'f', 3)));
        m_table->setItem(row, 5, new QTableWidgetItem(sample.status));
    }

    const bool hasSamples = m_table->rowCount() > 0;
    m_emptyState->setVisible(!hasSamples);
    m_table->setVisible(hasSamples);
}
