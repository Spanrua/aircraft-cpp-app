#include "ui/MainWindow.h"

#include "app/ProjectPaths.h"
#include "ui/pages/CapturePage.h"
#include "ui/pages/DatasetPage.h"
#include "ui/pages/PreprocessPage.h"
#include "ui/pages/RecognitionPage.h"
#include "ui/pages/TrainPage.h"
#include "ui/widgets/StatusBadge.h"
#include "ui/widgets/StepNavigator.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
QStringList workflowSteps()
{
    return {
        QStringLiteral("采集"),
        QStringLiteral("预处理"),
        QStringLiteral("数据集"),
        QStringLiteral("训练"),
        QStringLiteral("识别"),
    };
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("无人机射频信号识别系统"));
    resize(1600, 960);
    setMinimumSize(1100, 720);
    buildUi();
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(22, 18, 22, 14);
    root->setSpacing(14);

    auto *systemBar = new QFrame(central);
    systemBar->setObjectName(QStringLiteral("SystemBar"));
    auto *systemLayout = new QHBoxLayout(systemBar);
    systemLayout->setContentsMargins(20, 14, 20, 14);
    systemLayout->setSpacing(12);

    auto *brandBlock = new QVBoxLayout();
    auto *title = new QLabel(QStringLiteral("无人机射频信号识别系统"), systemBar);
    title->setObjectName(QStringLiteral("AppTitle"));
    m_currentStepLabel = new QLabel(QStringLiteral("当前步骤：1 / 5 采集"), systemBar);
    m_currentStepLabel->setObjectName(QStringLiteral("CurrentStepLabel"));
    brandBlock->addWidget(title);
    brandBlock->addWidget(m_currentStepLabel);
    systemLayout->addLayout(brandBlock, 1);

    m_deviceBadge = new StatusBadge(QStringLiteral("3943B 未接入"), systemBar);
    m_deviceBadge->setStatus(QStringLiteral("3943B 未接入"), QStringLiteral("warning"));
    m_taskBadge = new StatusBadge(QStringLiteral("任务待机"), systemBar);
    m_taskBadge->setStatus(QStringLiteral("任务待机"), QStringLiteral("neutral"));
    m_clockLabel = new QLabel(systemBar);
    m_clockLabel->setObjectName(QStringLiteral("ClockLabel"));
    m_dataPathLabel = new QLabel(ProjectPaths::dataDirectory(), systemBar);
    m_dataPathLabel->setObjectName(QStringLiteral("DataPathLabel"));
    m_dataPathLabel->setToolTip(ProjectPaths::dataDirectory());
    m_dataPathLabel->setMaximumWidth(420);

    systemLayout->addWidget(m_deviceBadge);
    systemLayout->addWidget(m_taskBadge);
    systemLayout->addWidget(m_clockLabel);
    systemLayout->addWidget(m_dataPathLabel);
    root->addWidget(systemBar);

    m_stepNavigator = new StepNavigator(this);
    m_stepNavigator->setSteps(workflowSteps());
    root->addWidget(m_stepNavigator);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("WorkflowStack"));
    m_stack->addWidget(new CapturePage(m_stack));
    m_stack->addWidget(new PreprocessPage(m_stack));
    m_stack->addWidget(new DatasetPage(m_stack));
    m_stack->addWidget(new TrainPage(m_stack));
    m_stack->addWidget(new RecognitionPage(m_stack));
    root->addWidget(m_stack, 1);

    connect(m_stepNavigator, &StepNavigator::stepSelected, this, &MainWindow::updateCurrentStep);

    auto *clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, &MainWindow::updateClock);
    clockTimer->start(1000);

    setCentralWidget(central);
    updateClock();
    updateCurrentStep(0);
    statusBar()->showMessage(QStringLiteral("系统就绪"));
}

void MainWindow::updateCurrentStep(int index)
{
    const QStringList steps = workflowSteps();
    if (index < 0 || index >= steps.size()) {
        return;
    }

    m_stack->setCurrentIndex(index);
    m_stepNavigator->setCurrentIndex(index);
    m_currentStepLabel->setText(QStringLiteral("当前步骤：%1 / %2 %3").arg(index + 1).arg(steps.size()).arg(steps.at(index)));
    m_taskBadge->setStatus(QStringLiteral("查看%1").arg(steps.at(index)), QStringLiteral("neutral"));
    statusBar()->showMessage(QStringLiteral("当前步骤：%1").arg(steps.at(index)));
}

void MainWindow::updateClock()
{
    m_clockLabel->setText(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}
