#pragma once

#include <QProcess>
#include <QMetaType>
#include <QString>
#include <QStringList>

class QTimer;

enum class TrainingTaskState
{
    Idle,
    Preparing,
    PrecomputingCache,
    Training,
    Completed,
    Cancelled,
    Failed
};

QString trainingTaskStateToString(TrainingTaskState state);

struct TrainingProgress
{
    QString runId;
    QString phase;
    int percent = 0;
    int epoch = 0;
    int epochs = 0;
    int trainWindows = 0;
    int valWindows = 0;
    double trainLoss = 0.0;
    double trainAccuracy = 0.0;
    double valAccuracy = 0.0;
    double bestAccuracy = 0.0;
    QString message;
};
Q_DECLARE_METATYPE(TrainingProgress)

struct TrainingRunRequest
{
    QString runId;
    QString datasetVersionId;
    QString datasetVersionDirectory;
    QString outputDirectory;
    int epochs = 6;
    int batchSize = 32;
    QString trainDataMode = QStringLiteral("raw");
    QString splitMode = QStringLiteral("source_group");
    double valRatio = 0.2;
    double internalDevRatio = 0.2;
    QString modelSize = QStringLiteral("axis_profile_xlarge");
    QString featureMode = QStringLiteral("complex_signed_mag");
    QString classifierHead = QStringLiteral("hierarchical");
    QString openSetAction = QStringLiteral("train");
    QString checkpointPath;
    QString cacheDirectory;
    QString cacheDataRoot;
    QString indexCacheDirectory;
    int precomputeBatchSize = 4096;
    bool overwriteCache = false;
    int stitchWindows = 8;
    int stitchStride = 4;
    int sampleLength = 32768;
    QString modelMode = QStringLiteral("closed_set");
    int numWorkers = 0;
    int seed = 42;
    bool deterministic = false;
    bool selectionOnly = true;
    bool precomputeCacheOnly = false;
    bool requireCuda = true;
    QStringList extraArgs;
};

struct TrainingRunResult
{
    bool success = false;
    bool cancelled = false;
    bool partial = false;
    bool timedOut = false;
    QString runId;
    QString datasetVersionId;
    TrainingTaskState state = TrainingTaskState::Idle;
    QString message;
    QString outputDirectory;
    QString summaryPath;
    QString bestModelPath;
    QString finalModelPath;
    int epoch = 0;
    int epochs = 0;
    int trainWindows = 0;
    int valWindows = 0;
    double bestValAccuracy = 0.0;
    double finalValAccuracy = 0.0;
    QString gpuDeviceName;
    QString pipelineVersion = QStringLiteral("CNNTRANS_v5");
    QString featureMode;
    QString modelSize;
    QString cacheDirectory;
    QString cacheSummaryPath;
    int sampleLength = 32768;
    QString modelMode = QStringLiteral("closed_set");
    bool precomputeCacheOnly = false;
    bool cacheReady = false;
    bool modelAvailable = false;
    QString device;
    QString splitSummaryPath;
    QStringList logs;
};
Q_DECLARE_METATYPE(TrainingRunResult)

class TrainingProcessRunner : public QObject
{
    Q_OBJECT

public:
    explicit TrainingProcessRunner(QObject *parent = nullptr);
    ~TrainingProcessRunner() override;

    bool start(
        const TrainingRunRequest &request,
        const QString &pythonExecutableOverride = QString(),
        const QString &backendScriptOverride = QString());
    void cancel();
    bool isRunning() const;

    static QString pythonExecutablePath();
    static QString backendScriptPath();

signals:
    void progressChanged(const TrainingProgress &progress);
    void finished(const TrainingRunResult &result);
    void failed(const QString &message);
    void cancelled();

private slots:
    void readProcessOutput();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);

private:
    void consumeLine(const QByteArray &line);
    void finishResult(int exitCode, QProcess::ExitStatus exitStatus);
    TrainingRunResult buildResult(int exitCode, QProcess::ExitStatus exitStatus) const;
    void appendLog(const QString &message);

    QProcess *m_process = nullptr;
    QTimer *m_cancelGraceTimer = nullptr;
    TrainingRunRequest m_request;
    QString m_cancelPath;
    QByteArray m_stdoutBuffer;
    QStringList m_logs;
    TrainingProgress m_latestProgress;
    TrainingRunResult m_latestResult;
    QString m_lastError;
    bool m_cancelRequested = false;
    bool m_cancelForced = false;
    bool m_completionEventSeen = false;
    bool m_failureEventSeen = false;
    bool m_resultEmitted = false;
};
