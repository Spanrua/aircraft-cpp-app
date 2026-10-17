#include <QtTest/QtTest>

#include "services/CeyearVisaService.h"
#include "ui/MainWindow.h"
#include "ui/pages/CapturePage.h"
#include "ui/pages/DatasetPage.h"
#include "ui/pages/PreprocessPage.h"
#include "ui/pages/RecognitionPage.h"
#include "ui/pages/SweepPage.h"
#include "ui/pages/TrainPage.h"
#include "ui/widgets/StepNavigator.h"
#include "ui/widgets/MetricCard.h"
#include "ui/widgets/SpectrogramReviewWidget.h"
#include "ui/widgets/WaterfallWidget.h"

#include <memory>

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLibraryInfo>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTemporaryDir>
#include <QImage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

static QByteArray minimalCeyearIfIqPacket()
{
    QByteArray packet(86 + 8, '\0');
    packet[0] = '\x00';
    packet[1] = '\x0E';
    packet[2] = '\xB2';
    packet[3] = '\x00';
    packet[12] = static_cast<char>((packet.size() >> 24) & 0xFF);
    packet[13] = static_cast<char>((packet.size() >> 16) & 0xFF);
    packet[14] = static_cast<char>((packet.size() >> 8) & 0xFF);
    packet[15] = static_cast<char>(packet.size() & 0xFF);
    packet[16] = '\x03';
    packet[17] = '\x85';
    packet[23] = '\x3A';
    packet[86] = '\x01';
    packet[87] = '\x00';
    packet[88] = '\xFF';
    packet[89] = '\xFF';
    packet[90] = '\x02';
    packet[91] = '\x00';
    packet[92] = '\xFE';
    packet[93] = '\xFF';
    return packet;
}

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
        result.logs.clear();
        result.logs.append(QStringLiteral("wrote CAP: %1").arg(config.outputFilePath));
        return result;
    }

    mutable UsrpCaptureConfig lastConfig;
    int captureDelayMs = 0;
};

class FakeCeyearVisaService : public ICeyearVisaService
{
public:
    explicit FakeCeyearVisaService(int triggerDelayMs = 0)
        : triggerDelayMs(triggerDelayMs)
    {
    }

