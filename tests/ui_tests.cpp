#include <QtTest/QtTest>

#include "ui/MainWindow.h"
#include "ui/pages/CapturePage.h"
#include "ui/pages/DatasetPage.h"
#include "ui/pages/PreprocessPage.h"
#include "ui/pages/RecognitionPage.h"
#include "ui/pages/TrainPage.h"
#include "ui/widgets/StepNavigator.h"

#include <memory>

#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>

class FakeUsrpDeviceService : public IUsrpDeviceService
{
public:
    explicit FakeUsrpDeviceService(int captureDelayMs = 0)
        : captureDelayMs(captureDelayMs)
    {
    }

    QVector<UsrpDeviceInfo> discover(QString *error = nullptr) const override
    {
        Q_UNUSED(error)
        UsrpDeviceInfo info;
        info.serial = QStringLiteral("30FB59C");
        info.name = QStringLiteral("MyB210");
        info.product = QStringLiteral("B210");
        info.type = QStringLiteral("b200");
        return {info};
    }

    UsrpCaptureResult captureToCap(const UsrpCaptureConfig &config) const override
    {
        if (captureDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(captureDelayMs));
        }
        lastConfig = config;
        UsrpCaptureResult result;
        result.success = true;
        result.message = QStringLiteral("capture complete");
        result.outputFilePath = config.outputFilePath;
        result.sampleCount = 128;
        result.fileSize = 1024;
        result.logs = {QStringLiteral("wrote CAP: %1").arg(config.outputFilePath)};
        return result;
    }

    mutable UsrpCaptureConfig lastConfig;
    int captureDelayMs = 0;
};

class UiTests : public QObject
{
    Q_OBJECT

private slots:
    void stepNavigatorShowsNumberedStepsAndCompletion();
    void mainWindowContainsWorkflowStatusLabels();
    void preprocessPageExposesDemoAction();
    void preprocessPageUsesThreeColumnWorkbenchLayout();
    void preprocessPageSeparatesParametersAndResultTabs();
    void preprocessPageUsesFlatPanelsForSummaryAndParameters();
    void preprocessPageKeepsParameterInputsReadable();
    void preprocessPageUsesPageLevelVerticalScrolling();
    void capturePageDiscoversDevicesAndRunsCapture();
    void capturePageSwitchesDeviceTypesAndPassesUsrpParameters();
    void capturePageUsesConsoleLayoutForInstrumentControls();
    void capturePageSeparatesBasicAdvancedParamsAndResultTabs();
    void capturePageUsesThreeColumnWorkbenchLayout();
    void capturePageUsesPageLevelVerticalScrolling();
    void capturePageRejectsInvalidUsrpParameters();
    void capturePageRunsCaptureWithoutBlockingUi();
    void mainWindowShowsCaptureWorkbench();
    void mainWindowStaysAliveAfterShowing();
    void stackedPagesStayAliveAfterShowing();
};

void UiTests::stepNavigatorShowsNumberedStepsAndCompletion()
{
    StepNavigator navigator;
    navigator.setSteps({QStringLiteral("Capture"), QStringLiteral("Preprocess"), QStringLiteral("Dataset")});
    navigator.setCurrentIndex(1);

    const auto buttons = navigator.findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 3);
    QCOMPARE(buttons.at(0)->text(), QStringLiteral("1  Capture"));
    QCOMPARE(buttons.at(1)->text(), QStringLiteral("2  Preprocess"));
    QCOMPARE(buttons.at(2)->text(), QStringLiteral("3  Dataset"));
    QVERIFY(buttons.at(0)->property("completed").toBool());
    QVERIFY(buttons.at(1)->isChecked());
    QVERIFY(!buttons.at(2)->property("completed").toBool());
}

void UiTests::mainWindowContainsWorkflowStatusLabels()
{
    MainWindow window;

    QVERIFY(window.findChild<QStackedWidget *>(QStringLiteral("WorkflowStack")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("CurrentStepLabel")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("ClockLabel")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("DataPathLabel")) != nullptr);
}

