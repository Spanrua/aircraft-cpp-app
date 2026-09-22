#pragma once

#include <QMetaType>
#include <QProcess>
#include <QString>
#include <QStringList>

struct RecognitionRequest
{
    QString modelPath;
    QString samplePath;
    int sampleIndex = 0;
    int gpuDevice = 0;
};

struct RecognitionResult
{
    bool success = false;
    QString modelPath;
    QString samplePath;
    QString predictedType;
    QString predictedIndividual;
    double confidence = 0.0;
    QStringList topCandidates;
    QString device;
    QString modelSize;
    QString featureMode;
    QString message;
};
Q_DECLARE_METATYPE(RecognitionResult)

class RecognitionProcessRunner : public QObject
{
    Q_OBJECT

public:
    explicit RecognitionProcessRunner(QObject *parent = nullptr);
    ~RecognitionProcessRunner() override;

    bool start(const RecognitionRequest &request,
               const QString &pythonExecutableOverride = QString(),
               const QString &backendScriptOverride = QString());
    void cancel();
    bool isRunning() const;

    static QString pythonExecutablePath();
    static QString backendScriptPath();

signals:
    void started();
    void finished(const RecognitionResult &result);
    void failed(const QString &message);

private slots:
    void readProcessOutput();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);

private:
    void consumeLine(const QByteArray &line);
    void finishResult(int exitCode, QProcess::ExitStatus exitStatus);
    void appendLog(const QString &message);

    QProcess *m_process = nullptr;
    RecognitionRequest m_request;
    QByteArray m_stdoutBuffer;
    QStringList m_logs;
    RecognitionResult m_result;
    QString m_lastError;
    bool m_startedEventSeen = false;
    bool m_completedEventSeen = false;
    bool m_resultEmitted = false;
};