    CeyearStatusResult queryStatus(const QString &resource) const override
    {
        lastResource = resource;
        CeyearStatusResult result;
        result.success = true;
        result.resource = resource;
        result.items.append(CeyearStatusItem{QStringLiteral("Identity"), QStringLiteral("*IDN?"), QStringLiteral("Ceyear Technologies,3943B,,V2.1.0"), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("Center frequency"), QStringLiteral("FREQuency?"), QStringLiteral("9.362000E+07"), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.message = QStringLiteral("3943B status ok");
        return result;
    }

    CeyearStatusResult applyControl(const QString &resource, const CeyearControlConfig &config) const override
    {
        lastResource = resource;
        lastControl = config;
        applyControlCount++;
        CeyearStatusResult result;
        result.success = true;
        result.resource = resource;
        result.items.append(CeyearStatusItem{QStringLiteral("Center frequency"), QStringLiteral("FREQuency?"), QString::number(config.centerFrequencyHz, 'f', 0), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("Frequency span"), QStringLiteral("FREQuency:SPAN?"), QString::number(config.frequencySpanHz, 'f', 0), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("Demodulation"), QStringLiteral("DEModulation?"), config.demodulation, true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("Detector"), QStringLiteral("DETector?"), config.detector, true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.message = QStringLiteral("3943B control ok");
        return result;
    }

    CeyearCaptureResult configureCapturePath(const QString &resource, const CeyearCaptureConfig &config) const override
    {
        lastResource = resource;
        lastCapture = config;
        configureCapturePathCount++;

        CeyearCaptureResult result;
        result.success = true;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_PATH_CONFIGURATION");
        result.sampleCount = 0;
        result.bytesReceived = 0;
        result.items.append(CeyearStatusItem{QStringLiteral("PC target"), QStringLiteral("TRACe:TCP:TAG"), config.pcReceiveIpAddress, true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("Report path"), QStringLiteral("TRACe:PATH?"), config.reportPath, true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.logs.append(QStringLiteral("fake 3943B path configured"));
        result.message = QStringLiteral("3943B path configured");
        return result;
    }

    CeyearCaptureResult startManualIqReport(const QString &resource, const CeyearCaptureConfig &config) const override
    {
        lastResource = resource;
        lastCapture = config;
        startManualIqReportCount++;

        CeyearCaptureResult result;
        result.success = true;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_MANUAL_IQ_REPORT");
        result.sampleCount = 0;
        result.bytesReceived = 8192;
        result.items.append(CeyearStatusItem{QStringLiteral("IQ report"), QStringLiteral("TRACe:IQ:DATA:REPort?"), QStringLiteral("MANual"), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.logs.append(QStringLiteral("fake 3943B manual IQ report completed"));
        result.message = QStringLiteral("3943B manual IQ report completed");
        return result;
    }

    CeyearCaptureResult triggerIqReport(const QString &resource, const CeyearCaptureConfig &config) const override
    {
        if (triggerDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(triggerDelayMs));
        }
        lastResource = resource;
        lastCapture = config;
        triggerIqReportCount++;

        CeyearCaptureResult result;
        result.success = true;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_TRIGGER_IQ_REPORT");
        result.sampleCount = 0;
        result.bytesReceived = 16470;
        result.outputFilePath = config.outputFilePath;
        QDir().mkpath(QFileInfo(config.outputFilePath).absolutePath());
        QFile rawFile(config.outputFilePath);
        if (rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            rawFile.write(minimalCeyearIfIqPacket());
            rawFile.close();
        }
        result.items.append(CeyearStatusItem{QStringLiteral("IQ report"), QStringLiteral("TRACe:IQ:DATA:REPort?"), QStringLiteral("MAN"), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.logs.append(QStringLiteral("fake 3943B trigger IQ report completed"));
        result.message = QStringLiteral("3943B trigger IQ report completed and stopped");
        return result;
    }

    CeyearCaptureResult startCapture(const QString &resource, const CeyearCaptureConfig &config) const override
    {
        lastResource = resource;
        lastCapture = config;
        startCaptureCount++;

        CeyearCaptureResult result;
        result.success = true;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_CAPTURE_001");
        result.sampleCount = 0;
        result.bytesReceived = 8192;
        result.items.append(CeyearStatusItem{QStringLiteral("Record state"), QStringLiteral("TRACe:RECord:STATe?"), QStringLiteral("1"), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.logs.append(QStringLiteral("fake 3943B capture completed and stopped"));
        result.message = QStringLiteral("3943B capture completed and stopped");
        return result;
    }

    CeyearCaptureResult stopCapture(const QString &resource) const override
    {
        lastResource = resource;
        stopCaptureCount++;

        CeyearCaptureResult result;
        result.success = true;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_CAPTURE_001");
        result.items.append(CeyearStatusItem{QStringLiteral("Record state"), QStringLiteral("TRACe:RECord:STATe?"), QStringLiteral("0"), true, {}});
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.message = QStringLiteral("3943B capture stopped");
        return result;
    }

    void requestStop() const override
    {
        requestStopCount++;
    }

    CeyearCaptureResult startPscSweep(const QString &resource, const CeyearPscSweepConfig &config) const override
    {
        if (pscSweepDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(pscSweepDelayMs));
        }
        lastResource = resource;
        lastPscSweep = config;
        startPscSweepCount++;

        CeyearCaptureResult result;
        result.success = pscSweepStartSucceeds && pscSweepSystemErrorClear;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_PSC_SWEEP");
        if (!pscSweepCommandErrorResponse.isEmpty()) {
            result.items.append(CeyearStatusItem{QStringLiteral("After PSC command: FREQ:PSC:STAR 900000000 Hz"),
                                                 QStringLiteral("SYSTem:ERRor?"),
                                                 pscSweepCommandErrorResponse,
                                                 true,
                                                 {}});
        }
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), pscSweepSystemErrorResponse, true, {}});
        if (!pscSweepStartSucceeds) {
            result.message = QStringLiteral("viOpen failed");
        } else if (!pscSweepSystemErrorClear) {
            result.message = QStringLiteral("3943B PSC sweep started, but SYSTem:ERRor? is not clear");
        } else {
            result.message = QStringLiteral("3943B PSC sweep started");
        }
        return result;
    }

    CeyearCaptureResult stopPscSweep(const QString &resource) const override
    {
        lastResource = resource;
        stopPscSweepCount++;

        CeyearCaptureResult result;
        result.success = true;
        result.resource = resource;
        result.captureName = QStringLiteral("3943B_PSC_SWEEP");
        result.items.append(CeyearStatusItem{QStringLiteral("System error"), QStringLiteral("SYSTem:ERRor?"), QStringLiteral("00"), true, {}});
        result.message = QStringLiteral("3943B PSC sweep stopped");
        return result;
    }

    mutable QString lastResource;
    mutable CeyearControlConfig lastControl;
    mutable CeyearCaptureConfig lastCapture;
    mutable CeyearPscSweepConfig lastPscSweep;
    mutable int applyControlCount = 0;
    mutable int configureCapturePathCount = 0;
    mutable int startManualIqReportCount = 0;
    mutable int triggerIqReportCount = 0;
    mutable int startCaptureCount = 0;
    mutable int stopCaptureCount = 0;
    mutable int requestStopCount = 0;
    mutable int startPscSweepCount = 0;
    mutable int stopPscSweepCount = 0;
    int triggerDelayMs = 0;
    int pscSweepDelayMs = 0;
    bool pscSweepStartSucceeds = true;
    bool pscSweepSystemErrorClear = true;
    QString pscSweepCommandErrorResponse;
    QString pscSweepSystemErrorResponse = QStringLiteral("00");
};

class UiTests : public QObject
{
    Q_OBJECT

private slots:
    void waterfallWidgetKeepsStableFrameBuffer();
    void waterfallWidgetUsesGreenNoiseFloorAndWarmPeaks();
    void waterfallWidgetMapsBinsByFrequencyRange();
    void waterfallWidgetDrawsPersistentFrequencyAsVerticalTrace();
    void waterfallWidgetDrawsTimeFrequencyAxes();
    void waterfallWidgetClearsBufferedFrames();
    void waterfallWidgetTracksWidebandAlertBand();
    void waterfallWidgetRendersSingleBinWidebandAlertFromCenterAndBandwidth();
    void waterfallWidgetClipsWidebandAlertBandToPlot();
    void sweepPageExposesPscControlsAndWaterfall();
    void sweepPageEmitsConfirmedCandidateFrequency();
    void sweepPageStartsPscSweepFromDefaultSegments();
    void sweepPageStartsPscSweepWithoutBlockingUi();
    void sweepPageCommitsEditedFrequencyCellsBeforeStartingSweep();
    void sweepPagePassesConfiguredHitThresholdToPscSweep();
    void sweepPageShowsPscSystemErrorDetails();
    void sweepPageShowsPscStartFailure();
    void sweepPageUpdatesWaterfallAndCandidatesFromPscFrame();
    void sweepPageClearsWaterfallWhenStartingNewSweep();
    void sweepPageSavesPscFramesToCsv();
    void stepNavigatorShowsNumberedStepsAndCompletion();
    void stepNavigatorExposesBlockedReason();
    void mainWindowContainsWorkflowStatusLabels();
    void preprocessPageExposesRealProcessingControls();
    void preprocessPageUsesThreeColumnWorkbenchLayout();
    void preprocessPageSeparatesParametersAndResultTabs();
    void preprocessPageUsesFlatPanelsForSummaryAndParameters();
    void preprocessPageKeepsParameterInputsReadable();
    void preprocessPageUsesPageLevelVerticalScrolling();
    void datasetPageExposesCategorySelectionWorkbench();
    void datasetPageExposesSpectrogramReviewWidget();
    void datasetPageUsesStandardLabelSelector();
    void trainPageExposesBackendControls();
    void recognitionPageExposesModelAndSampleWorkflow();
    void capturePageDiscoversDevicesAndRunsCapture();
    void capturePageSwitchesDeviceTypesAndPassesUsrpParameters();
    void capturePageUsesConsoleLayoutForInstrumentControls();
    void capturePageUsesDeviceSpecificParameterPanels();
    void capturePageSeparatesBasicAdvancedParamsAndResultTabs();
    void capturePageUsesThreeColumnWorkbenchLayout();
    void capturePageUsesPageLevelVerticalScrolling();
    void capturePageUsesColumnScrollContainersAtMinimumSize();
    void capturePageKeepsResultsTabsReadableAtMinimumSize();
    void capturePageRejectsInvalidUsrpParameters();
    void capturePageRunsCaptureWithoutBlockingUi();
    void capturePageQueries3943BStatusInsideDeviceOption();
    void capturePageDoesNotExposeSeparate3943BControlWrite();
    void capturePageDefaults3943BToVerifiedManualIqPath();
    void capturePageExposes3943BPointCountOnly();
    void capturePageContinuous3943BCaptureSchedulesNextFile();
    void capturePageContinuousCaptureStartsNextRoundImmediately();
    void capturePageConfigures3943BPathFromMainButton();
    void capturePageIgnoresSecond3943BStartWhileBusy();
    void capturePageCanRun3943BCaptureTwice();
    void capturePagePasses3943BReportPathSeparatelyFromPcProtocol();
    void capturePageStops3943BRecording();
    void capturePageKeeps3943BStopEnabledDuringBusyState();
    void mainWindowStartsWithSweepLocationBeforeCapture();
    void mainWindowPassesSweepFrequencyToCapturePage();
    void mainWindowShowsCaptureWorkbench();
    void mainWindowStaysAliveAfterShowing();
    void stackedPagesStayAliveAfterShowing();
};

void UiTests::waterfallWidgetKeepsStableFrameBuffer()
{
    WaterfallWidget widget;
    widget.setMaximumRows(2);
    widget.appendFrame({{{900000000.0, -70.0}, {901000000.0, -35.0}}});
    widget.appendFrame({{{900000000.0, -65.0}, {901000000.0, -32.0}}});
    widget.appendFrame({{{900000000.0, -80.0}, {901000000.0, -31.0}}});

    QCOMPARE(widget.frameCount(), 2);
    QVERIFY(widget.minimumSizeHint().width() >= 320);
}

void UiTests::waterfallWidgetUsesGreenNoiseFloorAndWarmPeaks()
{
    WaterfallWidget widget;
    widget.resize(480, 80);
    widget.setMaximumRows(1);
    widget.setFrequencyRange(900000000.0, 901000000.0);
    widget.appendFrame({{{900000000.0, -45.0}, {901000000.0, 45.0}}});

    QImage rendered(widget.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);

    const QRect plotArea = widget.plotArea();
    const QColor lowLevel = rendered.pixelColor(plotArea.left() + plotArea.width() / 4, plotArea.center().y());
    const QColor highLevel = rendered.pixelColor(plotArea.left() + plotArea.width() * 3 / 4, plotArea.center().y());

    QVERIFY2(lowLevel.green() > lowLevel.red() && lowLevel.green() > lowLevel.blue(),
             qPrintable(QStringLiteral("noise floor should be green, got rgb(%1,%2,%3)")
                            .arg(lowLevel.red())
                            .arg(lowLevel.green())
                            .arg(lowLevel.blue())));
    QVERIFY2(highLevel.red() > highLevel.blue() * 2,
             qPrintable(QStringLiteral("high level color should be warm, got rgb(%1,%2,%3)")
                            .arg(highLevel.red())
                            .arg(highLevel.green())
                            .arg(highLevel.blue())));
}

void UiTests::waterfallWidgetDrawsPersistentFrequencyAsVerticalTrace()
{
    WaterfallWidget widget;
    widget.resize(300, 90);
    widget.setMaximumRows(3);
    widget.setFrequencyRange(900000000.0, 1100000000.0);

    const CeyearPscFrame frame = {{{900000000.0, -45.0}, {1000000000.0, 45.0}, {1100000000.0, -45.0}}};
    widget.appendFrame(frame);
    widget.appendFrame(frame);
    widget.appendFrame(frame);

    QImage rendered(widget.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);

    const QRect plotArea = widget.plotArea();
    const int traceX = plotArea.center().x();
    const QVector<int> sampleYs = {
        plotArea.top() + plotArea.height() / 6,
        plotArea.center().y(),
        plotArea.bottom() - plotArea.height() / 6,
    };
    for (int y : sampleYs) {
        const QColor trace = rendered.pixelColor(traceX, y);
        QVERIFY2(trace.red() > trace.blue() * 2,
                 qPrintable(QStringLiteral("persistent frequency should form a warm vertical trace at y=%1, got rgb(%2,%3,%4)")
                                .arg(y)
                                .arg(trace.red())
                                .arg(trace.green())
                                .arg(trace.blue())));
    }
}

void UiTests::waterfallWidgetMapsBinsByFrequencyRange()
{
    WaterfallWidget widget;
    widget.resize(500, 80);
    widget.setMaximumRows(1);
    widget.setFrequencyRange(900000000.0, 1100000000.0);
    widget.appendFrame({{{100000000.0, 45.0}}});

    QImage rendered(widget.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);

    const QRect plotArea = widget.plotArea();
    const QColor leftEdge = rendered.pixelColor(plotArea.left() + 5, plotArea.center().y());
    QVERIFY2(leftEdge.red() < 80,
             qPrintable(QStringLiteral("out-of-range PSC bin must not be stretched across the plot, got rgb(%1,%2,%3)")
                            .arg(leftEdge.red())
                            .arg(leftEdge.green())
                            .arg(leftEdge.blue())));
}

void UiTests::waterfallWidgetDrawsTimeFrequencyAxes()
{
    WaterfallWidget widget;
    widget.resize(520, 300);
    widget.setMaximumRows(1);
    widget.setFrequencyRange(900000000.0, 1100000000.0);
    widget.appendFrame({{{900000000.0, -72.0}, {1000000000.0, -35.0}, {1100000000.0, -72.0}}});

    const QRect plotArea = widget.plotArea();
    QVERIFY(plotArea.left() > 0);
    QVERIFY(plotArea.top() > 0);
    QVERIFY(plotArea.right() < widget.width() - 1);
    QVERIFY(plotArea.bottom() < widget.height() - 1);

    QImage rendered(widget.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);

    const QColor bottomAxis = rendered.pixelColor(plotArea.center().x(), plotArea.bottom() + 2);
    const QColor leftAxis = rendered.pixelColor(plotArea.left() - 2, plotArea.center().y());
    QVERIFY2(bottomAxis.red() > 120 && bottomAxis.green() > 120 && bottomAxis.blue() > 120,
             qPrintable(QStringLiteral("frequency axis should be visible below the plot, got rgb(%1,%2,%3)")
                            .arg(bottomAxis.red())
                            .arg(bottomAxis.green())
                            .arg(bottomAxis.blue())));
    QVERIFY2(leftAxis.red() > 120 && leftAxis.green() > 120 && leftAxis.blue() > 120,
             qPrintable(QStringLiteral("time axis should be visible left of the plot, got rgb(%1,%2,%3)")
                            .arg(leftAxis.red())
                            .arg(leftAxis.green())
                            .arg(leftAxis.blue())));
}

void UiTests::waterfallWidgetClearsBufferedFrames()
{
    WaterfallWidget widget;
    widget.appendFrame({{{900000000.0, -72.0}}});
    widget.clearFrames();

    QCOMPARE(widget.frameCount(), 0);
}

void UiTests::waterfallWidgetTracksWidebandAlertBand()
{
    WaterfallWidget widget;
    QVERIFY(!widget.hasAlertBand());

    PscWidebandAlert alert;
    alert.startHz = 2400000000.0;
    alert.stopHz = 2420000000.0;
    widget.setAlertBand(alert);

    QVERIFY(widget.hasAlertBand());

    widget.clearAlertBand();
    QVERIFY(!widget.hasAlertBand());
}

void UiTests::waterfallWidgetRendersSingleBinWidebandAlertFromCenterAndBandwidth()
{
    WaterfallWidget widget;
    widget.resize(600, 300);
    widget.setMaximumRows(1);
    widget.setFrequencyRange(2400000000.0, 2500000000.0);
    widget.appendFrame({{{2400000000.0, -90.0}, {2500000000.0, -90.0}}});

    const QRect plot = widget.plotArea();
    const QPoint samplePoint(plot.center().x(), plot.center().y());
    QImage baseline(widget.size(), QImage::Format_ARGB32);
    baseline.fill(Qt::transparent);
    widget.render(&baseline);

    PscWidebandAlert alert;
    alert.startHz = 2450000000.0;
    alert.stopHz = 2450000000.0;
    alert.centerHz = 2450000000.0;
    alert.bandwidthHz = 20000000.0;
    widget.setAlertBand(alert);

    QImage withAlert(widget.size(), QImage::Format_ARGB32);
    withAlert.fill(Qt::transparent);
    widget.render(&withAlert);

    const QColor baselineColor = baseline.pixelColor(samplePoint);
    const QColor alertColor = withAlert.pixelColor(samplePoint);
    QVERIFY2(alertColor.red() > baselineColor.red(),
             qPrintable(QStringLiteral("wideband alert should add orange to the plot, before rgb(%1,%2,%3), after rgb(%4,%5,%6)")
                            .arg(baselineColor.red())
                            .arg(baselineColor.green())
                            .arg(baselineColor.blue())
                            .arg(alertColor.red())
                            .arg(alertColor.green())
                            .arg(alertColor.blue())));

    widget.clearAlertBand();
    QImage cleared(widget.size(), QImage::Format_ARGB32);
    cleared.fill(Qt::transparent);
    widget.render(&cleared);
    QCOMPARE(cleared.pixelColor(samplePoint), baselineColor);
}

void UiTests::waterfallWidgetClipsWidebandAlertBandToPlot()
{
    WaterfallWidget widget;
    widget.resize(600, 300);
    widget.setMaximumRows(1);
    widget.setFrequencyRange(2400000000.0, 2500000000.0);
    widget.appendFrame({{{2400000000.0, -90.0}, {2500000000.0, -90.0}}});

    const QRect plot = widget.plotArea();
    const QPoint insidePoint(plot.left() + 10, plot.center().y());
    const QPoint outsidePoint(plot.left() - 8, plot.center().y());
    QImage baseline(widget.size(), QImage::Format_ARGB32);
    baseline.fill(Qt::transparent);
    widget.render(&baseline);

    PscWidebandAlert alert;
    alert.centerHz = 2395000000.0;
    alert.bandwidthHz = 20000000.0;
    widget.setAlertBand(alert);

    QImage withAlert(widget.size(), QImage::Format_ARGB32);
    withAlert.fill(Qt::transparent);
    widget.render(&withAlert);

    QVERIFY(withAlert.pixelColor(insidePoint).red() > baseline.pixelColor(insidePoint).red());
    QCOMPARE(withAlert.pixelColor(outsidePoint), baseline.pixelColor(outsidePoint));
}

void UiTests::sweepPageExposesPscControlsAndWaterfall()
{
    SweepPage page;

    auto *segmentTable = page.findChild<QTableWidget *>(QStringLiteral("SweepSegmentTable"));
    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopPscSweepButton"));
    auto *saveButton = page.findChild<QPushButton *>(QStringLiteral("SavePscSweepDataButton"));
    auto *waterfall = page.findChild<WaterfallWidget *>(QStringLiteral("PscWaterfallWidget"));
    auto *candidateTable = page.findChild<QTableWidget *>(QStringLiteral("PscCandidateTable"));

    QVERIFY(segmentTable != nullptr);
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(saveButton != nullptr);
    QVERIFY(waterfall != nullptr);
    QVERIFY(candidateTable != nullptr);
    QCOMPARE(segmentTable->rowCount(), 1);
    QCOMPARE(segmentTable->item(0, 0)->text(), QStringLiteral("2385"));
    QCOMPARE(segmentTable->item(0, 1)->text(), QStringLiteral("2415"));
    QCOMPARE(segmentTable->item(0, 2)->text(), QStringLiteral("5"));
    QVERIFY(startButton->isEnabled());
    QVERIFY(!stopButton->isEnabled());
    QVERIFY(!saveButton->isEnabled());
}

void UiTests::sweepPageEmitsConfirmedCandidateFrequency()
{
    SweepPage page;
    QSignalSpy spy(&page, SIGNAL(frequencyConfirmed(double)));

    page.setCandidateRowsForTest({{901000000.0, -38.0, 3}});
    auto *candidateTable = page.findChild<QTableWidget *>(QStringLiteral("PscCandidateTable"));
    auto *confirmButton = page.findChild<QPushButton *>(QStringLiteral("ConfirmPscCandidateButton"));
    QVERIFY(candidateTable != nullptr);
    QVERIFY(confirmButton != nullptr);

    candidateTable->selectRow(0);
    QTest::mouseClick(confirmButton, Qt::LeftButton);

    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).toDouble(), 901000000.0);
}

void UiTests::sweepPageStartsPscSweepFromDefaultSegments()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    auto *servicePtr = service.get();
    SweepPage page(std::move(service));

    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopPscSweepButton"));
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);

    QTest::mouseClick(startButton, Qt::LeftButton);

    QVERIFY(QTest::qWaitFor([&]() {
        return servicePtr->startPscSweepCount == 1 && stopButton->isEnabled();
    }, 1000));
    QCOMPARE(servicePtr->startPscSweepCount, 1);
    QCOMPARE(servicePtr->lastPscSweep.segments.size(), 1);
    QCOMPARE(servicePtr->lastPscSweep.segments.at(0).startHz, 2385000000.0);
    QCOMPARE(servicePtr->lastPscSweep.segments.at(0).stopHz, 2415000000.0);
    QCOMPARE(servicePtr->lastPscSweep.segments.at(0).stepHz, 5000.0);
    QVERIFY(!startButton->isEnabled());
    QVERIFY(stopButton->isEnabled());
}

void UiTests::sweepPageStartsPscSweepWithoutBlockingUi()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    service->pscSweepDelayMs = 250;
    auto *servicePtr = service.get();
    SweepPage page(std::move(service));

    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopPscSweepButton"));
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);

    QElapsedTimer elapsed;
    elapsed.start();
    QTest::mouseClick(startButton, Qt::LeftButton);

    QVERIFY2(elapsed.elapsed() < 150,
             qPrintable(QStringLiteral("Start sweep blocked the UI thread for %1 ms").arg(elapsed.elapsed())));
    QVERIFY(!startButton->isEnabled());
    QVERIFY(!stopButton->isEnabled());
    QVERIFY(QTest::qWaitFor([&]() {
        return servicePtr->startPscSweepCount == 1 && stopButton->isEnabled();
    }, 1000));
}

void UiTests::sweepPageCommitsEditedFrequencyCellsBeforeStartingSweep()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    auto *servicePtr = service.get();
    SweepPage page(std::move(service));

