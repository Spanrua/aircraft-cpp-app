#include "ui/pages/SweepPage.h"

#include "app/ProjectPaths.h"
#include "services/CeyearPscReceiver.h"
#include "services/CeyearVisaQuery.h"
#include "services/CeyearVisaService.h"
#include "ui/widgets/WaterfallWidget.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>

namespace
{
QTableWidgetItem *centeredItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

QLineEdit *segmentValueEdit(const QString &text, QWidget *parent)
{
    auto *edit = new QLineEdit(text, parent);
    edit->setAlignment(Qt::AlignCenter);
    edit->setMinimumHeight(30);
    return edit;
}

bool pscSystemErrorIsClear(const QString &response)
{
    const QString normalized = response.trimmed().toLower();
    return normalized.isEmpty()
        || normalized == QStringLiteral("00")
        || normalized == QStringLiteral("0")
        || normalized.startsWith(QStringLiteral("0,"))
        || normalized.contains(QStringLiteral("no error"));
}

QString pscSweepStatusText(const CeyearCaptureResult &result, const QString &fallback)
{
    QString text = result.message.isEmpty() ? fallback : result.message;
    for (const CeyearStatusItem &item : result.items) {
        if (item.command.compare(QStringLiteral("SYSTem:ERRor?"), Qt::CaseInsensitive) != 0) {
            continue;
        }

        const QString detail = item.success ? item.response : item.error;
        if (!detail.trimmed().isEmpty()) {
            if (item.success && pscSystemErrorIsClear(detail)) {
                continue;
            }
            if (!item.label.trimmed().isEmpty() && item.label != QStringLiteral("System error")) {
                text += QStringLiteral("\n%1").arg(item.label.trimmed());
            }
            text += QStringLiteral("\nSYSTem:ERRor? -> %1").arg(detail.trimmed());
        }
    }
    return text;
}

QString defaultPscSweepCsvPath()
{
    const QString sweepDir = QDir(ProjectPaths::dataDirectory()).filePath(QStringLiteral("sweep"));
    QDir().mkpath(sweepDir);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return QDir(sweepDir).filePath(QStringLiteral("3943b_psc_sweep_%1.csv").arg(stamp));
}
} // namespace

SweepPage::SweepPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("扫频定位"),
                   QStringLiteral("使用 3943B PSC 多频段扫频生成时频图，先定位疑似无人机信号，再进入采集。"),
                   parent,
                   true)
{
    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setInterval(10000); // 10秒
    connect(m_autoSaveTimer, &QTimer::timeout, this, &SweepPage::autoSaveSweepData);

    buildUi();
}

SweepPage::SweepPage(std::unique_ptr<ICeyearVisaService> ceyearService, QWidget *parent)
    : WorkflowPage(QStringLiteral("Sweep"),
                   QStringLiteral("3943B PSC sweep location"),
                   parent,
                   true),
      m_ceyearService(std::move(ceyearService))
{
    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setInterval(10000); // 10秒
    connect(m_autoSaveTimer, &QTimer::timeout, this, &SweepPage::autoSaveSweepData);

    buildUi();
}

SweepPage::~SweepPage()
{
    if (m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }
    if (m_pscCommandThread) {
        m_pscCommandThread->quit();
        m_pscCommandThread->wait();
    }
    if (m_pscReceiver) {
        m_pscReceiver->disconnect(this);
        m_pscReceiver->stop();
        m_pscReceiver.reset();
    }
}

void SweepPage::appendPscFrameForTest(const CeyearPscFrame &frame)
{
    appendPscFrame(frame);
}

void SweepPage::setCandidateRowsForTest(const QVector<CeyearPscCandidate> &candidates)
{
    setCandidateRows(candidates);
}

