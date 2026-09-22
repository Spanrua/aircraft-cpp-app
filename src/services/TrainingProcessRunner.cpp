#include "services/TrainingProcessRunner.h"

#include "app/ProjectPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <cmath>

namespace
{
QString trainingBackendDefaultPath()
{
    const QByteArray configured = qgetenv("AIRCRAFT_TRAINING_BACKEND");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QFileInfo(QString::fromLocal8Bit(configured)).absoluteFilePath();
    }
    const QStringList candidates = {
        QDir(ProjectPaths::cppAppRoot()).filePath(QStringLiteral("dji_training_backend_v5/training_backend.py")),
        QDir(ProjectPaths::cppAppRoot()).filePath(QStringLiteral("../dji_training_backend/training_backend.py")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("dji_training_backend/training_backend.py")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("../dji_training_backend/training_backend.py")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(QStringLiteral("../../dji_training_backend/training_backend.py")),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QFileInfo(candidates.first()).absoluteFilePath();
}

QString trainingPythonDefaultPath()
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

QString jsonNumber(const QJsonObject &json, const QString &key, const QString &fallback = QStringLiteral("-"))
{
    if (!json.contains(key)) {
        return fallback;
    }
    return QString::number(json.value(key).toDouble(), 'f', 2);
}
} // namespace

QString trainingTaskStateToString(TrainingTaskState state)
{
    switch (state) {
    case TrainingTaskState::Preparing: return QStringLiteral("准备中");
    case TrainingTaskState::PrecomputingCache: return QStringLiteral("生成缓存中");
    case TrainingTaskState::Training: return QStringLiteral("训练中");
    case TrainingTaskState::Completed: return QStringLiteral("已完成");
    case TrainingTaskState::Cancelled: return QStringLiteral("已取消");
    case TrainingTaskState::Failed: return QStringLiteral("失败");
    case TrainingTaskState::Idle:
    default: return QStringLiteral("待执行");
    }
}

TrainingProcessRunner::TrainingProcessRunner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<TrainingProgress>("TrainingProgress");
    qRegisterMetaType<TrainingRunResult>("TrainingRunResult");

    m_cancelGraceTimer = new QTimer(this);
    m_cancelGraceTimer->setSingleShot(true);
    m_cancelGraceTimer->setInterval(5000);
    connect(m_cancelGraceTimer, &QTimer::timeout, this, [this]() {
        if (isRunning()) {
            m_cancelForced = true;
            m_process->kill();
        }
    });
}

TrainingProcessRunner::~TrainingProcessRunner()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

