#include "ui/pages/RecognitionPage.h"

#include "ui/widgets/MetricCard.h"
#include "ui/widgets/IllustrationPanel.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

RecognitionPage::RecognitionPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("无人机识别"), QStringLiteral("加载模型并执行类型识别或个体识别。"), parent)
{
    contentLayout()->addWidget(new IllustrationPanel(
        IllustrationPanel::Kind::Recognition,
        QStringLiteral("识别结果判读"),
        QStringLiteral("加载模型后输出类型、个体、置信度和诊断状态，辅助操作员快速确认结果。"),
        this));

    auto *metrics = new QHBoxLayout();
    auto *model = new MetricCard(QStringLiteral("模型状态"), this);
    auto *confidence = new MetricCard(QStringLiteral("最高置信度"), this);
    auto *result = new MetricCard(QStringLiteral("识别结果"), this);
    model->setValue(QStringLiteral("未加载"));
    confidence->setValue(QStringLiteral("-"));
    result->setValue(QStringLiteral("待识别"));
    metrics->addWidget(model);
    metrics->addWidget(confidence);
    metrics->addWidget(result);
    contentLayout()->addLayout(metrics);

    auto *actions = new QHBoxLayout();
    auto *loadModelButton = new QPushButton(QStringLiteral("加载模型"), this);
    loadModelButton->setProperty("secondary", true);
    auto *selectSampleButton = new QPushButton(QStringLiteral("选择样本"), this);
    selectSampleButton->setProperty("secondary", true);
    auto *runButton = new QPushButton(QStringLiteral("执行识别"), this);
    runButton->setProperty("primary", true);
    actions->addWidget(loadModelButton);
    actions->addWidget(selectSampleButton);
    actions->addWidget(runButton);
    actions->addStretch(1);
    contentLayout()->addLayout(actions);

    auto *emptyState = new QLabel(QStringLiteral("暂无识别结果。加载模型并选择样本后，结果将在下方表格中显示。"), this);
    emptyState->setObjectName(QStringLiteral("EmptyState"));
    contentLayout()->addWidget(emptyState);

    auto *table = new QTableWidget(0, 5, this);
    table->setAlternatingRowColors(true);
    table->setHorizontalHeaderLabels({
        QStringLiteral("样本"),
        QStringLiteral("类型"),
        QStringLiteral("个体"),
        QStringLiteral("置信度"),
        QStringLiteral("状态"),
    });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    contentLayout()->addWidget(table, 1);
}
