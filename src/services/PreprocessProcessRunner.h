#pragma once

#include "core/Records.h"
#include "services/CapProbeService.h"

#include <optional>

#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

enum class PreprocessTaskState
{
    Idle,
    Preparing,
    Reading,
    Detecting,
    Exporting,
    Finalizing,
    Completed,
    CompletedWithWarnings,
    Cancelled,
    TimedOut,
    Failed
};

QString preprocessTaskStateToString(PreprocessTaskState state);
PreprocessTaskState preprocessTaskStateFromString(const QString &value);

struct PreprocessProgress
{
    QString taskId;
    QString phase;
    int percent = 0;
    qint64 bytesRead = 0;
    qint64 totalBytes = 0;
    int chunksProcessed = 0;
    int candidateCount = 0;
    int sampleCount = 0;
    qint64 etaMs = -1;
    QString message;

    static PreprocessProgress fromJson(const QJsonObject &json);
};
Q_DECLARE_METATYPE(PreprocessProgress)

struct PreprocessSegment
{
    QString segmentId;
    QString candidateId;
    int startSample = 0;
    int endSample = 0;
    double durationMs = 0.0;
    double centerFreqHz = 0.0;
    double bandwidthHz = 0.0;
    double snrDb = 0.0;
    double score = 0.0;
    double occupancy = 0.0;
    QString outputFilePath;
    QString candidatePreviewFilePath;
    QString detectionModeDetail;
    QString status;

    QJsonObject toJson() const;
    static PreprocessSegment fromJson(const QJsonObject &json);
    bool isValid() const;
};

struct PreprocessRunResult
{
    bool success = false;
    QString message;
    QString taskId;
    PreprocessTaskState taskState = PreprocessTaskState::Idle;
    bool partial = false;
    bool cancelled = false;
    bool timedOut = false;
    qint64 bytesRead = 0;
    qint64 totalBytes = 0;
    double averageSnrDb = 0.0;
    double averageBandwidthHz = 0.0;
    QString qualitySummary;
    QString batchDirectory;
    QString progressPath;
    QString taskManifestPath;
    QString computeBackendRequested;
    QString computeBackend;
    bool gpuAvailable = false;
    bool gpuFallback = false;
    int gpuDevice = 0;
    QString gpuDeviceName;
    int detectedSegmentCount = 0;
    int candidateSegmentCount = 0;
    int outputSampleCount = 0;
    QString sampleOutputDir;
    QString sourceFormat;
    double sampleRateHz = 0.0;
    double centerFrequencyHz = 0.0;
    double bandwidthHz = 0.0;
    QString manifestPath;
    QString summaryPath;
    int exportedSegments = 0;
    int exportedWindows = 0;
    QVector<PreprocessSegment> segments;
    QStringList logs;
    QVector<SampleRecord> sampleRecords;
};
Q_DECLARE_METATYPE(PreprocessRunResult)

struct PreprocessRequest
{
    QString inputFilePath;
    QString taskId;
    QString batchId;
    int windowSamples = 4096;
    int chunkSamples = 2000000;
    QString payloadEndian = QStringLiteral("little");
    QString detectionMode = QStringLiteral("cv_band");
    double frequencySpanMhz = 12.0;
    double energyOffsetDb = 8.0;
    double targetBandwidthMhz = 20.0;
    double minBandwidthMhz = 16.0;
    double minWidebandFill = 0.42;
    double minWidebandSnrDb = 3.0;
    double morphFrequencyCloseMhz = 1.2;
    double shapeSplitQuantile = 55.0;
    double shapeSplitMergeGapMs = 0.05;
    double maxSegmentDurationMs = 2.0;
    double bandWindowMs = 2.0;
    double bandWindowOverlapMs = 0.0;
    double minBandScore = 0.02;
    double cvColumnQuantile = 40.0;
    double cvMinWindowOccupancy = 0.25;
    double cvMorphFrequencyMhz = 0.3;
    double cvMorphTimeMs = 0.05;
    double minDurationMs = 0.05;
    double mergeGapMs = 0.20;
    int nperseg = 2048;
    int noverlap = 1536;
    bool exportSpectrogramPng = false;
    int spectrogramNperseg = 512;
    int spectrogramNoverlap = 384;
    int spectrogramImageSize = 224;
    double spectrogramDynamicRangeDb = 60.0;
    double spectrogramFrequencySpanMhz = 12.0;
    QString sampleOutputDir;
    QString progressPath;
    QString cancelPath;
    QString label;
    QString computeBackend = QStringLiteral("cuda");
    int gpuDevice = 0;
    bool allowCpuFallback = false;
    int maxChunks = 0;
    int timeoutMs = 600000;
};

class PreprocessProcessRunner : public QObject
{
    Q_OBJECT

public:
    explicit PreprocessProcessRunner(QObject *parent = nullptr);
    ~PreprocessProcessRunner() override;

    static QJsonObject requestToJson(const PreprocessRequest &request);
    static bool writeRequestFile(const QString &path, const PreprocessRequest &request, QString *error = nullptr);
    static std::optional<PreprocessRunResult> readResponseFile(const QString &path, QString *error = nullptr);
    static QVector<SampleRecord> buildSampleRecords(const QString &inputCapPath, const CapProbeResult &capInfo, const PreprocessRunResult &result);
    static std::optional<PreprocessRunResult> run(
        const PreprocessRequest &request,
        const CapProbeResult &capInfo,
        QString *error = nullptr,
        const QString &pythonExecutable = QString(),
        const QString &runnerScript = QString());

    bool start(
        const PreprocessRequest &request,
        const CapProbeResult &capInfo,
        const QString &pythonExecutable = QString(),
        const QString &runnerScript = QString());
    void cancel();
    bool isRunning() const;
    QString taskDirectory() const;

signals:
    void progressChanged(const PreprocessProgress &progress);
    void partialResultAvailable(const PreprocessRunResult &result);
    void finished(const PreprocessRunResult &result);
    void failed(const QString &message);
    void cancelled();

private slots:
    void readProcessOutput();
    void readProgressFile();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);

private:
    static QString pythonExecutable();
    static QString runnerScriptPath();

    void emitPartialResult();
    std::optional<PreprocessRunResult> readPartialResult() const;
    void finishWithResult(const std::optional<PreprocessRunResult> &result, const QString &error);

    QProcess *m_process = nullptr;
    QTimer *m_progressTimer = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    PreprocessRequest m_request;
    CapProbeResult m_capInfo;
    QString m_requestPath;
    QString m_responsePath;
    QString m_progressReadPath;
    qint64 m_progressOffset = 0;
    QByteArray m_stdoutBuffer;
    bool m_cancelRequested = false;
    bool m_timedOut = false;
};
