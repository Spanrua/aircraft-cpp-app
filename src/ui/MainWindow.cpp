#include "ui/MainWindow.h"

#include "app/ProjectPaths.h"
#include "ui/pages/CapturePage.h"
#include "ui/pages/DatasetPage.h"
#include "ui/pages/PreprocessPage.h"
#include "ui/pages/RecognitionPage.h"
#include "ui/pages/SweepPage.h"
#include "ui/pages/TrainPage.h"
#include "services/AppStateStore.h"
#include "ui/WorkflowState.h"
#include "ui/widgets/StatusBadge.h"
#include "ui/widgets/StepNavigator.h"

#include "services/CeyearVisaService.h"
#include "services/UsrpDeviceService.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

namespace
{
QStringList workflowSteps()
{
    return {
        QStringLiteral("扫频定位"),
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
    buildUi(nullptr, nullptr, nullptr);
}

MainWindow::MainWindow(std::unique_ptr<ICeyearVisaService> sweepService,
                       std::unique_ptr<IUsrpDeviceService> captureUsrpService,
                       std::unique_ptr<ICeyearVisaService> captureCeyearService,
                       QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("无人机射频信号识别系统"));
    resize(1600, 960);
    setMinimumSize(1100, 720);
    buildUi(std::move(sweepService), std::move(captureUsrpService), std::move(captureCeyearService));
}

MainWindow::~MainWindow()
{
    if (m_clockTimer) {
        m_clockTimer->stop();
        m_clockTimer->disconnect(this);
    }
    m_clockLabel = nullptr;
    m_currentStepLabel = nullptr;
    m_dataPathLabel = nullptr;
}

void MainWindow::buildUi(std::unique_ptr<ICeyearVisaService> sweepService,
                         std::unique_ptr<IUsrpDeviceService> captureUsrpService,
                         std::unique_ptr<ICeyearVisaService> captureCeyearService)
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(22, 14, 22, 12);
    root->setSpacing(10);

    auto *systemBar = new QFrame(central);
    systemBar->setObjectName(QStringLiteral("SystemBar"));
    auto *systemLayout = new QHBoxLayout(systemBar);
    systemLayout->setContentsMargins(16, 11, 16, 11);
    systemLayout->setSpacing(16);
    auto *titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(2);
    auto *appTitle = new QLabel(QStringLiteral("无人机射频信号识别系统"), systemBar);
    appTitle->setObjectName(QStringLiteral("AppTitle"));
    titleBlock->addWidget(appTitle);
    m_currentStepLabel = new QLabel(QStringLiteral("当前步骤：1 / 6 扫频定位"), systemBar);
    m_currentStepLabel->setObjectName(QStringLiteral("CurrentStepLabel"));
    titleBlock->addWidget(m_currentStepLabel);
    systemLayout->addLayout(titleBlock, 1);
    auto *runtimeBlock = new QVBoxLayout();
    runtimeBlock->setSpacing(2);
    m_clockLabel = new QLabel(systemBar);
    m_clockLabel->setObjectName(QStringLiteral("ClockLabel"));
    m_clockLabel->setAlignment(Qt::AlignRight);
    runtimeBlock->addWidget(m_clockLabel);
    auto *dataPathCaption = new QLabel(QStringLiteral("数据目录"), systemBar);
    dataPathCaption->setProperty("muted", true);
    dataPathCaption->setAlignment(Qt::AlignRight);
    runtimeBlock->addWidget(dataPathCaption);
    systemLayout->addLayout(runtimeBlock);
    m_dataPathLabel = new QLabel(ProjectPaths::dataDirectory(), central);
    m_dataPathLabel->setObjectName(QStringLiteral("DataPathLabel"));
    m_dataPathLabel->setToolTip(ProjectPaths::dataDirectory());
    m_deviceBadge = nullptr;
    m_taskBadge = nullptr;
    root->addWidget(systemBar);
    root->addWidget(m_dataPathLabel);