void SweepPage::buildUi()
{
    auto *layout = new QHBoxLayout();
    layout->setSpacing(12);

    auto *segmentGroup = new QGroupBox(QStringLiteral("扫频频段"), this);
    auto *segmentLayout = new QVBoxLayout(segmentGroup);
    m_segmentTable = new QTableWidget(0, 3, segmentGroup);
    m_segmentTable->setObjectName(QStringLiteral("SweepSegmentTable"));
    m_segmentTable->setHorizontalHeaderLabels({
        QStringLiteral("起始 MHz"),
        QStringLiteral("结束 MHz"),
        QStringLiteral("步进 kHz"),
    });
    m_segmentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_segmentTable->verticalHeader()->setVisible(false);
    m_segmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    segmentLayout->addWidget(m_segmentTable);

    auto *thresholdLayout = new QHBoxLayout();
    auto *thresholdLabel = new QLabel(QStringLiteral("命中阈值 dBuV"), segmentGroup);
    m_hitThresholdEdit = new QLineEdit(segmentGroup);
    m_hitThresholdEdit->setObjectName(QStringLiteral("PscHitThresholdEdit"));
    m_hitThresholdEdit->setText(QStringLiteral("-60"));
    m_hitThresholdEdit->setAlignment(Qt::AlignRight);
    m_hitThresholdEdit->setMinimumHeight(36);
    thresholdLayout->addWidget(thresholdLabel);
    thresholdLayout->addWidget(m_hitThresholdEdit);
    segmentLayout->addLayout(thresholdLayout);

    auto *buttonLayout = new QHBoxLayout();
    m_startButton = new QPushButton(QStringLiteral("开始扫频"), segmentGroup);
    m_startButton->setObjectName(QStringLiteral("StartPscSweepButton"));
    m_startButton->setProperty("primary", true);
    m_stopButton = new QPushButton(QStringLiteral("停止"), segmentGroup);
    m_stopButton->setObjectName(QStringLiteral("StopPscSweepButton"));
    m_stopButton->setEnabled(false);
    m_saveButton = new QPushButton(QStringLiteral("保存扫频数据"), segmentGroup);
    m_saveButton->setObjectName(QStringLiteral("SavePscSweepDataButton"));
    m_saveButton->setEnabled(false);
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(m_stopButton);
    buttonLayout->addWidget(m_saveButton);
    segmentLayout->addLayout(buttonLayout);
    m_statusLabel = new QLabel(QStringLiteral("等待扫频"), segmentGroup);
    m_statusLabel->setObjectName(QStringLiteral("PscSweepStatusLabel"));
    m_statusLabel->setWordWrap(true);
    segmentLayout->addWidget(m_statusLabel);
    layout->addWidget(segmentGroup, 0);

    auto *waterfallGroup = new QGroupBox(QStringLiteral("PSC 时频图"), this);
    auto *waterfallLayout = new QVBoxLayout(waterfallGroup);
    m_waterfall = new WaterfallWidget(waterfallGroup);
    m_waterfall->setObjectName(QStringLiteral("PscWaterfallWidget"));
    waterfallLayout->addWidget(m_waterfall, 1);
    layout->addWidget(waterfallGroup, 1);

    auto *candidateGroup = new QGroupBox(QStringLiteral("候选信号"), this);
    auto *candidateLayout = new QVBoxLayout(candidateGroup);
    m_candidateTable = new QTableWidget(0, 3, candidateGroup);
    m_candidateTable->setObjectName(QStringLiteral("PscCandidateTable"));
    m_candidateTable->setHorizontalHeaderLabels({
        QStringLiteral("频率 Hz"),
        QStringLiteral("峰值 dBuV"),
        QStringLiteral("命中"),
    });
    m_candidateTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_candidateTable->verticalHeader()->setVisible(false);
    m_candidateTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_candidateTable->setSelectionMode(QAbstractItemView::SingleSelection);
    candidateLayout->addWidget(m_candidateTable, 1);

    m_confirmButton = new QPushButton(QStringLiteral("确认并采集"), candidateGroup);
    m_confirmButton->setObjectName(QStringLiteral("ConfirmPscCandidateButton"));
    m_confirmButton->setProperty("primary", true);
    candidateLayout->addWidget(m_confirmButton);
    layout->addWidget(candidateGroup, 0);

    contentLayout()->addLayout(layout, 1);
    seedDefaultSegments();

    connect(m_confirmButton, &QPushButton::clicked, this, &SweepPage::confirmSelectedCandidate);
    connect(m_startButton, &QPushButton::clicked, this, &SweepPage::startSweep);
    connect(m_stopButton, &QPushButton::clicked, this, &SweepPage::stopSweep);
    connect(m_saveButton, &QPushButton::clicked, this, &SweepPage::saveSweepData);
}