void UiTests::preprocessPageExposesDemoAction()
{
    PreprocessPage page;
    auto *demoButton = page.findChild<QPushButton *>(QStringLiteral("DemoPreprocessButton"));
    auto *title = page.findChild<QLabel *>(QStringLiteral("PageTitle"));
    QVERIFY(demoButton != nullptr);
    QVERIFY(title != nullptr);
    QCOMPARE(demoButton->text(), QStringLiteral("演示生成样本"));
    QVERIFY(!title->text().contains(QChar(0xfffd)));
    QCOMPARE(title->text(), QStringLiteral("信号预处理"));
}

void UiTests::preprocessPageUsesThreeColumnWorkbenchLayout()
{
    PreprocessPage page;

    QVERIFY(page.findChild<QFrame *>(QStringLiteral("PreprocessInputColumn")) != nullptr);
    QVERIFY(page.findChild<QFrame *>(QStringLiteral("PreprocessParametersColumn")) != nullptr);
    QVERIFY(page.findChild<QFrame *>(QStringLiteral("PreprocessRunColumn")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessInputColumn")) == nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessParametersColumn")) == nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessRunColumn")) == nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("PreprocessInputColumnTitle")) != nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("PreprocessParametersColumnTitle")) != nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("PreprocessRunColumnTitle")) != nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("PreprocessValidationMessage")) != nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("PreprocessRunStateLabel")) != nullptr);
    QVERIFY(page.findChild<QPlainTextEdit *>(QStringLiteral("PreprocessLog")) != nullptr);
}

void UiTests::preprocessPageSeparatesParametersAndResultTabs()
{
    PreprocessPage page;
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *advancedPanel = page.findChild<QWidget *>(QStringLiteral("PreprocessAdvancedParametersPanel"));
    auto *advancedToggle = page.findChild<QPushButton *>(QStringLiteral("PreprocessAdvancedParametersToggle"));
    auto *resultsTabs = page.findChild<QTabWidget *>(QStringLiteral("PreprocessResultsTabs"));
    auto *segmentTable = page.findChild<QTableWidget *>(QStringLiteral("PreprocessSegmentTable"));
    auto *sampleTable = page.findChild<QTableWidget *>(QStringLiteral("PreprocessSampleTable"));

    QVERIFY(advancedPanel != nullptr);
    QVERIFY(advancedToggle != nullptr);
    QVERIFY(resultsTabs != nullptr);
    QVERIFY(segmentTable != nullptr);
    QVERIFY(sampleTable != nullptr);
    QVERIFY(!advancedPanel->isVisible());
    QCOMPARE(resultsTabs->count(), 2);
    QCOMPARE(resultsTabs->tabText(0), QStringLiteral("候选片段"));
    QCOMPARE(resultsTabs->tabText(1), QStringLiteral("输出样本"));

    QTest::mouseClick(advancedToggle, Qt::LeftButton);
    QVERIFY(advancedPanel->isVisible());
    QCOMPARE(advancedToggle->text(), QStringLiteral("隐藏高级参数"));
}

void UiTests::preprocessPageUsesFlatPanelsForSummaryAndParameters()
{
    PreprocessPage page;

    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessSummaryPanel")) != nullptr);
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessPathParametersPanel")) != nullptr);
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessBasicParametersPanel")) != nullptr);
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessAdvancedParametersPanel")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessSummaryPanel")) == nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessPathParametersPanel")) == nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessBasicParametersPanel")) == nullptr);
}