    m_stepNavigator = new StepNavigator(this);
    m_stepNavigator->setSteps(workflowSteps());
    m_stepNavigator->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    root->addWidget(m_stepNavigator);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("WorkflowStack"));
    m_stack->setMinimumSize(0, 0);
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_sweepPage = sweepService
        ? new SweepPage(std::move(sweepService), m_stack)
        : new SweepPage(m_stack);
    if (captureUsrpService || captureCeyearService) {
        if (!captureUsrpService) {
            captureUsrpService = std::make_unique<UsrpDeviceService>();
        }
        if (!captureCeyearService) {
            captureCeyearService = std::make_unique<CeyearVisaService>();
        }
        m_capturePage = new CapturePage(std::move(captureUsrpService), std::move(captureCeyearService), m_stack);
    } else {
        m_capturePage = new CapturePage(m_stack);
    }
    m_stack->addWidget(m_sweepPage);
    m_stack->addWidget(m_capturePage);
    m_preprocessPage = new PreprocessPage(m_stack);
    m_stack->addWidget(m_preprocessPage);
    m_datasetPage = new DatasetPage(m_stack);
    m_trainPage = new TrainPage(m_stack);
    m_recognitionPage = new RecognitionPage(m_stack);
    m_stack->addWidget(m_datasetPage);
    m_stack->addWidget(m_trainPage);
    m_stack->addWidget(m_recognitionPage);
    root->addWidget(m_stack, 1);

    connect(m_stepNavigator, &StepNavigator::stepSelected, this, [this](int index) {
        updateCurrentStep(index);
    });
    connect(m_sweepPage, &SweepPage::frequencyConfirmed, this, [this](double frequencyHz) {
        if (m_capturePage) {
            m_capturePage->setPresetCenterFrequencyHz(frequencyHz);
        }
        updateCurrentStep(1);
    });
    connect(m_capturePage, &CapturePage::captureFileReady, this, [this](const QString &filePath) {
        if (m_preprocessPage) {
            m_preprocessPage->setInputFilePath(filePath);
        }
        statusBar()->showMessage(QStringLiteral("采集文件已传递到预处理页：%1").arg(QFileInfo(filePath).fileName()));
        refreshWorkflowStatuses();
    });
    connect(m_preprocessPage, &PreprocessPage::requestOpenDataset, this, [this]() {
        updateCurrentStep(3);
    });
    connect(m_datasetPage, &DatasetPage::requestOpenTraining, this, [this]() {
        updateCurrentStep(4);
    });
    connect(m_trainPage, &TrainPage::modelReadyForRecognition,
            this, [this](const QString &modelPath, const QString &datasetVersionId) {
                m_recognitionPage->setModelForRecognition(modelPath, datasetVersionId);
                updateCurrentStep(5);
            });

    m_clockTimer = new QTimer(this);
    connect(m_clockTimer, &QTimer::timeout, this, [this]() {
        updateClock();
    });
    m_clockTimer->start(1000);

    setCentralWidget(central);
    updateClock();
    updateCurrentStep(0);
    statusBar()->showMessage(QStringLiteral("系统就绪"));
    refreshWorkflowStatuses();
}

void MainWindow::updateCurrentStep(int index)
{
    const QStringList steps = workflowSteps();
    if (index < 0 || index >= steps.size()) {
        return;
    }

    m_stack->setCurrentIndex(index);
    m_stepNavigator->setCurrentIndex(index);
    const WorkflowStepState state = m_stepNavigator->stepState(index);
    const QString blockingReason = m_stepNavigator->stepBlockingReason(index);
    const QString suffix = state == WorkflowStepState::Blocked && !blockingReason.isEmpty()
        ? QStringLiteral(" · 阻塞：%1").arg(blockingReason)
        : QString();
    m_currentStepLabel->setText(QStringLiteral("当前步骤：%1 / %2 %3%4")
                                    .arg(index + 1)
                                    .arg(steps.size())
                                    .arg(steps.at(index))
                                    .arg(suffix));
    if (m_taskBadge) {
        m_taskBadge->setStatus(QStringLiteral("查看%1").arg(steps.at(index)), QStringLiteral("neutral"));
    }
    statusBar()->showMessage(state == WorkflowStepState::Blocked
                                 ? QStringLiteral("当前步骤暂不可继续：%1")
                                       .arg(blockingReason.isEmpty()
                                                ? QStringLiteral("请完成前置步骤")
                                                : blockingReason)
                                 : QStringLiteral("当前步骤：%1").arg(steps.at(index)),
                             8000);
    refreshWorkflowStatuses();
}