    auto *segmentTable = page.findChild<QTableWidget *>(QStringLiteral("SweepSegmentTable"));
    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    QVERIFY(segmentTable != nullptr);
    QVERIFY(startButton != nullptr);

    auto *startEditor = qobject_cast<QLineEdit *>(segmentTable->cellWidget(0, 0));
    QVERIFY(startEditor != nullptr);
    startEditor->selectAll();
    QTest::keyClicks(startEditor, QStringLiteral("950"));

    auto *stopEditor = qobject_cast<QLineEdit *>(segmentTable->cellWidget(0, 1));
    QVERIFY(stopEditor != nullptr);
    stopEditor->selectAll();
    QTest::keyClicks(stopEditor, QStringLiteral("1050"));

    QTest::mouseClick(startButton, Qt::LeftButton);

    QVERIFY(QTest::qWaitFor([&]() {
        return servicePtr->startPscSweepCount == 1;
    }, 1000));
    QCOMPARE(servicePtr->startPscSweepCount, 1);
    QCOMPARE(servicePtr->lastPscSweep.segments.size(), 1);
    QCOMPARE(servicePtr->lastPscSweep.segments.at(0).startHz, 950000000.0);
    QCOMPARE(servicePtr->lastPscSweep.segments.at(0).stopHz, 1050000000.0);
}