void SweepPage::seedDefaultSegments()
{
    const QVector<CeyearPscSweepSegment> segments = {
        {2385000000.0, 2415000000.0, 5000.0},
    };

    m_segmentTable->setRowCount(segments.size());
    for (int row = 0; row < segments.size(); ++row) {
        const CeyearPscSweepSegment &segment = segments.at(row);
        const QString startText = QString::number(segment.startHz / 1000000.0, 'f', 0);
        const QString stopText = QString::number(segment.stopHz / 1000000.0, 'f', 0);
        const QString stepText = QString::number(segment.stepHz / 1000.0, 'f', segment.stepHz == 12500.0 ? 1 : 0);
        m_segmentTable->setItem(row, 0, centeredItem(startText));
        m_segmentTable->setItem(row, 1, centeredItem(stopText));
        m_segmentTable->setItem(row, 2, centeredItem(stepText));
        m_segmentTable->setCellWidget(row, 0, segmentValueEdit(startText, m_segmentTable));
        m_segmentTable->setCellWidget(row, 1, segmentValueEdit(stopText, m_segmentTable));
        m_segmentTable->setCellWidget(row, 2, segmentValueEdit(stepText, m_segmentTable));
    }
}

QString SweepPage::segmentCellText(int row, int column) const
{
    if (!m_segmentTable) {
        return QString();
    }

    if (auto *edit = qobject_cast<QLineEdit *>(m_segmentTable->cellWidget(row, column))) {
        return edit->text();
    }

    const QTableWidgetItem *item = m_segmentTable->item(row, column);
    return item ? item->text() : QString();
}

CeyearPscSweepConfig SweepPage::sweepConfigFromTable() const
{
    CeyearPscSweepConfig config;
    if (!m_segmentTable) {
        return config;
    }

    if (m_hitThresholdEdit) {
        bool thresholdOk = false;
        const double threshold = m_hitThresholdEdit->text().toDouble(&thresholdOk);
        if (thresholdOk) {
            config.hitThresholdDbuv = threshold;
        }
    }

    for (int row = 0; row < m_segmentTable->rowCount(); ++row) {
        const QString startText = segmentCellText(row, 0);
        const QString stopText = segmentCellText(row, 1);
        const QString stepText = segmentCellText(row, 2);
        if (startText.isEmpty() || stopText.isEmpty() || stepText.isEmpty()) {
            continue;
        }

        bool startOk = false;
        bool stopOk = false;
        bool stepOk = false;
        const double startMhz = startText.toDouble(&startOk);
        const double stopMhz = stopText.toDouble(&stopOk);
        const double stepKhz = stepText.toDouble(&stepOk);
        if (!startOk || !stopOk || !stepOk || startMhz <= 0.0 || stopMhz <= startMhz || stepKhz <= 0.0) {
            continue;
        }

        config.segments.append({startMhz * 1000000.0, stopMhz * 1000000.0, stepKhz * 1000.0});
    }

    return config;
}

void SweepPage::startSweep()
{
    if (m_pscCommandThread) {
        return;
    }

    if (!m_ceyearService) {
        m_ceyearService = std::make_unique<CeyearVisaService>();
    }

    const CeyearPscSweepConfig config = sweepConfigFromTable();
    if (config.segments.isEmpty()) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("扫频频段无效，请检查起止频率和步进"));
        }
        return;
    }

    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("正在连接 3943B 设备...\n请稍候，设备连接可能需要几秒钟"));
    }
    resetPscVisualization(config);
    if (!startPscReceiver(config)) {
        return;
    }

    if (m_startButton) {
        m_startButton->setEnabled(false);
    }
    if (m_stopButton) {
        m_stopButton->setEnabled(false);
    }
    startPscSweepAsync(config);
}