void UiTests::preprocessPageKeepsParameterInputsReadable()
{
    PreprocessPage page;
    page.resize(1600, 620);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    const QStringList inputNames = {
        QStringLiteral("PreprocessOutputDirEdit"),
        QStringLiteral("PreprocessModelPathEdit"),
        QStringLiteral("PreprocessSliceLengthEdit"),
        QStringLiteral("PreprocessEnergyThresholdEdit"),
        QStringLiteral("PreprocessAiConfidenceEdit"),
    };

    for (const QString &name : inputNames) {
        auto *edit = page.findChild<QLineEdit *>(name);
        QVERIFY2(edit != nullptr, qPrintable(name));
        QVERIFY2(edit->minimumHeight() >= 42, qPrintable(QStringLiteral("%1 minimum height is %2").arg(name).arg(edit->minimumHeight())));
        QVERIFY2(edit->height() >= 38, qPrintable(QStringLiteral("%1 height is %2").arg(name).arg(edit->height())));
    }

    auto *capPathEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessCapPathEdit"));
    auto *outputDirEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessOutputDirEdit"));
    auto *modelPathEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessModelPathEdit"));
    QVERIFY(capPathEdit != nullptr);
    QVERIFY(outputDirEdit != nullptr);
    QVERIFY(modelPathEdit != nullptr);
    QVERIFY(capPathEdit->minimumWidth() >= 520);
    QVERIFY(outputDirEdit->minimumWidth() >= 520);
    QVERIFY(modelPathEdit->minimumWidth() >= 520);

    auto *pathPanel = page.findChild<QWidget *>(QStringLiteral("PreprocessPathParametersPanel"));
    auto *basicPanel = page.findChild<QWidget *>(QStringLiteral("PreprocessBasicParametersPanel"));
    auto *resultsPanel = page.findChild<QGroupBox *>(QStringLiteral("PreprocessResultsPanel"));
    QVERIFY(pathPanel != nullptr);
    QVERIFY(basicPanel != nullptr);
    QVERIFY(resultsPanel != nullptr);
    QVERIFY(pathPanel->minimumHeight() >= 150);
    QVERIFY(basicPanel->minimumHeight() >= 150);
    QVERIFY(resultsPanel->maximumHeight() <= 230);
}

void UiTests::preprocessPageUsesPageLevelVerticalScrolling()
{
    PreprocessPage page;
    page.resize(1280, 640);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *scrollArea = page.findChild<QScrollArea *>(QStringLiteral("WorkflowPageScrollArea"));
    QVERIFY(scrollArea != nullptr);
    QCOMPARE(scrollArea->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(scrollArea->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(scrollArea->widgetResizable());
}

void UiTests::capturePageDiscoversDevicesAndRunsCapture()
{
    auto service = std::make_unique<FakeUsrpDeviceService>();
    auto *servicePtr = service.get();
    CapturePage page(std::move(service));

    auto *discoverButton = page.findChild<QPushButton *>(QStringLiteral("DiscoverUsrpButton"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *deviceTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpDeviceTable"));
    auto *durationCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpDurationCombo"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(discoverButton != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(deviceTable != nullptr);
    QVERIFY(durationCombo != nullptr);
    QVERIFY(logOutput != nullptr);
    QCOMPARE(deviceTypeCombo->currentText(), QStringLiteral("USRP B210"));

    QTest::mouseClick(discoverButton, Qt::LeftButton);
    QCOMPARE(deviceTable->rowCount(), 1);
    QCOMPARE(deviceTable->item(0, 0)->text(), QStringLiteral("B210 / MyB210 / SN 30FB59C"));

    durationCombo->setCurrentText(QStringLiteral("0.1"));
    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return logOutput->toPlainText().contains(QStringLiteral("capture complete"));
    }, 2000));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("capture complete")));
    QCOMPARE(servicePtr->lastConfig.durationSeconds, 0.1);
    QVERIFY(servicePtr->lastConfig.outputFilePath.endsWith(QStringLiteral(".cap")));
}