void UiTests::sweepPagePassesConfiguredHitThresholdToPscSweep()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    auto *servicePtr = service.get();
    SweepPage page(std::move(service));

    auto *thresholdEdit = page.findChild<QLineEdit *>(QStringLiteral("PscHitThresholdEdit"));
    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    QVERIFY(thresholdEdit != nullptr);
    QVERIFY(startButton != nullptr);
    QCOMPARE(thresholdEdit->text(), QStringLiteral("-60"));

    thresholdEdit->setText(QStringLiteral("-35.5"));
    QTest::mouseClick(startButton, Qt::LeftButton);

    QVERIFY(QTest::qWaitFor([&]() {
        return servicePtr->startPscSweepCount == 1;
    }, 1000));
    QCOMPARE(servicePtr->startPscSweepCount, 1);
    QCOMPARE(servicePtr->lastPscSweep.hitThresholdDbuv, -35.5);
}

void UiTests::sweepPageShowsPscSystemErrorDetails()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    service->pscSweepSystemErrorClear = false;
    service->pscSweepCommandErrorResponse = QStringLiteral("402 CMD ERR");
    service->pscSweepSystemErrorResponse = QStringLiteral("-113,\"Undefined header\"");
    SweepPage page(std::move(service));

    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopPscSweepButton"));
    auto *statusLabel = page.findChild<QLabel *>(QStringLiteral("PscSweepStatusLabel"));
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(statusLabel != nullptr);

    QTest::mouseClick(startButton, Qt::LeftButton);

    QVERIFY(QTest::qWaitFor([&]() {
        return startButton->isEnabled() && !stopButton->isEnabled();
    }, 1000));
    QVERIFY(startButton->isEnabled());
    QVERIFY(!stopButton->isEnabled());
    QVERIFY(statusLabel->text().contains(QStringLiteral("FREQ:PSC:STAR")));
    QVERIFY(statusLabel->text().contains(QStringLiteral("402 CMD ERR")));
    QVERIFY(statusLabel->text().contains(QStringLiteral("SYSTem:ERRor?")));
    QVERIFY(statusLabel->text().contains(QStringLiteral("-113,\"Undefined header\"")));
}

void UiTests::sweepPageShowsPscStartFailure()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    service->pscSweepStartSucceeds = false;
    auto *servicePtr = service.get();
    SweepPage page(std::move(service));

    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopPscSweepButton"));
    auto *statusLabel = page.findChild<QLabel *>(QStringLiteral("PscSweepStatusLabel"));
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(statusLabel != nullptr);

    QTest::mouseClick(startButton, Qt::LeftButton);

    QVERIFY(QTest::qWaitFor([&]() {
        return servicePtr->startPscSweepCount == 1 && startButton->isEnabled();
    }, 1000));
    QCOMPARE(servicePtr->startPscSweepCount, 1);
    QVERIFY(startButton->isEnabled());
    QVERIFY(!stopButton->isEnabled());
    QVERIFY(statusLabel->text().contains(QStringLiteral("viOpen failed")));
}

void UiTests::sweepPageUpdatesWaterfallAndCandidatesFromPscFrame()
{
    SweepPage page;

    auto *waterfall = page.findChild<WaterfallWidget *>(QStringLiteral("PscWaterfallWidget"));
    auto *candidateTable = page.findChild<QTableWidget *>(QStringLiteral("PscCandidateTable"));
    QVERIFY(waterfall != nullptr);
    QVERIFY(candidateTable != nullptr);

    CeyearPscFrame frame;
    frame.bins = {
        {900000000.0, -72.0},
        {901000000.0, -35.0},
    };

    page.appendPscFrameForTest(frame);
    page.appendPscFrameForTest(frame);

    QCOMPARE(waterfall->frameCount(), 2);
    QCOMPARE(candidateTable->rowCount(), 1);
    QCOMPARE(candidateTable->item(0, 0)->text(), QStringLiteral("901000000"));
}

void UiTests::sweepPageClearsWaterfallWhenStartingNewSweep()
{
    auto service = std::make_unique<FakeCeyearVisaService>();
    SweepPage page(std::move(service));

    auto *waterfall = page.findChild<WaterfallWidget *>(QStringLiteral("PscWaterfallWidget"));
    auto *candidateTable = page.findChild<QTableWidget *>(QStringLiteral("PscCandidateTable"));
    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartPscSweepButton"));
    QVERIFY(waterfall != nullptr);
    QVERIFY(candidateTable != nullptr);
    QVERIFY(startButton != nullptr);

    CeyearPscFrame frame;
    frame.bins = {
        {900000000.0, -72.0},
        {901000000.0, -35.0},
    };
    page.appendPscFrameForTest(frame);
    page.appendPscFrameForTest(frame);
    QCOMPARE(waterfall->frameCount(), 2);
    QCOMPARE(candidateTable->rowCount(), 1);

    QTest::mouseClick(startButton, Qt::LeftButton);

    QCOMPARE(waterfall->frameCount(), 0);
    QCOMPARE(candidateTable->rowCount(), 0);
}

void UiTests::sweepPageSavesPscFramesToCsv()
{
    SweepPage page;

    auto *saveButton = page.findChild<QPushButton *>(QStringLiteral("SavePscSweepDataButton"));
    auto *statusLabel = page.findChild<QLabel *>(QStringLiteral("PscSweepStatusLabel"));
    QVERIFY(saveButton != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(!saveButton->isEnabled());

    page.appendPscFrameForTest({{{900000000.0, -72.0}, {901000000.0, -35.0}}});
    QVERIFY(saveButton->isEnabled());

    QTest::mouseClick(saveButton, Qt::LeftButton);

    QVERIFY(statusLabel->text().contains(QStringLiteral("扫频数据已保存")));
    const int separator = statusLabel->text().indexOf(QStringLiteral("："));
    QVERIFY(separator >= 0);
    const QString filePath = QDir::fromNativeSeparators(statusLabel->text().mid(separator + 1).trimmed());
    QFile file(filePath);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(filePath));
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    QVERIFY(text.contains(QStringLiteral("frame_index,bin_index,frequency_hz,level_dbuv")));
    QVERIFY(text.contains(QStringLiteral("0,1,901000000,-35.0")));
    QVERIFY(QFile::remove(filePath));
}

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

void UiTests::stepNavigatorExposesBlockedReason()
{
    StepNavigator navigator;
    navigator.setSteps({QStringLiteral("输入"), QStringLiteral("训练")});
    navigator.setStepStatus(1,
                            WorkflowStepState::Blocked,
                            QStringLiteral("等待数据集"),
                            QStringLiteral("请先生成训练集版本"));

    QCOMPARE(navigator.stepState(1), WorkflowStepState::Blocked);
    QCOMPARE(navigator.stepSummary(1), QStringLiteral("等待数据集"));
    QCOMPARE(navigator.stepBlockingReason(1), QStringLiteral("请先生成训练集版本"));
    const auto buttons = navigator.findChildren<QPushButton *>();
    QCOMPARE(buttons.at(1)->toolTip(), QStringLiteral("请先生成训练集版本"));
}

void UiTests::mainWindowContainsWorkflowStatusLabels()
{
    MainWindow window(std::make_unique<FakeCeyearVisaService>(),
                      std::make_unique<FakeUsrpDeviceService>(),
                      std::make_unique<FakeCeyearVisaService>());

    QVERIFY(window.findChild<QStackedWidget *>(QStringLiteral("WorkflowStack")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("CurrentStepLabel")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("ClockLabel")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("DataPathLabel")) != nullptr);
}

void UiTests::preprocessPageExposesRealProcessingControls()
{
    PreprocessPage page;
    auto *runButton = page.findChild<QPushButton *>(QStringLiteral("StartPreprocessButton"));
    auto *detectionModeCombo = page.findChild<QComboBox *>(QStringLiteral("PreprocessDetectionModeCombo"));
    auto *payloadEndianCombo = page.findChild<QComboBox *>(QStringLiteral("PreprocessPayloadEndianCombo"));
    auto *exportSpectrogramCheck = page.findChild<QCheckBox *>(QStringLiteral("PreprocessExportSpectrogramCheck"));
    auto *title = page.findChild<QLabel *>(QStringLiteral("PageTitle"));
    QVERIFY(runButton != nullptr);
    QVERIFY(detectionModeCombo != nullptr);
    QVERIFY(payloadEndianCombo != nullptr);
    QVERIFY(exportSpectrogramCheck != nullptr);
    QVERIFY(title != nullptr);
    QCOMPARE(runButton->text(), QStringLiteral("开始预处理"));
    QCOMPARE(detectionModeCombo->currentData().toString(), QStringLiteral("cv_band"));
    QCOMPARE(payloadEndianCombo->currentData().toString(), QStringLiteral("little"));
    QVERIFY(!exportSpectrogramCheck->isChecked());
    QVERIFY(!title->text().contains(QChar(0xfffd)));
    QCOMPARE(title->text(), QStringLiteral("信号预处理"));
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("CancelPreprocessButton")) != nullptr);
    QVERIFY(page.findChild<QProgressBar *>(QStringLiteral("PreprocessProgressBar")) != nullptr);
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessResultDetailPanel")) != nullptr);
    auto *backendLabel = page.findChild<QLabel *>(QStringLiteral("PreprocessComputeBackendLabel"));
    QVERIFY(backendLabel != nullptr);
    QVERIFY(backendLabel->text().contains(QStringLiteral("CUDA GPU")));
}