bool TrainingProcessRunner::start(
    const TrainingRunRequest &request,
    const QString &pythonExecutableOverride,
    const QString &backendScriptOverride)
{
    if (isRunning()) {
        return false;
    }
    if (request.datasetVersionDirectory.trimmed().isEmpty()
        || !QFileInfo(request.datasetVersionDirectory).isDir()) {
        emit failed(QStringLiteral("数据集版本目录不存在。"));
        return false;
    }
    const QString manifestPath = QDir(request.datasetVersionDirectory).filePath(QStringLiteral("manifest.jsonl"));
    if (!QFileInfo::exists(manifestPath)) {
        emit failed(QStringLiteral("数据集版本清单不存在：%1").arg(manifestPath));
        return false;
    }
    if (request.outputDirectory.trimmed().isEmpty()) {
        emit failed(QStringLiteral("训练输出目录不能为空。"));
        return false;
    }
    if (request.stitchWindows < 1 || request.stitchStride < 1) {
        emit failed(QStringLiteral("拼接窗口数和步长必须至少为 1。"));
        return false;
    }
    if (request.sampleLength != request.stitchWindows * 4096) {
        emit failed(QStringLiteral("样本长度与拼接窗口数不一致：%1 != 4096 × %2。")
                        .arg(request.sampleLength)
                        .arg(request.stitchWindows));
        return false;
    }
    if (request.trainDataMode == QStringLiteral("cache")
        && request.cacheDirectory.trimmed().isEmpty()) {
        emit failed(QStringLiteral("使用特征缓存前，请先指定项目数据目录下的 cache 输出目录。"));
        return false;
    }
    if (!QDir().mkpath(request.outputDirectory)) {
        emit failed(QStringLiteral("无法创建训练输出目录：%1").arg(request.outputDirectory));
        return false;
    }
    const QString backendScript = backendScriptOverride.isEmpty()
        ? backendScriptPath()
        : backendScriptOverride;
    if (!QFileInfo::exists(backendScript)) {
        emit failed(QStringLiteral("训练后端脚本不存在：%1").arg(backendScript));
        return false;
    }

    m_request = request;
    if (m_request.runId.isEmpty()) {
        m_request.runId = QStringLiteral("train_%1")
                              .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    }
    m_cancelPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("cancel.flag"));
    QFile::remove(m_cancelPath);
    m_stdoutBuffer.clear();
    m_logs.clear();
    m_latestProgress = TrainingProgress();
    m_latestResult = TrainingRunResult();
    m_lastError.clear();
    m_cancelRequested = false;
    m_cancelForced = false;
    m_completionEventSeen = false;
    m_failureEventSeen = false;
    m_resultEmitted = false;

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(QFileInfo(backendScript).absolutePath());
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &TrainingProcessRunner::readProcessOutput);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &TrainingProcessRunner::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &TrainingProcessRunner::handleProcessError);
    m_process->setProgram(pythonExecutableOverride.isEmpty() ? pythonExecutablePath() : pythonExecutableOverride);
    QStringList arguments = {
        backendScript,
        QStringLiteral("--dataset-version"),
        m_request.datasetVersionDirectory,
        QStringLiteral("--output-dir"),
        m_request.outputDirectory,
        QStringLiteral("--epochs"),
        QString::number(m_request.epochs),
        QStringLiteral("--batch-size"),
        QString::number(m_request.batchSize),
        QStringLiteral("--stitch-windows"),
        QString::number(m_request.stitchWindows),
        QStringLiteral("--stitch-stride"),
        QString::number(m_request.stitchStride),
        QStringLiteral("--split-mode"),
        m_request.splitMode,
        QStringLiteral("--val-ratio"),
        QString::number(m_request.valRatio, 'f', 4),
        QStringLiteral("--model-arch"),
        m_request.modelSize,
        QStringLiteral("--feature-mode"),
        m_request.featureMode,
        QStringLiteral("--classifier-head"),
        m_request.classifierHead,
        QStringLiteral("--model-mode"),
        m_request.modelMode,
        QStringLiteral("--train-data-mode"),
        m_request.trainDataMode,
        QStringLiteral("--internal-dev-ratio"),
        QString::number(m_request.internalDevRatio, 'f', 4),
        QStringLiteral("--num-workers"),
        QString::number(m_request.numWorkers),
        QStringLiteral("--seed"),
        QString::number(m_request.seed),
    };
    const bool shouldPassCacheDirectory = m_request.precomputeCacheOnly
        || m_request.trainDataMode == QStringLiteral("cache");
    if (shouldPassCacheDirectory && !m_request.cacheDirectory.trimmed().isEmpty()) {
        arguments << QStringLiteral("--cache-dir") << m_request.cacheDirectory;
    }
    if (m_request.modelMode == QStringLiteral("open_set")) {
        arguments << QStringLiteral("--open-set-action") << m_request.openSetAction;
        if (!m_request.checkpointPath.trimmed().isEmpty()) {
            arguments << QStringLiteral("--checkpoint") << m_request.checkpointPath;
        }
    }
    if (m_request.precomputeCacheOnly) {
        arguments << QStringLiteral("--precompute-cache-only")
                  << QStringLiteral("--precompute-batch-size")
                  << QString::number(m_request.precomputeBatchSize);
        if (m_request.overwriteCache) {
            arguments.append(QStringLiteral("--overwrite-cache"));
        }
    }
    if (!m_request.cacheDataRoot.trimmed().isEmpty()) {
        arguments << QStringLiteral("--cache-data-root") << m_request.cacheDataRoot;
    }
    if (!m_request.indexCacheDirectory.trimmed().isEmpty()) {
        arguments << QStringLiteral("--index-cache-dir") << m_request.indexCacheDirectory;
    }
    if (m_request.selectionOnly) {
        arguments.append(QStringLiteral("--selection-only"));
    }
    if (m_request.deterministic) {
        arguments.append(QStringLiteral("--deterministic"));
    }
    if (m_request.requireCuda) {
        arguments.append(QStringLiteral("--require-cuda"));
    }
    arguments << QStringLiteral("--cancel-flag") << m_cancelPath;
    arguments.append(m_request.extraArgs);
    m_process->setArguments(arguments);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_process->setProcessEnvironment(environment);
    m_process->start();
    if (!m_process->waitForStarted(3000)) {
        const QString error = QStringLiteral("无法启动训练 Python 进程：%1").arg(m_process->errorString());
        m_process->deleteLater();
        m_process = nullptr;
        emit failed(error);
        return false;
    }

    emit progressChanged(TrainingProgress{
        m_request.runId,
        QStringLiteral("准备中"),
        0,
        0,
        m_request.epochs,
        0,
        0,
        0.0,
        0.0,
        0.0,
        0.0,
        m_request.precomputeCacheOnly ? QStringLiteral("正在启动缓存预计算后端")
                                      : QStringLiteral("正在启动模型训练后端")
    });
    return true;
}

