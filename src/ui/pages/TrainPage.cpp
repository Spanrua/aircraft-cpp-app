#include "ui/pages/TrainPage.h"

#include "ui/widgets/MetricCard.h"
#include "ui/widgets/IllustrationPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

TrainPage::TrainPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("模型训练"), QStringLiteral("选择数据集版本、查看训练状态并导出模型。"), parent)
{
    contentLayout()->addWidget(new IllustrationPanel(
        IllustrationPanel::Kind::Training,
        QStringLiteral("模型训练管线"),
        QStringLiteral("训练阶段保留为占位工作台，后续通过 Python 或 C++ 后端接入真实训练任务。"),
        this));

    auto *metrics = new QHBoxLayout();
    auto *dataset = new MetricCard(QStringLiteral("数据集版本"), this);
    auto *samples = new MetricCard(QStringLiteral("样本数量"), this);
    auto *trainState = new MetricCard(QStringLiteral("训练状态"), this);
    dataset->setValue(QStringLiteral("未选择"));
    samples->setValue(QStringLiteral("-"));
    trainState->setValue(QStringLiteral("待训练"));
    metrics->addWidget(dataset);
    metrics->addWidget(samples);
    metrics->addWidget(trainState);
    contentLayout()->addLayout(metrics);

    auto *actions = new QHBoxLayout();
    auto *selectDatasetButton = new QPushButton(QStringLiteral("选择数据集"), this);
    selectDatasetButton->setProperty("secondary", true);
    auto *startButton = new QPushButton(QStringLiteral("开始训练"), this);
    startButton->setProperty("primary", true);
    auto *exportButton = new QPushButton(QStringLiteral("导出模型"), this);
    exportButton->setProperty("secondary", true);
    actions->addWidget(selectDatasetButton);
    actions->addWidget(startButton);
    actions->addWidget(exportButton);
    actions->addStretch(1);
    contentLayout()->addLayout(actions);

    auto *emptyState = new QLabel(QStringLiteral("尚未选择数据集版本。完成数据集生成后，可在这里启动训练任务。"), this);
    emptyState->setObjectName(QStringLiteral("EmptyState"));
    contentLayout()->addWidget(emptyState);

    auto *logs = new QPlainTextEdit(this);
    logs->setReadOnly(true);
    logs->setPlainText(QStringLiteral("训练日志将在这里显示。\n当前阶段：等待选择数据集版本。"));
    contentLayout()->addWidget(logs, 1);
}