void UiTests::preprocessPageUsesThreeColumnWorkbenchLayout()
{
    PreprocessPage page;

    QVERIFY(page.findChild<QFrame *>(QStringLiteral("PreprocessInputColumn")) != nullptr);
    QVERIFY(page.findChild<QFrame *>(QStringLiteral("PreprocessParametersColumn")) != nullptr);
    QVERIFY(page.findChild<QFrame *>(QStringLiteral("PreprocessRunColumn")) != nullptr);
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
    QCOMPARE(advancedToggle->text(), QStringLiteral("收起专家设置"));
}

void UiTests::preprocessPageUsesFlatPanelsForSummaryAndParameters()
{
    PreprocessPage page;

    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessSummaryPanel")) != nullptr);
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessPathParametersPanel")) != nullptr);
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("PreprocessAdvancedParametersPanel")) != nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessSummaryPanel")) == nullptr);
    QVERIFY(page.findChild<QGroupBox *>(QStringLiteral("PreprocessPathParametersPanel")) == nullptr);
}

void UiTests::preprocessPageKeepsParameterInputsReadable()
{
    PreprocessPage page;
    page.resize(1600, 620);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    const QStringList inputNames = {
        QStringLiteral("PreprocessOutputDirEdit"),
        QStringLiteral("PreprocessWindowSamplesEdit"),
        QStringLiteral("PreprocessChunkSamplesEdit"),
        QStringLiteral("PreprocessFrequencySpanEdit"),
        QStringLiteral("PreprocessTargetBandwidthEdit"),
        QStringLiteral("PreprocessEnergyOffsetEdit"),
    };

    for (const QString &name : inputNames) {
        auto *edit = page.findChild<QLineEdit *>(name);
        QVERIFY2(edit != nullptr, qPrintable(name));
        QVERIFY2(edit->minimumHeight() >= 36, qPrintable(QStringLiteral("%1 minimum height is %2").arg(name).arg(edit->minimumHeight())));
        QVERIFY2(edit->height() >= 38, qPrintable(QStringLiteral("%1 height is %2").arg(name).arg(edit->height())));
    }

    auto *capPathEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessCapPathEdit"));
    auto *outputDirEdit = page.findChild<QLineEdit *>(QStringLiteral("PreprocessOutputDirEdit"));
    QVERIFY(capPathEdit != nullptr);
    QVERIFY(outputDirEdit != nullptr);
    QVERIFY(capPathEdit->minimumWidth() >= 520);
    QVERIFY(outputDirEdit->minimumWidth() >= 520);

    auto *pathPanel = page.findChild<QWidget *>(QStringLiteral("PreprocessPathParametersPanel"));
    auto *resultsPanel = page.findChild<QGroupBox *>(QStringLiteral("PreprocessResultsPanel"));
    QVERIFY(pathPanel != nullptr);
    QVERIFY(resultsPanel != nullptr);
    QVERIFY(resultsPanel->maximumHeight() >= 500);
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

void UiTests::datasetPageExposesCategorySelectionWorkbench()
{
    DatasetPage page;
    auto *categoryTable = page.findChild<QTableWidget *>(QStringLiteral("DatasetCategoryTable"));
    auto *includePartial = page.findChild<QCheckBox *>(QStringLiteral("DatasetIncludePartialCheck"));
    auto *generateButton = page.findChild<QPushButton *>(QStringLiteral("GenerateDatasetVersionButton"));
    auto *versionTable = page.findChild<QTableWidget *>(QStringLiteral("DatasetVersionTable"));
    auto *selectedMetric = page.findChild<MetricCard *>(QStringLiteral("DatasetSelectedSamplesMetric"));

    QVERIFY(categoryTable != nullptr);
    QVERIFY(includePartial != nullptr);
    QVERIFY(generateButton != nullptr);
    QVERIFY(versionTable != nullptr);
    QVERIFY(selectedMetric != nullptr);
    QCOMPARE(categoryTable->columnCount(), 6);
    QCOMPARE(categoryTable->horizontalHeaderItem(1)->text(), QStringLiteral("类别"));
    QCOMPARE(generateButton->text(), QStringLiteral("生成训练集版本"));
    QVERIFY(!includePartial->isChecked());
}

void UiTests::datasetPageExposesSpectrogramReviewWidget()
{
    DatasetPage page;
    auto *reviewWidget = page.findChild<SpectrogramReviewWidget *>(
        QStringLiteral("DatasetSpectrogramReviewWidget"));
    auto *title = page.findChild<QLabel *>(QStringLiteral("DatasetSpectrogramTitle"));
    auto *pathLabel = page.findChild<QLabel *>(QStringLiteral("DatasetSpectrogramPathLabel"));

    QVERIFY(reviewWidget != nullptr);
    QVERIFY(title != nullptr);
    QVERIFY(pathLabel != nullptr);
    QCOMPARE(title->text(), QStringLiteral("候选框选时频图（专家复核）"));
    QVERIFY(reviewWidget->minimumSizeHint().width() >= 480);
    QVERIFY(!reviewWidget->hasImage());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("review.png"));
    QImage image(320, 180, QImage::Format_RGB32);
    image.fill(QColor(255, 178, 0));
    QVERIFY(image.save(imagePath));

    reviewWidget->resize(640, 360);
    reviewWidget->setImagePath(imagePath);
    QVERIFY(reviewWidget->hasImage());
    QCOMPARE(reviewWidget->imagePath(), imagePath);
    reviewWidget->clearImage(QStringLiteral("等待下一张图"));
    QVERIFY(!reviewWidget->hasImage());
    QCOMPARE(reviewWidget->imagePath(), QString());
}

void UiTests::datasetPageUsesStandardLabelSelector()
{
    DatasetPage page;
    auto *labelCombo = page.findChild<QComboBox *>(QStringLiteral("DatasetReviewLabelCombo"));
    QVERIFY(labelCombo != nullptr);
    QVERIFY(labelCombo->count() >= 17);
    QCOMPARE(labelCombo->itemData(1).toString(), QStringLiteral("air2_01"));
}

void UiTests::trainPageExposesBackendControls()
{
    TrainPage page;
    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartTrainingButton"));
    auto *cancelButton = page.findChild<QPushButton *>(QStringLiteral("CancelTrainingButton"));
    auto *progressBar = page.findChild<QProgressBar *>(QStringLiteral("TrainingProgressBar"));
    auto *epochs = page.findChild<QSpinBox *>(QStringLiteral("TrainingEpochsSpin"));
    auto *dataMode = page.findChild<QComboBox *>(QStringLiteral("TrainingDataModeCombo"));
    auto *cacheButton = page.findChild<QPushButton *>(QStringLiteral("PrecomputeTrainingCacheButton"));
    auto *sampleLength = page.findChild<QComboBox *>(QStringLiteral("TrainingSampleLengthCombo"));
    auto *modelMode = page.findChild<QComboBox *>(QStringLiteral("TrainingModelModeCombo"));
    auto *featureDescription = page.findChild<QLabel *>(QStringLiteral("TrainingFeatureDescriptionLabel"));

    QVERIFY(startButton != nullptr);
    QVERIFY(cancelButton != nullptr);
    QVERIFY(progressBar != nullptr);
    QVERIFY(epochs != nullptr);
    QVERIFY(dataMode != nullptr);
    QVERIFY(cacheButton != nullptr);
    QVERIFY(sampleLength != nullptr);
    QVERIFY(modelMode != nullptr);
    QVERIFY(featureDescription != nullptr);
    QVERIFY(!startButton->isEnabled());
    QVERIFY(!cancelButton->isEnabled());
    QCOMPARE(progressBar->value(), 0);
    QCOMPARE(epochs->value(), 6);
    QCOMPARE(dataMode->currentData().toString(), QStringLiteral("raw"));
    QCOMPARE(sampleLength->currentData().toInt(), 32768);
    QCOMPARE(modelMode->currentData().toString(), QStringLiteral("closed_set"));
    QVERIFY(featureDescription->text().contains(QStringLiteral("complex_signed_mag")));
    auto *advancedPanel = page.findChild<QWidget *>(QStringLiteral("TrainingAdvancedPanel"));
    QVERIFY(advancedPanel != nullptr);
    QVERIFY(!advancedPanel->isVisible());
}