void TrainingProcessRunner::cancel()
{
    if (!isRunning() || m_cancelRequested) {
        return;
    }
    m_cancelRequested = true;
    QFile flag(m_cancelPath);
    if (flag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        flag.write("cancel\n");
        flag.close();
    }
    appendLog(QStringLiteral("已请求停止训练，正在等待训练进程退出。"));
    emit progressChanged(TrainingProgress{
        m_request.runId,
        QStringLiteral("停止中"),
        m_latestProgress.percent,
        m_latestProgress.epoch,
        m_latestProgress.epochs,
        m_latestProgress.trainWindows,
        m_latestProgress.valWindows,
        m_latestProgress.trainLoss,
        m_latestProgress.trainAccuracy,
        m_latestProgress.valAccuracy,
        m_latestProgress.bestAccuracy,
        QStringLiteral("已请求停止训练")
    });
    m_cancelGraceTimer->start();
}

bool TrainingProcessRunner::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

QString TrainingProcessRunner::pythonExecutablePath()
{
    return trainingPythonDefaultPath();
}

QString TrainingProcessRunner::backendScriptPath()
{
    return trainingBackendDefaultPath();
}

void TrainingProcessRunner::readProcessOutput()
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

void TrainingProcessRunner::consumeLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        const QString text = QString::fromUtf8(line).trimmed();
        appendLog(text);
        if (m_request.precomputeCacheOnly) {
            static const QRegularExpression progressPattern(
                QStringLiteral("Precomputing spectrogram cache:\\s*([0-9]+)%"));
            const QRegularExpressionMatch match = progressPattern.match(text);
            if (match.hasMatch()) {
                m_latestProgress.phase = QStringLiteral("生成缓存");
                m_latestProgress.percent = qBound(0, match.captured(1).toInt(), 100);
                m_latestProgress.message = QStringLiteral("正在生成时频缓存（%1%）")
                                               .arg(m_latestProgress.percent);
                m_latestProgress.runId = m_request.runId;
                emit progressChanged(m_latestProgress);
            } else if (text.startsWith(QStringLiteral("Estimated spectrogram cache size:"))) {
                m_latestProgress.phase = QStringLiteral("准备缓存");
                m_latestProgress.message = text;
                m_latestProgress.runId = m_request.runId;
                emit progressChanged(m_latestProgress);
            }
        }
        return;
    }
    const QJsonObject json = document.object();
    const QString type = json.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("started")) {
        m_latestProgress.phase = QStringLiteral("准备中");
        m_latestProgress.message = QStringLiteral("训练进程已启动");
        appendLog(QStringLiteral("训练后端已启动。"));
    } else if (type == QStringLiteral("dataset")) {
        m_latestProgress.phase = QStringLiteral("读取数据");
        m_latestProgress.trainWindows = json.value(QStringLiteral("train_windows")).toInt();
        m_latestProgress.valWindows = json.value(QStringLiteral("official_val_windows"))
                                          .toInt(json.value(QStringLiteral("selection_windows")).toInt());
        m_latestProgress.message = QStringLiteral("数据集索引完成");
        appendLog(QStringLiteral("训练窗口 %1，选择窗口 %2，官方验证窗口 %3。")
                      .arg(m_latestProgress.trainWindows)
                      .arg(json.value(QStringLiteral("selection_windows")).toInt())
                      .arg(json.value(QStringLiteral("official_val_windows")).toInt()));
    } else if (type == QStringLiteral("epoch")) {
        m_latestProgress.phase = QStringLiteral("训练中");
        m_latestProgress.epoch = json.value(QStringLiteral("epoch")).toInt();
        m_latestProgress.epochs = json.value(QStringLiteral("epochs")).toInt(m_request.epochs);
        m_latestProgress.percent = m_latestProgress.epochs > 0
            ? qBound(0, static_cast<int>(std::round(100.0 * m_latestProgress.epoch / m_latestProgress.epochs)), 100)
            : 0;
        m_latestProgress.trainLoss = json.value(QStringLiteral("train_loss")).toDouble();
        m_latestProgress.trainAccuracy = json.value(QStringLiteral("train_acc")).toDouble();
        m_latestProgress.valAccuracy = json.value(QStringLiteral("val_acc"))
                                           .toDouble(json.value(QStringLiteral("selection_val_acc")).toDouble());
        m_latestProgress.bestAccuracy = json.value(QStringLiteral("best_acc"))
                                            .toDouble(json.value(QStringLiteral("best_val_acc")).toDouble());
        m_latestProgress.message = QStringLiteral("第 %1 / %2 轮完成").arg(m_latestProgress.epoch).arg(m_latestProgress.epochs);
        appendLog(QStringLiteral("%1，训练准确率 %2%，验证准确率 %3%，最佳 %4%。")
                      .arg(m_latestProgress.message)
                      .arg(jsonNumber(json, QStringLiteral("train_acc")))
                      .arg(jsonNumber(json, QStringLiteral("val_acc")))
                      .arg(json.contains(QStringLiteral("best_acc"))
                               ? jsonNumber(json, QStringLiteral("best_acc"))
                               : jsonNumber(json, QStringLiteral("best_val_acc"))));
    } else if (type == QStringLiteral("result")) {
        m_latestProgress.phase = QStringLiteral("评估中");
        m_latestProgress.message = QStringLiteral("模型评估结果已生成");
        m_latestProgress.bestAccuracy = json.value(QStringLiteral("best_accuracy"))
                                            .toDouble(json.value(QStringLiteral("best_val_acc"))
                                                          .toDouble(m_latestProgress.bestAccuracy));
        m_latestProgress.valAccuracy = json.value(QStringLiteral("final_accuracy"))
                                           .toDouble(json.value(QStringLiteral("official_val_acc"))
                                                         .toDouble(m_latestProgress.valAccuracy));
        appendLog(QStringLiteral("训练结果：最佳准确率 %1%，最终准确率 %2%。")
                      .arg(json.contains(QStringLiteral("best_accuracy"))
                               ? jsonNumber(json, QStringLiteral("best_accuracy"))
                               : jsonNumber(json, QStringLiteral("best_val_acc")))
                      .arg(json.contains(QStringLiteral("final_accuracy"))
                               ? jsonNumber(json, QStringLiteral("final_accuracy"))
                               : jsonNumber(json, QStringLiteral("official_val_acc"))));
    } else if (type == QStringLiteral("log")) {
        appendLog(json.value(QStringLiteral("message")).toString());
    } else if (type == QStringLiteral("completed")) {
        m_completionEventSeen = true;
        m_latestResult.summaryPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("run_summary.json"));
        m_latestResult.message = QStringLiteral("训练完成");
    } else if (type == QStringLiteral("failed")) {
        m_failureEventSeen = true;
        m_lastError = json.value(QStringLiteral("error")).toString();
        if (m_lastError.isEmpty()) {
            m_lastError = json.value(QStringLiteral("message")).toString();
        }
    } else if (type == QStringLiteral("cancelled")) {
        m_cancelRequested = true;
        appendLog(json.value(QStringLiteral("message")).toString(
            QStringLiteral("后端已响应取消请求。")));
    }
    m_latestProgress.runId = m_request.runId;
    emit progressChanged(m_latestProgress);
}