void UiTests::capturePageSwitchesDeviceTypesAndPassesUsrpParameters()
{
    auto service = std::make_unique<FakeUsrpDeviceService>();
    auto *servicePtr = service.get();
    CapturePage page(std::move(service));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *discoverButton = page.findChild<QPushButton *>(QStringLiteral("DiscoverUsrpButton"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *deviceTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpDeviceTable"));
    auto *antennaCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpAntennaCombo"));
    auto *channelCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpChannelCombo"));
    auto *centerFreqCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpCenterFrequencyCombo"));
    auto *sampleRateCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpSampleRateCombo"));
    auto *bandwidthCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpBandwidthCombo"));
    auto *gainCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpGainCombo"));
    auto *durationCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpDurationCombo"));
    auto *cpuFormatCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpCpuFormatCombo"));
    auto *wireFormatCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpWireFormatCombo"));
    auto *placeholder3943b = page.findChild<QLabel *>(QStringLiteral("Device3943BPlaceholder"));
    auto *deviceConfigStack = page.findChild<QStackedWidget *>(QStringLiteral("DeviceConfigStack"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(discoverButton != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(deviceTable != nullptr);
    QVERIFY(antennaCombo != nullptr);
    QVERIFY(channelCombo != nullptr);
    QVERIFY(centerFreqCombo != nullptr);
    QVERIFY(sampleRateCombo != nullptr);
    QVERIFY(bandwidthCombo != nullptr);
    QVERIFY(gainCombo != nullptr);
    QVERIFY(durationCombo != nullptr);
    QVERIFY(cpuFormatCombo != nullptr);
    QVERIFY(wireFormatCombo != nullptr);
    QVERIFY(placeholder3943b != nullptr);
    QVERIFY(deviceConfigStack != nullptr);
    QVERIFY(logOutput != nullptr);

    deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    QVERIFY(!discoverButton->isEnabled());
    QVERIFY(!captureButton->isEnabled());
    QCOMPARE(deviceConfigStack->currentIndex(), 1);
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B")));

    deviceTypeCombo->setCurrentText(QStringLiteral("USRP B210"));
    QVERIFY(discoverButton->isEnabled());
    QTest::mouseClick(discoverButton, Qt::LeftButton);
    QCOMPARE(deviceTable->rowCount(), 1);

    antennaCombo->setCurrentText(QStringLiteral("RX2"));
    channelCombo->setCurrentText(QStringLiteral("0"));
    centerFreqCombo->setCurrentText(QStringLiteral("1090000000"));
    sampleRateCombo->setCurrentText(QStringLiteral("8000000"));
    bandwidthCombo->setCurrentText(QStringLiteral("6000000"));
    gainCombo->setCurrentText(QStringLiteral("20"));
    durationCombo->setCurrentText(QStringLiteral("0.25"));
    cpuFormatCombo->setCurrentText(QStringLiteral("sc16"));
    wireFormatCombo->setCurrentText(QStringLiteral("sc16"));

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return servicePtr->lastConfig.outputFilePath.endsWith(QStringLiteral(".cap"));
    }, 2000));
    QCOMPARE(servicePtr->lastConfig.antenna, QStringLiteral("RX2"));
    QCOMPARE(servicePtr->lastConfig.channel, 0);
    QCOMPARE(servicePtr->lastConfig.centerFrequencyHz, 1090000000.0);
    QCOMPARE(servicePtr->lastConfig.sampleRateHz, 8000000.0);
    QCOMPARE(servicePtr->lastConfig.bandwidthHz, 6000000.0);
    QCOMPARE(servicePtr->lastConfig.gainDb, 20.0);
    QCOMPARE(servicePtr->lastConfig.durationSeconds, 0.25);
    QCOMPARE(servicePtr->lastConfig.cpuFormat, QStringLiteral("sc16"));
    QCOMPARE(servicePtr->lastConfig.wireFormat, QStringLiteral("sc16"));
}

void UiTests::capturePageUsesConsoleLayoutForInstrumentControls()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());

    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("UsrpSourceButton")) != nullptr);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("Device3943BSourceButton")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureDeviceColumn")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureParametersColumn")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureRunColumn")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureResultsPanel")) != nullptr);
    QVERIFY(page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo")) != nullptr);
    QVERIFY(!page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"))->isVisible());
}

void UiTests::capturePageSeparatesBasicAdvancedParamsAndResultTabs()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *basicPanel = page.findChild<QGroupBox *>(QStringLiteral("UsrpBasicParametersPanel"));
    auto *advancedPanel = page.findChild<QWidget *>(QStringLiteral("UsrpAdvancedParametersPanel"));
    auto *advancedToggle = page.findChild<QPushButton *>(QStringLiteral("UsrpAdvancedParametersToggle"));
    auto *editableHint = page.findChild<QLabel *>(QStringLiteral("UsrpEditableParametersHint"));
    auto *resultsTabs = page.findChild<QTabWidget *>(QStringLiteral("CaptureResultsTabs"));

    QVERIFY(basicPanel != nullptr);
    QVERIFY(advancedPanel != nullptr);
    QVERIFY(advancedToggle != nullptr);
    QVERIFY(editableHint != nullptr);
    QVERIFY(resultsTabs != nullptr);
    QVERIFY(editableHint->text().contains(QStringLiteral("直接输入")));
    QVERIFY(!advancedPanel->isVisible());
    QCOMPARE(resultsTabs->count(), 2);
    QCOMPARE(resultsTabs->tabText(0), QStringLiteral("设备列表"));
    QCOMPARE(resultsTabs->tabText(1), QStringLiteral("采集记录"));

    QTest::mouseClick(advancedToggle, Qt::LeftButton);
    QVERIFY(advancedPanel->isVisible());
}