void UiTests::recognitionPageExposesModelAndSampleWorkflow()
{
    RecognitionPage page;
    auto *modelCombo = page.findChild<QComboBox *>(QStringLiteral("RecognitionModelCombo"));
    auto *sampleCombo = page.findChild<QComboBox *>(QStringLiteral("RecognitionSampleCombo"));
    auto *runButton = page.findChild<QPushButton *>(QStringLiteral("RecognitionRunButton"));
    auto *cancelButton = page.findChild<QPushButton *>(QStringLiteral("RecognitionCancelButton"));
    QVERIFY(modelCombo != nullptr);
    QVERIFY(sampleCombo != nullptr);
    QVERIFY(runButton != nullptr);
    QVERIFY(cancelButton != nullptr);
    QVERIFY(!cancelButton->isEnabled());
    QVERIFY(!runButton->isEnabled());
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
    auto *gainCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpGainCombo"));
    auto *durationCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpDurationCombo"));
    auto *cpuFormatCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpCpuFormatCombo"));
    auto *wireFormatCombo = page.findChild<QComboBox *>(QStringLiteral("UsrpWireFormatCombo"));
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
    QVERIFY(page.findChild<QComboBox *>(QStringLiteral("UsrpBandwidthCombo")) == nullptr);
    QVERIFY(gainCombo != nullptr);
    QVERIFY(durationCombo != nullptr);
    QVERIFY(cpuFormatCombo != nullptr);
    QVERIFY(wireFormatCombo != nullptr);
    QVERIFY(deviceConfigStack != nullptr);
    QVERIFY(logOutput != nullptr);

    deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    QVERIFY(discoverButton->isEnabled());
    QVERIFY(captureButton->isEnabled());
    QVERIFY(captureButton->text().contains(QStringLiteral("3943B IQ")));
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
    QCOMPARE(servicePtr->lastConfig.bandwidthHz, 8000000.0);
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

void UiTests::capturePageUsesDeviceSpecificParameterPanels()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *parameterStack = page.findChild<QStackedWidget *>(QStringLiteral("CaptureParameterStack"));
    auto *usrpPanel = page.findChild<QWidget *>(QStringLiteral("UsrpParameterPanel"));
    auto *ceyearPanel = page.findChild<QWidget *>(QStringLiteral("CeyearControlPanel"));
    auto *usrpBasicPanel = page.findChild<QGroupBox *>(QStringLiteral("UsrpBasicParametersPanel"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(parameterStack != nullptr);
    QVERIFY(usrpPanel != nullptr);
    QVERIFY(ceyearPanel != nullptr);
    QVERIFY(usrpBasicPanel != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("ApplyCeyearControlButton")) == nullptr);

    QCOMPARE(parameterStack->currentWidget(), usrpPanel);
    QVERIFY(usrpBasicPanel->isVisibleTo(parameterStack));

    deviceTypeCombo->setCurrentIndex(1);
    QCOMPARE(parameterStack->currentWidget(), ceyearPanel);
    QVERIFY(!usrpBasicPanel->isVisibleTo(parameterStack));
    QVERIFY(captureButton->isEnabled());
    QCOMPARE(captureButton->text(), QStringLiteral("开始 3943B IQ 采集"));
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
    QCOMPARE(resultsTabs->count(), 3);
    QCOMPARE(resultsTabs->tabText(0), QStringLiteral("设备列表"));
    QCOMPARE(resultsTabs->tabText(1), QStringLiteral("采集记录"));
    QCOMPARE(resultsTabs->tabText(2), QStringLiteral("3943B 状态"));

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

void UiTests::capturePageUsesColumnScrollContainersAtMinimumSize()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());
    page.resize(1100, 720);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *deviceScroll = page.findChild<QScrollArea *>(QStringLiteral("CaptureDeviceColumnScroll"));
    auto *parametersScroll = page.findChild<QScrollArea *>(QStringLiteral("CaptureParametersColumnScroll"));
    auto *runScroll = page.findChild<QScrollArea *>(QStringLiteral("CaptureRunColumnScroll"));
    QVERIFY(deviceScroll != nullptr);
    QVERIFY(parametersScroll != nullptr);
    QVERIFY(runScroll != nullptr);
    QCOMPARE(deviceScroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(parametersScroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(runScroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(deviceScroll->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QCOMPARE(parametersScroll->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QCOMPARE(runScroll->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(deviceScroll->widgetResizable());
    QVERIFY(parametersScroll->widgetResizable());
    QVERIFY(runScroll->widgetResizable());

    auto *sourceButton = page.findChild<QPushButton *>(QStringLiteral("Device3943BSourceButton"));
    auto *deviceConfigStack = page.findChild<QStackedWidget *>(QStringLiteral("DeviceConfigStack"));
    QVERIFY(sourceButton != nullptr);
    QVERIFY(deviceConfigStack != nullptr);
    QTest::mouseClick(sourceButton, Qt::LeftButton);
    QCOMPARE(deviceConfigStack->currentIndex(), 1);

    auto *visaEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearVisaResourceEdit"));
    auto *frequencyEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearCenterFrequencyEdit"));
    auto *spanEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearFrequencySpanEdit"));
    QVERIFY(visaEdit != nullptr);
    QVERIFY(frequencyEdit != nullptr);
    QVERIFY(spanEdit != nullptr);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("ApplyCeyearControlButton")) == nullptr);
    QVERIFY(visaEdit->isVisible());
    QVERIFY(frequencyEdit->isVisible());
    QVERIFY(spanEdit->isVisible());
    QVERIFY(visaEdit->height() >= 30);
    QVERIFY(frequencyEdit->height() >= 30);
    QVERIFY(spanEdit->height() >= 30);
}

void UiTests::capturePageKeepsResultsTabsReadableAtMinimumSize()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>());
    page.resize(1100, 720);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));

    auto *resultsGroup = page.findChild<QGroupBox *>(QStringLiteral("CaptureResultsPanel"));
    auto *resultsTabs = page.findChild<QTabWidget *>(QStringLiteral("CaptureResultsTabs"));
    auto *statusTable = page.findChild<QTableWidget *>(QStringLiteral("CeyearStatusTable"));
    QVERIFY(resultsGroup != nullptr);
    QVERIFY(resultsTabs != nullptr);
    QVERIFY(statusTable != nullptr);

    resultsTabs->setCurrentWidget(statusTable);
    QVERIFY(resultsGroup->minimumHeight() >= 280);
    QVERIFY(resultsTabs->minimumHeight() >= 220);
    QVERIFY(statusTable->minimumHeight() >= 180);
    QVERIFY(resultsGroup->height() >= 260);
    QVERIFY(resultsTabs->height() >= 210);
    QVERIFY(statusTable->height() >= 170);
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

void UiTests::capturePageQueries3943BStatusInsideDeviceOption()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *discoverButton = page.findChild<QPushButton *>(QStringLiteral("DiscoverUsrpButton"));
    auto *sourceButton = page.findChild<QPushButton *>(QStringLiteral("Device3943BSourceButton"));
    auto *resourceEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearVisaResourceEdit"));
    auto *statusTable = page.findChild<QTableWidget *>(QStringLiteral("CeyearStatusTable"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(sourceButton != nullptr);
    QVERIFY(discoverButton != nullptr);
    QVERIFY(resourceEdit != nullptr);
    QVERIFY(statusTable != nullptr);
    QVERIFY(logOutput != nullptr);

    deviceTypeCombo->setCurrentText(QStringLiteral("3943B（预留）"));
    QCOMPARE(resourceEdit->text(), QStringLiteral("TCPIP::192.168.50.54::5025::SOCKET"));
    QVERIFY(discoverButton->isEnabled());

    QTest::mouseClick(discoverButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return statusTable->rowCount() == 3;
    }, 2000));
    QCOMPARE(ceyearServicePtr->lastResource, QStringLiteral("TCPIP::192.168.50.54::5025::SOCKET"));
    QCOMPARE(statusTable->item(0, 0)->text(), QStringLiteral("Identity"));
    QCOMPARE(statusTable->item(0, 2)->text(), QStringLiteral("Ceyear Technologies,3943B,,V2.1.0"));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B status ok")));
}