void MainWindow::refreshWorkflowStatuses()
{
    if (!m_stepNavigator) {
        return;
    }
    AppStateStore store;
    const auto state = store.load();
    const AppState appState = state.value_or(AppState{});

    m_stepNavigator->setStepStatus(0, WorkflowStepState::Ready,
                                   QStringLiteral("可开始扫频定位"));
    m_stepNavigator->setStepStatus(1, WorkflowStepState::Ready,
                                   QStringLiteral("可配置采集设备并生成 IQRAW"));

    if (m_preprocessPage && m_preprocessPage->isRunning()) {
        m_stepNavigator->setStepStatus(2, WorkflowStepState::Running,
                                       QStringLiteral("预处理任务正在运行"));
    } else if (m_preprocessPage && m_preprocessPage->hasResults()) {
        m_stepNavigator->setStepStatus(2, WorkflowStepState::Completed,
                                       QStringLiteral("已生成候选片段和样本"));
    } else if (m_preprocessPage && m_preprocessPage->isInputConfirmed()) {
        m_stepNavigator->setStepStatus(2, WorkflowStepState::Ready,
                                       QStringLiteral("输入文件已确认，可以开始预处理"));
    } else {
        m_stepNavigator->setStepStatus(2, WorkflowStepState::Blocked,
                                       QStringLiteral("请先确认 IQRAW/CAP 输入文件"),
                                       QStringLiteral("还没有确认输入文件"));
    }

    bool hasPartialSamples = false;
    for (const SampleRecord &sample : appState.samples) {
        const QString processing = sample.processingState.trimmed();
        hasPartialSamples = hasPartialSamples
            || processing == QStringLiteral("部分完成")
            || processing == QStringLiteral("已取消")
            || processing == QStringLiteral("已超时")
            || processing == QStringLiteral("失败")
            || processing.contains(QStringLiteral("部分"));
    }
    if (appState.samples.isEmpty()) {
        m_stepNavigator->setStepStatus(3, WorkflowStepState::Blocked,
                                       QStringLiteral("等待预处理样本"),
                                       QStringLiteral("尚未生成可用于数据集的样本"));
    } else if (!appState.datasetVersions.isEmpty()) {
        m_stepNavigator->setStepStatus(
            3,
            hasPartialSamples ? WorkflowStepState::Warning : WorkflowStepState::Completed,
            hasPartialSamples ? QStringLiteral("已有版本，但仍有部分完成样本待复核")
                              : QStringLiteral("已有数据集版本，可继续训练"));
    } else {
        m_stepNavigator->setStepStatus(3, WorkflowStepState::Ready,
                                       QStringLiteral("请选择类别并生成训练集版本"));
    }

    if (appState.datasetVersions.isEmpty()) {
        m_stepNavigator->setStepStatus(4, WorkflowStepState::Blocked,
                                       QStringLiteral("等待训练集版本"),
                                       QStringLiteral("请先在数据集页生成训练集版本"));
    } else {
        bool hasRunning = false;
        bool hasCompleted = false;
        for (const TrainingRunRecord &run : appState.trainingRuns) {
            hasRunning = hasRunning || run.state == QStringLiteral("训练中");
            hasCompleted = hasCompleted
                || (run.modelMode == QStringLiteral("closed_set")
                    && run.state == QStringLiteral("已完成")
                    && run.modelAvailable
                    && QFileInfo::exists(run.bestModelPath));
        }
        m_stepNavigator->setStepStatus(4, hasRunning ? WorkflowStepState::Running
                                                      : (hasCompleted ? WorkflowStepState::Completed
                                                                      : WorkflowStepState::Ready),
                                       hasRunning ? QStringLiteral("训练正在运行")
                                                  : (hasCompleted ? QStringLiteral("已有可用模型")
                                                                  : QStringLiteral("可以开始 GPU 训练")));
    }

    bool hasModel = false;
    for (const TrainingRunRecord &run : appState.trainingRuns) {
        if (run.modelMode == QStringLiteral("closed_set")
            && run.state == QStringLiteral("已完成")
            && run.modelAvailable
            && QFileInfo::exists(run.bestModelPath)) {
            hasModel = true;
            break;
        }
    }
    m_stepNavigator->setStepStatus(
        5,
        hasModel ? (m_recognitionPage && m_recognitionPage->hasUsableModel()
                        ? WorkflowStepState::Completed
                        : WorkflowStepState::Ready)
                  : WorkflowStepState::Blocked,
        hasModel ? (m_recognitionPage && m_recognitionPage->hasUsableModel()
                        ? QStringLiteral("已加载模型，可以执行识别")
                        : QStringLiteral("选择样本后可以执行识别"))
                 : QStringLiteral("等待训练完成的最佳模型"),
        hasModel ? QString() : QStringLiteral("没有可用的已完成模型"));
}

void MainWindow::updateClock()
{
    if (!m_clockLabel) {
        return;
    }
    m_clockLabel->setText(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}
