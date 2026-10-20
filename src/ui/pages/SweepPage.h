#pragma once

#include "services/CeyearPscSweep.h"
#include "services/PscWidebandAlertDetector.h"
#include "ui/pages/WorkflowPage.h"

#include <memory>
#include <optional>

class ICeyearVisaService;
class CeyearPscReceiver;
struct CeyearCaptureResult;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QThread;
class QTimer;
class WaterfallWidget;

class SweepPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit SweepPage(QWidget *parent = nullptr);
    explicit SweepPage(std::unique_ptr<ICeyearVisaService> ceyearService, QWidget *parent = nullptr);
    ~SweepPage() override;

    void appendPscFrameForTest(const CeyearPscFrame &frame);
    void setCandidateRowsForTest(const QVector<CeyearPscCandidate> &candidates);
    void startOfflineReplayForTest(const QVector<CeyearPscFrame> &frames);

signals:
    void frequencyConfirmed(double frequencyHz);

private:
    void buildUi();
    void seedDefaultSegments();
    QString segmentCellText(int row, int column) const;
    CeyearPscSweepConfig sweepConfigFromTable() const;
    void startSweep();
    void stopSweep();
    void importOfflineSweepCsv();
    void startOfflineReplay(const QVector<CeyearPscFrame> &frames, const QString &sourcePath);
    void playNextOfflineFrame();
    void stopOfflineReplay();
    void finishOfflineReplay(bool completed);
    void updateOfflineReplayStatus() const;
    bool startPscReceiver(const CeyearPscSweepConfig &config);
    void startPscSweepAsync(const CeyearPscSweepConfig &config);
    void stopPscSweepAsync();
    void finalizePscSweepStart(const CeyearCaptureResult &result);
    void finalizePscSweepStop(const CeyearCaptureResult &result);
    void resetPscVisualization(const CeyearPscSweepConfig &config);
    void handlePscFrames(const QVector<CeyearPscFrame> &frames);
    void updatePscReceiveStatus();
    void appendPscFrame(const CeyearPscFrame &frame);
    void setCandidateRows(const QVector<CeyearPscCandidate> &candidates);
    void showWidebandAlert(const PscWidebandAlert &alert);
    void recordWidebandAlert(const PscWidebandAlert &alert);
    void confirmSelectedCandidate();
    void saveSweepData();
    void autoSaveSweepData();

    std::unique_ptr<ICeyearVisaService> m_ceyearService;
    std::unique_ptr<CeyearPscReceiver> m_pscReceiver;
    QThread *m_pscCommandThread = nullptr;
    CeyearPscCandidateExtractor m_candidateExtractor;
    PscWidebandAlertDetector m_widebandAlertDetector;
    QVector<PscWidebandAlert> m_widebandAlertHistory;
    QVector<CeyearPscFrame> m_pscFrames;
    QString m_pscSweepMessage;
    qint64 m_pscBytesReceived = 0;
    qint64 m_pscDatagramCount = 0;
    int m_pscFrameCount = 0;
    bool m_pscSweepActive = false;
    bool m_offlineReplayActive = false;
    QVector<CeyearPscFrame> m_offlineReplayFrames;
    int m_nextOfflineReplayFrame = 0;
    std::optional<double> m_pendingConfirmedFrequencyHz;
    int m_autoSaveCounter = 0;
    QTimer *m_autoSaveTimer = nullptr;
    QTimer *m_offlineReplayTimer = nullptr;
    QTableWidget *m_segmentTable = nullptr;
    QLineEdit *m_hitThresholdEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_widebandAlertLabel = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_importButton = nullptr;
    WaterfallWidget *m_waterfall = nullptr;
    QTableWidget *m_candidateTable = nullptr;
    QPushButton *m_confirmButton = nullptr;
};