void UiTests::capturePageDoesNotExposeSeparate3943BControlWrite()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *resourceEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearVisaResourceEdit"));
    auto *centerFreqEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearCenterFrequencyEdit"));
    auto *spanEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearFrequencySpanEdit"));
    auto *demodCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearDemodulationCombo"));
    auto *detectorCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearDetectorCombo"));
    auto *statusTable = page.findChild<QTableWidget *>(QStringLiteral("CeyearStatusTable"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(resourceEdit != nullptr);
    QVERIFY(centerFreqEdit != nullptr);
    QVERIFY(spanEdit != nullptr);
    QVERIFY(demodCombo != nullptr);
    QVERIFY(detectorCombo != nullptr);
    QVERIFY(statusTable != nullptr);
    QVERIFY(page.findChild<QPushButton *>(QStringLiteral("ApplyCeyearControlButton")) == nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    centerFreqEdit->setText(QStringLiteral("109000000"));
    spanEdit->setText(QStringLiteral("20000000"));
    QCOMPARE(demodCombo->currentText(), QStringLiteral("IQ"));
    detectorCombo->setCurrentText(QStringLiteral("PEAK"));
    QCoreApplication::processEvents();

    QCOMPARE(ceyearServicePtr->applyControlCount, 0);
    QCOMPARE(statusTable->rowCount(), 0);
}

void UiTests::capturePageDefaults3943BToVerifiedManualIqPath()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::make_unique<FakeCeyearVisaService>());

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *resourceEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearVisaResourceEdit"));
    auto *centerFreqEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearCenterFrequencyEdit"));
    auto *spanEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearFrequencySpanEdit"));
    auto *demodCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearDemodulationCombo"));
    auto *measureModeCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearMeasureModeCombo"));
    auto *iqPointsEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearIqPointCountEdit"));
    auto *recordSourceCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearRecordSourceCombo"));
    auto *reportPathCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearReportPathCombo"));
    auto *pcProtocolCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearPcReceiveProtocolCombo"));
    auto *pcIpEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearPcReceiveIpEdit"));
    auto *pcPortEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearPcReceivePortEdit"));
    auto *continuousCheck = page.findChild<QCheckBox *>(QStringLiteral("CeyearContinuousCaptureCheck"));
    const auto stopButtons = page.findChildren<QPushButton *>(QStringLiteral("StopCeyearCaptureButton"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(resourceEdit != nullptr);
    QVERIFY(centerFreqEdit != nullptr);
    QVERIFY(spanEdit != nullptr);
    QVERIFY(demodCombo != nullptr);
    QVERIFY(measureModeCombo != nullptr);
    QVERIFY(iqPointsEdit != nullptr);
    QVERIFY(recordSourceCombo != nullptr);
    QVERIFY(reportPathCombo != nullptr);
    QVERIFY(pcProtocolCombo != nullptr);
    QVERIFY(pcIpEdit != nullptr);
    QVERIFY(pcPortEdit != nullptr);
    QVERIFY(continuousCheck != nullptr);
    QCOMPARE(stopButtons.size(), 1);

    deviceTypeCombo->setCurrentIndex(1);

    QCOMPARE(resourceEdit->text(), QStringLiteral("TCPIP::192.168.50.54::5025::SOCKET"));
    QCOMPARE(centerFreqEdit->text(), QStringLiteral("2400000000"));
    QCOMPARE(spanEdit->text(), QStringLiteral("20000000"));
    QCOMPARE(demodCombo->currentText(), QStringLiteral("IQ"));
    QVERIFY(demodCombo->findText(QStringLiteral("IQ")) >= 0);
    QCOMPARE(measureModeCombo->currentText(), QStringLiteral("FIX"));
    QCOMPARE(iqPointsEdit->text(), QStringLiteral("4096"));
    QCOMPARE(recordSourceCombo->currentText(), QStringLiteral("IQAudio"));
    QVERIFY(recordSourceCombo->findText(QStringLiteral("IQAudio")) >= 0);
    QVERIFY(recordSourceCombo->findText(QStringLiteral("IQ")) >= 0);
    QCOMPARE(reportPathCombo->currentText(), QStringLiteral("UDP"));
    QCOMPARE(pcProtocolCombo->currentText(), QStringLiteral("UDP"));
    QCOMPARE(pcIpEdit->text(), QStringLiteral("192.168.50.20"));
    QCOMPARE(pcPortEdit->text(), QStringLiteral("5030"));
    QVERIFY(!continuousCheck->isChecked());
    continuousCheck->setChecked(true);
    QCOMPARE(iqPointsEdit->text(), QStringLiteral("4096"));

    bool hasReceiveWindowLabel = false;
    const auto labels = page.findChildren<QLabel *>();
    for (const auto *label : labels) {
        if (label->text() == QStringLiteral("接收窗口 s")) {
            hasReceiveWindowLabel = true;
            break;
        }
    }
    QVERIFY(!hasReceiveWindowLabel);
}

void UiTests::capturePageExposes3943BPointCountOnly()
{
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::make_unique<FakeCeyearVisaService>());

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *presetCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearCapturePresetCombo"));
    auto *advancedToggle = page.findChild<QPushButton *>(QStringLiteral("CeyearAdvancedCaptureToggle"));
    auto *advancedPanel = page.findChild<QWidget *>(QStringLiteral("CeyearAdvancedCapturePanel"));
    auto *iqPointsEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearIqPointCountEdit"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(iqPointsEdit != nullptr);
    QVERIFY(presetCombo == nullptr);
    QVERIFY(advancedToggle == nullptr);
    QVERIFY(advancedPanel == nullptr);
    QVERIFY(page.findChild<QLineEdit *>(QStringLiteral("CeyearMeasureTimeEdit")) == nullptr);
    QVERIFY(page.findChild<QLineEdit *>(QStringLiteral("CeyearCaptureDurationEdit")) == nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    QCOMPARE(iqPointsEdit->text(), QStringLiteral("4096"));
}

void UiTests::capturePageContinuous3943BCaptureSchedulesNextFile()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopCeyearCaptureButton"));
    auto *continuousCheck = page.findChild<QCheckBox *>(QStringLiteral("CeyearContinuousCaptureCheck"));
    auto *iqPointsEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearIqPointCountEdit"));
    auto *captureTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpCaptureTable"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(continuousCheck != nullptr);
    QVERIFY(iqPointsEdit != nullptr);
    QVERIFY(captureTable != nullptr);
    QVERIFY(logOutput != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    iqPointsEdit->setText(QStringLiteral("2048"));
    continuousCheck->setChecked(true);

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount >= 1 && captureTable->rowCount() >= 1;
    }, 2000));

    QVERIFY(!captureButton->isEnabled());
    QVERIFY(stopButton->isEnabled());
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B 连续采集已启动")));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B 连续采集第 1 轮开始")));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("立即启动第 2 轮")));
    QCOMPARE(ceyearServicePtr->lastCapture.measurementTimeSeconds, 0.0);
    QCOMPARE(ceyearServicePtr->lastCapture.pcReceiveTimeoutMs, 0);
    QCOMPARE(ceyearServicePtr->lastCapture.iqPointCount, 2048);
    QVERIFY(captureTable->item(0, 0)->text().endsWith(QStringLiteral(".iqraw")));

    QTest::mouseClick(stopButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B 连续采集已停止。")));
}

void UiTests::capturePageContinuousCaptureStartsNextRoundImmediately()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopCeyearCaptureButton"));
    auto *continuousCheck = page.findChild<QCheckBox *>(QStringLiteral("CeyearContinuousCaptureCheck"));
    auto *iqPointsEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearIqPointCountEdit"));
    auto *captureTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpCaptureTable"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(continuousCheck != nullptr);
    QVERIFY(iqPointsEdit != nullptr);
    QVERIFY(captureTable != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    iqPointsEdit->setText(QStringLiteral("8192"));
    continuousCheck->setChecked(true);

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount >= 2 && captureTable->rowCount() >= 2;
    }, 2000));

    QCOMPARE(ceyearServicePtr->lastCapture.iqPointCount, 8192);
    QCOMPARE(ceyearServicePtr->lastCapture.measurementTimeSeconds, 0.0);
    QCOMPARE(ceyearServicePtr->lastCapture.pcReceiveTimeoutMs, 0);
    QCOMPARE(stopButton->text(), QStringLiteral("停止连续采集"));

    QTest::mouseClick(stopButton, Qt::LeftButton);
    QCoreApplication::processEvents();
}

void UiTests::capturePageConfigures3943BPathFromMainButton()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *pcIpEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearPcReceiveIpEdit"));
    auto *pcPortEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearPcReceivePortEdit"));
    auto *measureModeCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearMeasureModeCombo"));
    auto *iqPointsEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearIqPointCountEdit"));
    auto *recordSourceCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearRecordSourceCombo"));
    auto *pcProtocolCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearPcReceiveProtocolCombo"));
    auto *pcOutputEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearPcOutputFileEdit"));
    auto *targetModelCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearTargetModelCombo"));
    auto *modelIndexCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearModelIndexCombo"));
    auto *distanceCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearDistanceCombo"));
    auto *heightCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearHeightCombo"));
    auto *bandCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearBandCombo"));
    auto *segmentEdit = page.findChild<QLineEdit *>(QStringLiteral("CeyearSegmentEdit"));
    auto *recordCheck = page.findChild<QCheckBox *>(QStringLiteral("CeyearRecordEnableCheck"));
    auto *statusTable = page.findChild<QTableWidget *>(QStringLiteral("CeyearStatusTable"));
    auto *captureTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpCaptureTable"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(pcIpEdit != nullptr);
    QVERIFY(pcPortEdit != nullptr);
    QVERIFY(measureModeCombo != nullptr);
    QVERIFY(iqPointsEdit != nullptr);
    QVERIFY(recordSourceCombo != nullptr);
    QVERIFY(pcProtocolCombo != nullptr);
    QVERIFY(pcOutputEdit != nullptr);
    QVERIFY(targetModelCombo != nullptr);
    QVERIFY(modelIndexCombo != nullptr);
    QVERIFY(distanceCombo != nullptr);
    QVERIFY(heightCombo != nullptr);
    QVERIFY(bandCombo != nullptr);
    QVERIFY(segmentEdit != nullptr);
    QVERIFY(recordCheck != nullptr);
    QVERIFY(statusTable != nullptr);
    QVERIFY(captureTable != nullptr);
    QVERIFY(logOutput != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    QCOMPARE(measureModeCombo->currentText(), QStringLiteral("FIX"));
    QCOMPARE(iqPointsEdit->text(), QStringLiteral("4096"));
    iqPointsEdit->setText(QStringLiteral("2048"));
    pcPortEdit->setText(QStringLiteral("5030"));
    targetModelCombo->setCurrentText(QStringLiteral("Mini5Pro"));
    modelIndexCombo->setCurrentText(QStringLiteral("02"));
    distanceCombo->setCurrentText(QStringLiteral("D03"));
    heightCombo->setCurrentText(QStringLiteral("H01"));
    bandCombo->setCurrentText(QStringLiteral("F58"));
    segmentEdit->setText(QStringLiteral("7"));
    recordCheck->setChecked(true);

    QVERIFY(captureButton->isEnabled());
    QCOMPARE(captureButton->text(), QStringLiteral("开始 3943B IQ 采集"));

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount == 1 && captureTable->rowCount() == 1;
    }, 2000));

    QCOMPARE(ceyearServicePtr->configureCapturePathCount, 0);
    QCOMPARE(ceyearServicePtr->startCaptureCount, 0);
    QCOMPARE(ceyearServicePtr->startManualIqReportCount, 0);
    QCOMPARE(ceyearServicePtr->triggerIqReportCount, 1);
    QCOMPARE(ceyearServicePtr->lastCapture.iqPointCount, 2048);
    QCOMPARE(ceyearServicePtr->lastCapture.pcReceivePort, 5030);
    QCOMPARE(ceyearServicePtr->lastCapture.measureMode, QStringLiteral("FIX"));
    QCOMPARE(ceyearServicePtr->lastCapture.measurementTimeSeconds, 0.0);
    QCOMPARE(ceyearServicePtr->lastCapture.pcReceiveTimeoutMs, 0);
    QCOMPARE(ceyearServicePtr->lastCapture.demodulation, QStringLiteral("IQ"));
    QCOMPARE(ceyearServicePtr->lastCapture.detector, QStringLiteral("PEAK"));
    QCOMPARE(ceyearServicePtr->lastCapture.recordSource, QStringLiteral("IQAudio"));
    QCOMPARE(ceyearServicePtr->lastCapture.reportPath, QStringLiteral("UDP"));
    QCOMPARE(ceyearServicePtr->lastCapture.pcReceiveProtocol, QStringLiteral("UDP"));
    const QString captureFileName = QFileInfo(ceyearServicePtr->lastCapture.outputFilePath).fileName();
    QVERIFY(QRegularExpression(QStringLiteral("^B_Mini5Pro_02_D03_H01_F58_S007_\\d{8}_\\d{6}\\.iqraw$"))
                .match(captureFileName)
                .hasMatch());
    QCOMPARE(pcOutputEdit->text(), ceyearServicePtr->lastCapture.outputFilePath);
    QVERIFY(statusTable->rowCount() > 0);
    QVERIFY(captureTable->item(0, 0)->text().endsWith(QStringLiteral(".iqraw")));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B IQ raw saved to")));
    QVERIFY(!logOutput->toPlainText().contains(QStringLiteral("3943B CAP saved to")));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("16470")));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("IQ 点数 2048")));
    QVERIFY(!logOutput->toPlainText().contains(QStringLiteral("采集时长")));
    QVERIFY(!logOutput->toPlainText().contains(QStringLiteral("接收窗口")));
    QVERIFY(!logOutput->toPlainText().contains(QStringLiteral("Time 10.000 s")));
}