void TrainingProcessRunner::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readProcessOutput();
    finishResult(exitCode, exitStatus);
}

void TrainingProcessRunner::handleProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        m_lastError = m_process ? m_process->errorString() : QStringLiteral("训练进程启动失败");
        finishResult(-1, QProcess::CrashExit);
    }
}

void TrainingProcessRunner::finishResult(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_resultEmitted) {
        return;
    }
    m_resultEmitted = true;
    if (m_cancelGraceTimer) {
        m_cancelGraceTimer->stop();
    }
    const TrainingRunResult result = buildResult(exitCode, exitStatus);
    if (result.cancelled) {
        emit cancelled();
    }
    if (!result.success && !result.cancelled && m_failureEventSeen) {
        emit failed(result.message);
    }
    emit finished(result);
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
}

TrainingRunResult TrainingProcessRunner::buildResult(int exitCode, QProcess::ExitStatus exitStatus) const
{
    TrainingRunResult result;
    result.success = exitCode == 0 && exitStatus == QProcess::NormalExit && !m_cancelRequested;
    result.cancelled = m_cancelRequested;
    result.timedOut = m_cancelForced;
    result.runId = m_request.runId;
    result.datasetVersionId = m_request.datasetVersionId;
    result.state = result.cancelled
        ? TrainingTaskState::Cancelled
        : (result.success ? TrainingTaskState::Completed : TrainingTaskState::Failed);
    result.message = result.timedOut
        ? QStringLiteral("训练进程未在取消宽限期内退出，已终止；已保留已经写入的输出。")
        : result.cancelled
        ? (m_request.precomputeCacheOnly
            ? QStringLiteral("缓存生成已取消，已保留已经生成的缓存文件。")
            : QStringLiteral("训练已取消，已保留训练输出。"))
        : (result.success ? (m_request.precomputeCacheOnly
            ? QStringLiteral("预计算缓存完成。")
            : QStringLiteral("训练完成。")) : (m_lastError.isEmpty()
            ? QStringLiteral("训练进程失败，退出码 %1。").arg(exitCode)
            : m_lastError));
    result.outputDirectory = m_request.outputDirectory;
    result.cacheDirectory = m_request.cacheDirectory;
    result.partial = result.cancelled || result.timedOut
        || (!result.success && (QFileInfo::exists(result.outputDirectory)
                                || QFileInfo::exists(result.cacheDirectory)));
    result.cacheSummaryPath = m_request.cacheDirectory.isEmpty()
        ? QString()
        : QDir(m_request.cacheDirectory).filePath(QStringLiteral("cache_summary.json"));
    result.cacheReady = !result.cacheSummaryPath.isEmpty()
        && QFileInfo::exists(result.cacheSummaryPath);
    result.precomputeCacheOnly = m_request.precomputeCacheOnly;
    result.summaryPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("run_summary.json"));
    result.splitSummaryPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("split_audit.json"));
    result.bestModelPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("best_model.pth"));
    QString rootSummaryPath = result.summaryPath;
    if (m_request.modelMode == QStringLiteral("open_set")) {
        rootSummaryPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("run_summary.json"));
    }
    result.finalModelPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("final_model.pth"));
    result.modelAvailable = m_request.modelMode == QStringLiteral("closed_set")
        && (QFileInfo::exists(result.bestModelPath)
            || QFileInfo::exists(QDir(m_request.outputDirectory).filePath(QStringLiteral("best_checkpoint.pth"))));
    result.sampleLength = m_request.sampleLength;
    result.modelMode = m_request.modelMode;
    result.epoch = m_latestProgress.epoch;
    result.epochs = m_latestProgress.epochs > 0 ? m_latestProgress.epochs : m_request.epochs;
    result.trainWindows = m_latestProgress.trainWindows;
    result.valWindows = m_latestProgress.valWindows;
    result.bestValAccuracy = m_latestProgress.bestAccuracy;
    result.finalValAccuracy = m_latestProgress.valAccuracy;
    QFile summaryFile(rootSummaryPath);
    if (summaryFile.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument summaryDocument = QJsonDocument::fromJson(summaryFile.readAll(), &parseError);
        if (summaryDocument.isObject()) {
            const QJsonObject summary = summaryDocument.object();
            result.bestValAccuracy = summary.value(QStringLiteral("best_val_acc")).toDouble(result.bestValAccuracy);
            result.finalValAccuracy = summary.value(QStringLiteral("best_model_evaluated_acc"))
                                          .toDouble(summary.value(QStringLiteral("official_val_acc"))
                                                        .toDouble(result.finalValAccuracy));
            result.trainWindows = summary.value(QStringLiteral("train_windows")).toInt(result.trainWindows);
            result.valWindows = summary.value(QStringLiteral("val_windows"))
                                    .toInt(summary.value(QStringLiteral("official_val_windows"))
                                               .toInt(summary.value(QStringLiteral("selection_windows"))
                                                           .toInt(result.valWindows)));
            if (m_request.modelMode == QStringLiteral("open_set")) {
                const QString bestModel = summary.value(QStringLiteral("best_model_path")).toString();
                const QString bestOutput = summary.value(QStringLiteral("best_seed_output_dir")).toString();
                result.bestModelPath = bestModel.isEmpty()
                    ? (bestOutput.isEmpty()
                           ? QString()
                           : QDir(bestOutput).filePath(QStringLiteral("best_model.pth")))
                    : bestModel;
                result.finalModelPath = summary.value(QStringLiteral("final_model_path")).toString();
                result.bestValAccuracy = summary.value(QStringLiteral("best_metric"))
                                             .toDouble(result.bestValAccuracy);
                result.finalValAccuracy = summary.value(QStringLiteral("final_metric"))
                                              .toDouble(result.finalValAccuracy);
            }
        }
    }
    QFile configFile(QDir(m_request.outputDirectory).filePath(QStringLiteral("run_config.json")));
    if (configFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument configDocument = QJsonDocument::fromJson(configFile.readAll());
        if (configDocument.isObject()) {
            const QJsonObject config = configDocument.object();
            const QJsonObject args = config.value(QStringLiteral("args")).toObject();
            result.gpuDeviceName = config.value(QStringLiteral("device")).toString();
            result.device = result.gpuDeviceName;
            const QString gpuName = config.value(QStringLiteral("gpu_name")).toString();
            if (!gpuName.isEmpty()) {
                result.gpuDeviceName = gpuName;
            }
            result.pipelineVersion = QStringLiteral("CNNTRANS_v5");
            result.featureMode = config.value(QStringLiteral("feature_mode"))
                                     .toString(args.value(QStringLiteral("feature_mode")).toString());
            result.modelSize = config.value(QStringLiteral("model_arch")).toString(
                args.value(QStringLiteral("model_arch")).toString(
                    config.value(QStringLiteral("model_size")).toString()));
        }
    }
    result.logs = m_logs;
    return result;
}

void TrainingProcessRunner::appendLog(const QString &message)
{
    const QString text = message.trimmed();
    if (!text.isEmpty()) {
        m_logs.append(text);
    }
}