void SweepPage::stopSweep()
{
    if (m_pscCommandThread) {
        return;
    }

    if (m_pscReceiver) {
        m_pscReceiver->stop();
    }
    if (m_startButton) {
        m_startButton->setEnabled(false);
    }
    if (m_stopButton) {
        m_stopButton->setEnabled(false);
    }
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("Stopping PSC sweep..."));
    }
    stopPscSweepAsync();
}

void SweepPage::startPscSweepAsync(const CeyearPscSweepConfig &config)
{
    if (!m_ceyearService || m_pscCommandThread) {
        return;
    }

    auto *service = m_ceyearService.get();
    const QString resource = defaultCeyearVisaResource();
    QPointer<SweepPage> page(this);
    QThread *thread = QThread::create([page, service, resource, config]() {
        CeyearCaptureResult result;
        try {
            result = service->startPscSweep(resource, config);
        } catch (const std::exception &ex) {
            result.resource = resource;
            result.captureName = QStringLiteral("3943B_PSC_SWEEP");
            result.message = QStringLiteral("3943B PSC sweep start crashed before completion: %1")
                                 .arg(QString::fromLocal8Bit(ex.what()));
        } catch (...) {
            result.resource = resource;
            result.captureName = QStringLiteral("3943B_PSC_SWEEP");
            result.message = QStringLiteral("3943B PSC sweep start crashed before completion.");
        }

        if (!page) {
            return;
        }
        QMetaObject::invokeMethod(page, [page, result]() {
            if (!page) {
                return;
            }
            page->finalizePscSweepStart(result);
        }, Qt::QueuedConnection);
    });
    m_pscCommandThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_pscCommandThread == thread) {
            m_pscCommandThread = nullptr;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void SweepPage::stopPscSweepAsync()
{
    if (!m_ceyearService || m_pscCommandThread) {
        return;
    }

    auto *service = m_ceyearService.get();
    const QString resource = defaultCeyearVisaResource();
    QPointer<SweepPage> page(this);
    QThread *thread = QThread::create([page, service, resource]() {
        CeyearCaptureResult result;
        try {
            result = service->stopPscSweep(resource);
        } catch (const std::exception &ex) {
            result.resource = resource;
            result.captureName = QStringLiteral("3943B_PSC_SWEEP");
            result.message = QStringLiteral("3943B PSC sweep stop crashed before completion: %1")
                                 .arg(QString::fromLocal8Bit(ex.what()));
        } catch (...) {
            result.resource = resource;
            result.captureName = QStringLiteral("3943B_PSC_SWEEP");
            result.message = QStringLiteral("3943B PSC sweep stop crashed before completion.");
        }

        if (!page) {
            return;
        }
        QMetaObject::invokeMethod(page, [page, result]() {
            if (!page) {
                return;
            }
            page->finalizePscSweepStop(result);
        }, Qt::QueuedConnection);
    });
    m_pscCommandThread = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_pscCommandThread == thread) {
            m_pscCommandThread = nullptr;
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void SweepPage::finalizePscSweepStart(const CeyearCaptureResult &result)
{
    if (result.success) {
        m_pscSweepActive = true;
        m_autoSaveCounter = 0;
        if (m_autoSaveTimer) {
            m_autoSaveTimer->start();
        }
        if (m_startButton) {
            m_startButton->setEnabled(false);
        }
        if (m_stopButton) {
            m_stopButton->setEnabled(true);
        }
        m_pscSweepMessage = pscSweepStatusText(result, QStringLiteral("PSC sweep started"));
        if (m_statusLabel) {
            updatePscReceiveStatus();
        }
        return;
    }

    m_pscSweepActive = false;
    if (m_pscReceiver) {
        m_pscReceiver->stop();
    }
    if (m_startButton) {
        m_startButton->setEnabled(true);
    }
    if (m_stopButton) {
        m_stopButton->setEnabled(false);
    }
    if (m_statusLabel) {
        m_statusLabel->setText(pscSweepStatusText(result, QStringLiteral("PSC sweep start failed")));
    }
}

void SweepPage::finalizePscSweepStop(const CeyearCaptureResult &result)
{
    m_pscSweepActive = false;
    if (m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }
    if (m_startButton) {
        m_startButton->setEnabled(true);
    }
    if (m_stopButton) {
        m_stopButton->setEnabled(false);
    }
    if (m_statusLabel) {
        m_statusLabel->setText(pscSweepStatusText(result, QStringLiteral("PSC sweep stopped")));
    }
}

bool SweepPage::startPscReceiver(const CeyearPscSweepConfig &config)
{
    if (!m_pscReceiver) {
        m_pscReceiver = std::make_unique<CeyearPscReceiver>();
        connect(m_pscReceiver.get(), &CeyearPscReceiver::framesReceived, this, &SweepPage::handlePscFrames);
        connect(m_pscReceiver.get(), &CeyearPscReceiver::datagramReceived, this, [this](qint64 bytesReceived, qint64 datagramCount) {
            m_pscBytesReceived = bytesReceived;
            m_pscDatagramCount = datagramCount;
            updatePscReceiveStatus();
        });
    }

    CeyearPscReceiverConfig receiverConfig;
    receiverConfig.bindAddress = QStringLiteral("0.0.0.0");
    receiverConfig.port = static_cast<quint16>(config.pcReceivePort);
    m_pscBytesReceived = 0;
    m_pscDatagramCount = 0;
    m_pscFrameCount = 0;

    QString error;
    if (!m_pscReceiver->start(receiverConfig, &error)) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("PSC UDP 接收启动失败：%1").arg(error));
        }
        return false;
    }

    m_pscSweepMessage = QStringLiteral("PSC UDP receiver listening on 0.0.0.0:%1, 3943B target %2:%3")
                            .arg(m_pscReceiver->boundPort())
                            .arg(config.pcReceiveIpAddress)
                            .arg(config.pcReceivePort);
    updatePscReceiveStatus();
    return true;
}