void UiTests::capturePageUsesThreeColumnWorkbenchLayout()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());

    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureDeviceColumn")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureParametersColumn")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("CaptureRunColumn")) != nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("UsrpConnectionSummary")) != nullptr);
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("UsrpCaptureValidationMessage")) != nullptr);
}

void UiTests::capturePageUsesPageLevelVerticalScrolling()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());
    page.resize(1280, 640);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *scrollArea = page.findChild<QScrollArea *>(QStringLiteral("WorkflowPageScrollArea"));
    QVERIFY(scrollArea != nullptr);
    QCOMPARE(scrollArea->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(scrollArea->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(scrollArea->widgetResizable());
}

void UiTests::capturePageRejectsInvalidUsrpParameters()
{
    auto service = std::make_unique<FakeUsrpDeviceService>();
    auto *servicePtr = service.get();
    CapturePage page(std::move(service));

    auto *discoverButton = page.findChild<QPushButton *>(QStringLiteral("DiscoverUsrpButton"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *sampleRateCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpSampleRateCombo"));
    auto *validationMessage = page.findChild<QLabel *>(QStringLiteral("UsrpCaptureValidationMessage"));

    QVERIFY(discoverButton != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(sampleRateCombo != nullptr);
    QVERIFY(validationMessage != nullptr);

    QTest::mouseClick(discoverButton, Qt::LeftButton);
    sampleRateCombo->setCurrentText(QStringLiteral("abc"));
    QTest::mouseClick(captureButton, Qt::LeftButton);

    QVERIFY(validationMessage->text().contains(QStringLiteral("采样率")));
    QVERIFY(validationMessage->property("kind").toString() == QStringLiteral("error"));
    QVERIFY(servicePtr->lastConfig.outputFilePath.isEmpty());
}

void UiTests::capturePageRunsCaptureWithoutBlockingUi()
{
    auto service = std::make_unique<FakeUsrpDeviceService>(250);
    CapturePage page(std::move(service));

    auto *discoverButton = page.findChild<QPushButton *>(QStringLiteral("DiscoverUsrpButton"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *taskLabel = page.findChild<QLabel *>(QStringLiteral("CaptureTaskStateLabel"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(discoverButton != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(taskLabel != nullptr);
    QVERIFY(logOutput != nullptr);

    QTest::mouseClick(discoverButton, Qt::LeftButton);
    QTest::mouseClick(captureButton, Qt::LeftButton);

    QVERIFY(!captureButton->isEnabled());
    QCOMPARE(taskLabel->text(), QStringLiteral("采集中"));
    QVERIFY(!logOutput->toPlainText().contains(QStringLiteral("capture complete")));
    QVERIFY(QTest::qWaitFor([&]() {
        return logOutput->toPlainText().contains(QStringLiteral("capture complete"));
    }, 2000));
    QCOMPARE(taskLabel->text(), QStringLiteral("完成"));
}

void UiTests::mainWindowShowsCaptureWorkbench()
{
    MainWindow window;

    QVERIFY(window.findChild<QGroupBox *>(QStringLiteral("CaptureDeviceColumn")) != nullptr);
    QVERIFY(window.findChild<QGroupBox *>(QStringLiteral("CaptureParametersColumn")) != nullptr);
    QVERIFY(window.findChild<QGroupBox *>(QStringLiteral("CaptureRunColumn")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("UsrpConnectionSummary")) != nullptr);
}

void UiTests::mainWindowStaysAliveAfterShowing()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTest::qWait(5500);
    QVERIFY(window.isVisible());
}

void UiTests::stackedPagesStayAliveAfterShowing()
{
    QStackedWidget stack;
    stack.addWidget(new CapturePage(&stack));
    stack.addWidget(new PreprocessPage(&stack));
    stack.addWidget(new DatasetPage(&stack));
    stack.addWidget(new TrainPage(&stack));
    stack.addWidget(new RecognitionPage(&stack));
    stack.show();
    QVERIFY(QTest::qWaitForWindowExposed(&stack));
    QTest::qWait(5500);
    QVERIFY(stack.isVisible());
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    UiTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ui_tests.moc"
