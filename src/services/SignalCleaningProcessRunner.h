#pragma once

#include <QMetaType>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

struct SignalCleaningRequest
{
    QString modelPath;
    QString backendScriptPath;
    QString inputRoot;
    QString outputDirectory;
    QString runId;
    double threshold = 0.5;
    int batchSize = 64;
    int cacheFiles = 2;
    int numWorkers = 0;
    bool useCpu = false;
};

struct SignalCleaningPrediction
{
    QString path;
    int index = 0;
    QString sampleId;
    QString group;
    double probability = 0.0;
    QString label;
};

struct SignalCleaningResult
{
    bool success = false;
    bool cancelled = false;
    QString runId;
    QString modelPath;
    QString inputRoot;
    QString outputDirectory;
    QString outputCsv;
    QString summaryPath;
    double threshold = 0.5;
    int totalRecords = 0;
    int signalRecords = 0;
    int noiseRecords = 0;
    QString device;
    QString message;
    QStringList logs;
    QVector<SignalCleaningPrediction> predictions;
};

Q_DECLARE_METATYPE(SignalCleaningPrediction)
Q_DECLARE_METATYPE(SignalCleaningResult)

class SignalCleaningProcessRunner : public QObject
{
    Q_OBJECT

public:
    explicit SignalCleaningProcessRunner(QObject *parent = nullptr);
    ~SignalCleaningProcessRunner() override;

    bool start(const SignalCleaningRequest &request,
               const QString &pythonExecutableOverride = QString());
    void cancel();
    bool isRunning() const;

    static QString pythonExecutablePath();
    static QString backendScriptPath();
    static QString modelPath();

signals:
    void started();
    void progressChanged(int percent, const QString &message);
    void finished(const SignalCleaningResult &result);
    void failed(const QString &message);
    void cancelled();

private slots:
    void readProcessOutput();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);

private:
    void consumeLine(const QByteArray &line);
    void finishResult(int exitCode, QProcess::ExitStatus exitStatus);
    bool loadResultFiles(QString *error);
    void appendLog(const QString &message);

    QProcess *m_process = nullptr;
    SignalCleaningRequest m_request;
    SignalCleaningResult m_result;
    QByteArray m_stdoutBuffer;
    QStringList m_logs;
    QString m_lastError;
    bool m_startedEventSeen = false;
    bool m_resultEmitted = false;
    bool m_cancelRequested = false;
};