void SweepPage::resetPscVisualization(const CeyearPscSweepConfig &config)
{
    if (m_waterfall) {
        if (!config.segments.isEmpty()) {
            const CeyearPscSweepSegment &segment = config.segments.first();
            m_waterfall->setFrequencyRange(segment.startHz, segment.stopHz);
        } else {
            m_waterfall->setFrequencyRange(0.0, 0.0);
        }
        m_waterfall->clearFrames();
    }

    m_candidateExtractor = CeyearPscCandidateExtractor(config.hitThresholdDbuv);
    m_pscFrames.clear();
    if (m_saveButton) {
        m_saveButton->setEnabled(false);
    }
    if (m_candidateTable) {
        m_candidateTable->setRowCount(0);
    }
}

void SweepPage::handlePscFrames(const QVector<CeyearPscFrame> &frames)
{
    for (const CeyearPscFrame &frame : frames) {
        appendPscFrame(frame);
        ++m_pscFrameCount;
    }
    updatePscReceiveStatus();
}

void SweepPage::updatePscReceiveStatus()
{
    if (!m_statusLabel) {
        return;
    }

    QString text = m_pscSweepMessage;
    if (text.trimmed().isEmpty()) {
        text = QStringLiteral("PSC 扫频已启动");
    }
    text += QStringLiteral("\nUDP datagrams: %1, bytes: %2, PSC frames: %3")
                .arg(m_pscDatagramCount)
                .arg(m_pscBytesReceived)
                .arg(m_pscFrameCount);
    m_statusLabel->setText(text);
}

