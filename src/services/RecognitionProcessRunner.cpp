#include "services/RecognitionProcessRunner.h"

#include "app/ProjectPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace
{
QString recognitionBackendDefaultPath()
{
    const QByteArray configured = qgetenv("AIRCRAFT_RECOGNITION_BACKEND");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QFileInfo(QString::fromLocal8Bit(configured)).absoluteFilePath();
    }
    const QStringList candidates = {
        QDir(ProjectPaths::cppAppRoot()).filePath(QStringLiteral("dji_training_backend_v5/recognition_backend.py")),
        QDir(ProjectPaths::cppAppRoot()).filePath(QStringLiteral("../dji_training_backend/recognition_backend.py")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("dji_training_backend/recognition_backend.py")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("../dji_training_backend/recognition_backend.py")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("../../dji_training_backend/recognition_backend.py")),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QFileInfo(candidates.first()).absoluteFilePath();
}

QString recognitionPythonDefaultPath()
{
    const QByteArray configured = qgetenv("AIRCRAFT_TRAINING_PYTHON");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QFileInfo(QString::fromLocal8Bit(configured)).absoluteFilePath();
    }
    const QStringList candidates = {
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("python_envs/aircraft_preprocess_gpu/Scripts/python.exe")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("../python_envs/aircraft_preprocess_gpu/Scripts/python.exe")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("python_envs/aircraft_preprocess_gpu/bin/python")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("../python_envs/aircraft_preprocess_gpu/bin/python")),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()
        ? QStringLiteral("python")
        : QStringLiteral("python3");
}
} // namespace

RecognitionProcessRunner::RecognitionProcessRunner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<RecognitionResult>("RecognitionResult");
}

RecognitionProcessRunner::~RecognitionProcessRunner()
{
    cancel();
    if (m_process) {
        m_process->waitForFinished(1000);
    }
}

bool RecognitionProcessRunner::start(const RecognitionRequest &request,
                                     const QString &pythonExecutableOverride,
                                     const QString &backendScriptOverride)
{
    if (isRunning()) {
        return false;
    }
    if (request.modelPath.trimmed().isEmpty() || !QFileInfo(request.modelPath).isFile()) {
        emit failed(QStringLiteral("模型文件不存在。"));
        return false;
    }
    if (request.samplePath.trimmed().isEmpty() || !QFileInfo(request.samplePath).isFile()) {
        emit failed(QStringLiteral("样本 NPZ 文件不存在。"));
        return false;
    }
    const QString backend = backendScriptOverride.isEmpty() ? backendScriptPath() : backendScriptOverride;
    if (!QFileInfo(backend).isFile()) {
        emit failed(QStringLiteral("识别后端脚本不存在：%1").arg(backend));
        return false;
    }

    m_request = request;
    m_stdoutBuffer.clear();
    m_logs.clear();
    m_result = RecognitionResult();
    m_lastError.clear();
    m_startedEventSeen = false;
    m_completedEventSeen = false;
    m_resultEmitted = false;

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(QFileInfo(backend).absolutePath());
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &RecognitionProcessRunner::readProcessOutput);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &RecognitionProcessRunner::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &RecognitionProcessRunner::handleProcessError);
    m_process->setProgram(pythonExecutableOverride.isEmpty() ? pythonExecutablePath() : pythonExecutableOverride);
    m_process->setArguments({
        backend,
        QStringLiteral("--model"), request.modelPath,
        QStringLiteral("--sample"), request.samplePath,
        QStringLiteral("--sample-index"), QString::number(request.sampleIndex),
    });
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    environment.insert(QStringLiteral("CUDA_VISIBLE_DEVICES"), QString::number(request.gpuDevice));
    m_process->setProcessEnvironment(environment);
    m_process->start();
    if (!m_process->waitForStarted(3000)) {
        const QString error = QStringLiteral("无法启动识别 Python 进程：%1").arg(m_process->errorString());
        m_process->deleteLater();
        m_process = nullptr;
        emit failed(error);
        return false;
    }
    return true;
}

void RecognitionProcessRunner::cancel()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
}

bool RecognitionProcessRunner::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

QString RecognitionProcessRunner::pythonExecutablePath()
{
    return recognitionPythonDefaultPath();
}

QString RecognitionProcessRunner::backendScriptPath()
{
    return recognitionBackendDefaultPath();
}

void RecognitionProcessRunner::readProcessOutput()
{
    if (!m_process) {
        return;
    }
    m_stdoutBuffer += m_process->readAll();
    int newlineIndex = -1;
    while ((newlineIndex = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newlineIndex).trimmed();
        m_stdoutBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            consumeLine(line);
        }
    }
}

void RecognitionProcessRunner::consumeLine(const QByteArray &line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        appendLog(QString::fromUtf8(line));
        return;
    }
    const QJsonObject json = document.object();
    const QString type = json.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("started")) {
        m_startedEventSeen = true;
        emit started();
    } else if (type == QStringLiteral("completed")) {
        m_completedEventSeen = true;
        m_result.success = true;
        m_result.modelPath = json.value(QStringLiteral("model_path")).toString(m_request.modelPath);
        m_result.samplePath = json.value(QStringLiteral("sample_path")).toString(m_request.samplePath);
        m_result.predictedType = json.value(QStringLiteral("predicted_type")).toString();
        m_result.predictedIndividual = json.value(QStringLiteral("predicted_individual")).toString();
        m_result.confidence = json.value(QStringLiteral("confidence")).toDouble();
        m_result.device = json.value(QStringLiteral("device")).toString();
        m_result.modelSize = json.value(QStringLiteral("model_size")).toString();
        m_result.featureMode = json.value(QStringLiteral("feature_mode")).toString();
        QStringList candidates;
        for (const QJsonValue &value : json.value(QStringLiteral("top_candidates")).toArray()) {
            const QJsonObject candidate = value.toObject();
            candidates.append(QStringLiteral("%1 %2%")
                                 .arg(candidate.value(QStringLiteral("label")).toString())
                                 .arg(candidate.value(QStringLiteral("confidence")).toDouble(), 0, 'f', 2));
        }
        m_result.topCandidates = candidates;
        m_result.message = QStringLiteral("识别完成");
    } else if (type == QStringLiteral("failed")) {
        m_lastError = json.value(QStringLiteral("error")).toString();
        if (m_lastError.isEmpty()) {
            m_lastError = QStringLiteral("识别后端返回失败。");
        }
    }
}

void RecognitionProcessRunner::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readProcessOutput();
    finishResult(exitCode, exitStatus);
}

void RecognitionProcessRunner::handleProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        m_lastError = m_process ? m_process->errorString() : QStringLiteral("识别进程启动失败");
        finishResult(-1, QProcess::CrashExit);
    }
}

void RecognitionProcessRunner::finishResult(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_resultEmitted) {
        return;
    }
    m_resultEmitted = true;
    if (!m_completedEventSeen) {
        m_result.success = false;
        m_result.modelPath = m_request.modelPath;
        m_result.samplePath = m_request.samplePath;
        m_result.message = m_lastError.isEmpty()
            ? QStringLiteral("识别进程失败，退出码 %1。").arg(exitCode)
            : m_lastError;
        emit failed(m_result.message);
    } else if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
        m_result.success = false;
        m_result.message = QStringLiteral("识别进程异常退出。");
        emit failed(m_result.message);
    }
    emit finished(m_result);
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void RecognitionProcessRunner::appendLog(const QString &message)
{
    const QString text = message.trimmed();
    if (!text.isEmpty()) {
        m_logs.append(text);
    }
}