void UiTests::capturePageIgnoresSecond3943BStartWhileBusy()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>(250);
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *captureTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpCaptureTable"));
    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(captureTable != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    QVERIFY(captureButton->isEnabled());

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY(!captureButton->isEnabled());

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCOMPARE(ceyearServicePtr->triggerIqReportCount, 0);

    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount == 1 && captureButton->isEnabled();
    }, 2000));
    QCOMPARE(captureTable->rowCount(), 1);
}

void UiTests::capturePageCanRun3943BCaptureTwice()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *captureTable = page.findChild<QTableWidget *>(QStringLiteral("UsrpCaptureTable"));
    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(captureTable != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    QVERIFY(captureButton->isEnabled());
    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount == 1 && captureButton->isEnabled();
    }, 2000));

    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount == 2 && captureButton->isEnabled();
    }, 2000));

    QCOMPARE(captureTable->rowCount(), 2);
}

void UiTests::capturePagePasses3943BReportPathSeparatelyFromPcProtocol()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *captureButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *reportPathCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearReportPathCombo"));
    auto *pcProtocolCombo = page.findChild<QComboBox *>(QStringLiteral("CeyearPcReceiveProtocolCombo"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(captureButton != nullptr);
    QVERIFY(reportPathCombo != nullptr);
    QVERIFY(pcProtocolCombo != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    reportPathCombo->setCurrentText(QStringLiteral("TCP"));
    pcProtocolCombo->setCurrentText(QStringLiteral("UDP"));

    QVERIFY(captureButton->isEnabled());
    QTest::mouseClick(captureButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount == 1;
    }, 2000));

    QCOMPARE(ceyearServicePtr->configureCapturePathCount, 0);
    QCOMPARE(ceyearServicePtr->startCaptureCount, 0);
    QCOMPARE(ceyearServicePtr->triggerIqReportCount, 1);
    QCOMPARE(ceyearServicePtr->lastCapture.reportPath, QStringLiteral("TCP"));
    QCOMPARE(ceyearServicePtr->lastCapture.pcReceiveProtocol, QStringLiteral("UDP"));
    QCOMPARE(reportPathCombo->currentText(), QStringLiteral("TCP"));
}

void UiTests::capturePageStops3943BRecording()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>();
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopCeyearCaptureButton"));
    auto *statusTable = page.findChild<QTableWidget *>(QStringLiteral("CeyearStatusTable"));
    auto *logOutput = page.findChild<QPlainTextEdit *>(QStringLiteral("UsrpCaptureLog"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(stopButton != nullptr);
    QVERIFY(statusTable != nullptr);
    QVERIFY(logOutput != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    QVERIFY(stopButton->isEnabled());
    QCOMPARE(stopButton->text(), QStringLiteral("停止 3943B 采集"));

    page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"))->setEnabled(true);

    QTest::mouseClick(stopButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->stopCaptureCount == 1 && statusTable->rowCount() == 2;
    }, 2000));

    QCOMPARE(statusTable->item(0, 2)->text(), QStringLiteral("0"));
    QVERIFY(logOutput->toPlainText().contains(QStringLiteral("3943B capture stopped")));
}

void UiTests::capturePageKeeps3943BStopEnabledDuringBusyState()
{
    auto ceyearService = std::make_unique<FakeCeyearVisaService>(250);
    auto *ceyearServicePtr = ceyearService.get();
    CapturePage page(std::make_unique<FakeUsrpDeviceService>(), std::move(ceyearService));

    auto *deviceTypeCombo = page.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    auto *startButton = page.findChild<QPushButton *>(QStringLiteral("StartUsrpCaptureButton"));
    auto *stopButton = page.findChild<QPushButton *>(QStringLiteral("StopCeyearCaptureButton"));

    QVERIFY(deviceTypeCombo != nullptr);
    QVERIFY(startButton != nullptr);
    QVERIFY(stopButton != nullptr);

    deviceTypeCombo->setCurrentIndex(1);
    QCOMPARE(stopButton->isEnabled(), true);
    QCOMPARE(stopButton->text(), QStringLiteral("停止 3943B 采集"));

    QVERIFY(startButton->isEnabled());
    QTest::mouseClick(startButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY(!startButton->isEnabled());
    QVERIFY(stopButton->isEnabled());

    QTest::mouseClick(stopButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    QCOMPARE(ceyearServicePtr->requestStopCount, 1);

    QVERIFY(QTest::qWaitFor([&]() {
        return ceyearServicePtr->triggerIqReportCount == 1 && startButton->isEnabled();
    }, 2000));
    QCOMPARE(ceyearServicePtr->configureCapturePathCount, 0);
    QCOMPARE(ceyearServicePtr->startCaptureCount, 0);
    QCOMPARE(ceyearServicePtr->triggerIqReportCount, 1);
    QVERIFY(stopButton->isEnabled());
}

void UiTests::mainWindowStartsWithSweepLocationBeforeCapture()
{
    MainWindow window(std::make_unique<FakeCeyearVisaService>(),
                      std::make_unique<FakeUsrpDeviceService>(),
                      std::make_unique<FakeCeyearVisaService>());

    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("WorkflowStack"));
    auto *navigator = window.findChild<StepNavigator *>(QStringLiteral("StepNavigator"));
    QVERIFY(stack != nullptr);
    QVERIFY(navigator != nullptr);
    QVERIFY(qobject_cast<SweepPage *>(stack->widget(0)) != nullptr);
    QVERIFY(qobject_cast<CapturePage *>(stack->widget(1)) != nullptr);

    const auto buttons = navigator->findChildren<QPushButton *>();
    QVERIFY(buttons.size() >= 2);
    QVERIFY(buttons.at(0)->text().contains(QStringLiteral("扫频定位")));
    QVERIFY(buttons.at(1)->text().contains(QStringLiteral("采集")));
}

void UiTests::mainWindowPassesSweepFrequencyToCapturePage()
{
    MainWindow window(std::make_unique<FakeCeyearVisaService>(),
                      std::make_unique<FakeUsrpDeviceService>(),
                      std::make_unique<FakeCeyearVisaService>());

    auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("WorkflowStack"));
    auto *sweepPage = window.findChild<SweepPage *>();
    auto *centerFreqEdit = window.findChild<QLineEdit *>(QStringLiteral("CeyearCenterFrequencyEdit"));
    auto *deviceTypeCombo = window.findChild<QComboBox *>(QStringLiteral("CaptureDeviceTypeCombo"));
    QVERIFY(stack != nullptr);
    QVERIFY(sweepPage != nullptr);
    QVERIFY(centerFreqEdit != nullptr);
    QVERIFY(deviceTypeCombo != nullptr);

    emit sweepPage->frequencyConfirmed(901000000.0);

    QCOMPARE(stack->currentIndex(), 1);
    QCOMPARE(centerFreqEdit->text(), QStringLiteral("901000000"));
    QCOMPARE(deviceTypeCombo->currentText(), QStringLiteral("3943B（预留）"));
}

void UiTests::mainWindowShowsCaptureWorkbench()
{
    MainWindow window(std::make_unique<FakeCeyearVisaService>(),
                      std::make_unique<FakeUsrpDeviceService>(),
                      std::make_unique<FakeCeyearVisaService>());
    QVERIFY(window.findChild<QStackedWidget *>(QStringLiteral("WorkflowStack")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("CurrentStepLabel")) != nullptr);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("DataPathLabel")) != nullptr);
}

void UiTests::mainWindowStaysAliveAfterShowing()
{
    MainWindow window(std::make_unique<FakeCeyearVisaService>(),
                      std::make_unique<FakeUsrpDeviceService>(),
                      std::make_unique<FakeCeyearVisaService>());
    QVERIFY(window.findChild<QStackedWidget *>(QStringLiteral("WorkflowStack")) != nullptr);
    QCoreApplication::processEvents();
    QVERIFY(window.isEnabled());
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
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH")) {
        const QString pluginPath = QDir(QLibraryInfo::location(QLibraryInfo::PluginsPath)).filePath(QStringLiteral("platforms"));
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", QFile::encodeName(pluginPath));
    }
    QApplication app(argc, argv);
    UiTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ui_tests.moc"