void SweepPage::appendPscFrame(const CeyearPscFrame &frame)
{
    m_pscFrames.append(frame);
    if (m_saveButton) {
        m_saveButton->setEnabled(true);
    }
    if (m_waterfall) {
        m_waterfall->appendFrame(frame);
    }

    m_candidateExtractor.update(frame);

    // 限制候选表刷新频率，时频图仍然保持实时有界缓存。
    static int frameCounter = 0;
    ++frameCounter;
    if (frameCounter % 100 == 0 || !m_candidateTable || m_candidateTable->rowCount() == 0) {
        const QVector<CeyearPscCandidate> candidates = m_candidateExtractor.getCandidates();
        setCandidateRows(candidates);
    }
}

void SweepPage::setCandidateRows(const QVector<CeyearPscCandidate> &candidates)
{
    if (!m_candidateTable) {
        return;
    }
    m_candidateTable->setRowCount(candidates.size());
    for (int row = 0; row < candidates.size(); ++row) {
        const CeyearPscCandidate &candidate = candidates.at(row);
        m_candidateTable->setItem(row, 0, centeredItem(QString::number(candidate.frequencyHz, 'f', 0)));
        m_candidateTable->setItem(row, 1, centeredItem(QString::number(candidate.peakLevelDbuv, 'f', 1)));
        m_candidateTable->setItem(row, 2, centeredItem(QString::number(candidate.hitCount)));
    }
}

void SweepPage::confirmSelectedCandidate()
{
    if (!m_candidateTable || m_candidateTable->currentRow() < 0) {
        return;
    }

    const QTableWidgetItem *frequencyItem = m_candidateTable->item(m_candidateTable->currentRow(), 0);
    if (!frequencyItem) {
        return;
    }

    bool ok = false;
    const double frequencyHz = frequencyItem->text().toDouble(&ok);
    if (ok) {
        emit frequencyConfirmed(frequencyHz);
    }
}

void SweepPage::saveSweepData()
{
    if (m_pscFrames.isEmpty()) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("暂无可保存的扫频数据"));
        }
        return;
    }

    const QString filePath = defaultPscSweepCsvPath();
    QString error;
    if (!saveCeyearPscFramesCsv(filePath, m_pscFrames, &error)) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("扫频数据保存失败：%1").arg(error));
        }
        return;
    }

    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("扫频数据已保存：%1").arg(QDir::toNativeSeparators(filePath)));
    }
}

void SweepPage::autoSaveSweepData()
{
    if (m_pscFrames.isEmpty()) {
        return;
    }

    ++m_autoSaveCounter;
    const QString sweepDir = QDir(ProjectPaths::dataDirectory()).filePath(QStringLiteral("sweep"));
    QDir().mkpath(sweepDir);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString filePath = QDir(sweepDir).filePath(
        QStringLiteral("3943b_psc_sweep_%1_part%2.csv").arg(stamp).arg(m_autoSaveCounter, 3, 10, QChar('0')));

    // 复制数据用于异步保存
    QVector<CeyearPscFrame> framesToSave = m_pscFrames;
    const int frameCount = framesToSave.size();
    m_pscFrames.clear();  // 立即清空，避免内存累积

    // 异步保存，避免阻塞UI线程
    QString statusMessage = m_pscSweepMessage;
    QPointer<QLabel> statusLabel(m_statusLabel);
    int saveCounter = m_autoSaveCounter;

    QThread *saveThread = QThread::create([framesToSave, filePath, statusMessage, statusLabel, saveCounter, frameCount]() {
        QString error;
        const bool success = saveCeyearPscFramesCsv(filePath, framesToSave, &error);

        if (statusLabel && success) {
            const QString msg = QStringLiteral("自动保存 #%1: %2 帧 -> %3")
                                    .arg(saveCounter)
                                    .arg(frameCount)
                                    .arg(QFileInfo(filePath).fileName());
            QMetaObject::invokeMethod(statusLabel, [statusLabel, statusMessage, msg]() {
                if (statusLabel) {
                    statusLabel->setText(statusMessage + QStringLiteral("\n") + msg);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(saveThread, &QThread::finished, saveThread, &QObject::deleteLater);
    saveThread->start();
}
