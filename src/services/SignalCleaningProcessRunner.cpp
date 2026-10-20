#include "services/SignalCleaningProcessRunner.h"

#include "app/ProjectPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QtGlobal>

#include <cmath>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif

namespace
{
QString firstExistingFile(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return candidates.isEmpty() ? QString() : QFileInfo(candidates.first()).absoluteFilePath();
}

QString defaultPythonPath()
{
    const QByteArray configured = qgetenv("AIRCRAFT_SIGNAL_NOISE_PYTHON");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QFileInfo(QString::fromLocal8Bit(configured)).absoluteFilePath();
    }

    const QStringList candidates = {
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(
            QStringLiteral("python_envs/aircraft_preprocess_gpu/Scripts/python.exe")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(
            QStringLiteral("../python_envs/aircraft_preprocess_gpu/Scripts/python.exe")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(
            QStringLiteral("python_envs/aircraft_preprocess_gpu/bin/python")),
        QDir(ProjectPaths::aircraftProjectRoot()).filePath(
            QStringLiteral("../python_envs/aircraft_preprocess_gpu/bin/python")),
    };
    const QString existing = firstExistingFile(candidates);
    if (!existing.isEmpty() && QFileInfo::exists(existing)) {
        return existing;
    }
    return QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()
        ? QStringLiteral("python")
        : QStringLiteral("python3");
}

QString csvField(const QStringList &fields, int index)
{
    return index >= 0 && index < fields.size() ? fields.at(index) : QString();
}

QStringList parseCsvLine(const QString &line)
{
    QStringList fields;
    QString current;
    bool quoted = false;
    for (int index = 0; index < line.size(); ++index) {
        const QChar character = line.at(index);
        if (character == QLatin1Char('"')) {
            if (quoted && index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                current.append(QLatin1Char('"'));
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == QLatin1Char(',') && !quoted) {
            fields.append(current);
            current.clear();
        } else {
            current.append(character);
        }
    }
    fields.append(current);
    return fields;
}

QString normalizedInputRoot(const QString &path)
{
    const QFileInfo info(path);
    if (info.isFile() && info.fileName().compare(QStringLiteral("manifest.jsonl"), Qt::CaseInsensitive) == 0) {
        return info.absolutePath();
    }
    return info.absoluteFilePath();
}
} // namespace

SignalCleaningProcessRunner::SignalCleaningProcessRunner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<SignalCleaningPrediction>("SignalCleaningPrediction");
    qRegisterMetaType<SignalCleaningResult>("SignalCleaningResult");
}

SignalCleaningProcessRunner::~SignalCleaningProcessRunner()
{
    cancel();
    if (m_process) {
        m_process->waitForFinished(1000);
    }
}

bool SignalCleaningProcessRunner::start(
    const SignalCleaningRequest &request,
    const QString &pythonExecutableOverride)
{
    if (isRunning()) {
        return false;
    }
    if (request.modelPath.trimmed().isEmpty() || !QFileInfo(request.modelPath).isFile()) {
        emit failed(QStringLiteral("图传/噪声模型文件不存在。"));
        return false;
    }
    if (request.inputRoot.trimmed().isEmpty() || !QFileInfo(request.inputRoot).exists()) {
        emit failed(QStringLiteral("信号清洗输入目录或清单不存在。"));
        return false;
    }
    if (request.outputDirectory.trimmed().isEmpty()) {
        emit failed(QStringLiteral("信号清洗输出目录不能为空。"));
        return false;
    }
    if (!QDir().mkpath(request.outputDirectory)) {
        emit failed(QStringLiteral("无法创建信号清洗输出目录：%1").arg(request.outputDirectory));
        return false;
    }
    const QString backend = request.backendScriptPath.trimmed().isEmpty()
        ? backendScriptPath()
        : request.backendScriptPath;
    if (!QFileInfo(backend).isFile()) {
        emit failed(QStringLiteral("图传/噪声推理脚本不存在：%1").arg(backend));
        return false;
    }

    m_request = request;
    m_request.inputRoot = normalizedInputRoot(request.inputRoot);
    m_request.backendScriptPath = QFileInfo(backend).absoluteFilePath();
    if (m_request.runId.trimmed().isEmpty()) {
        m_request.runId = QStringLiteral("clean_%1")
                              .arg(QDateTime::currentDateTimeUtc().toString(
                                  QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    }
    m_result = SignalCleaningResult();
    m_result.runId = m_request.runId;
    m_result.modelPath = m_request.modelPath;
    m_result.inputRoot = m_request.inputRoot;
    m_result.outputDirectory = m_request.outputDirectory;
    m_result.outputCsv = QDir(m_request.outputDirectory).filePath(QStringLiteral("predictions.csv"));
    m_result.summaryPath = QDir(m_request.outputDirectory).filePath(QStringLiteral("predictions.json"));
    m_result.threshold = m_request.threshold;
    m_stdoutBuffer.clear();
    m_logs.clear();
    m_lastError.clear();
    m_startedEventSeen = false;
    m_resultEmitted = false;
    m_cancelRequested = false;

    QSaveFile configFile(QDir(m_request.outputDirectory).filePath(QStringLiteral("run_config.json")));
    if (configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QJsonObject config;
        config.insert(QStringLiteral("run_id"), m_request.runId);
        config.insert(QStringLiteral("model_path"), m_request.modelPath);
        config.insert(QStringLiteral("backend_script"), m_request.backendScriptPath);
        config.insert(QStringLiteral("input_root"), m_request.inputRoot);
        config.insert(QStringLiteral("output_csv"), m_result.outputCsv);
        config.insert(QStringLiteral("threshold"), m_request.threshold);
        config.insert(QStringLiteral("batch_size"), m_request.batchSize);
        config.insert(QStringLiteral("cache_files"), m_request.cacheFiles);
        config.insert(QStringLiteral("num_workers"), m_request.numWorkers);
        config.insert(QStringLiteral("use_cpu"), m_request.useCpu);
        configFile.write(QJsonDocument(config).toJson(QJsonDocument::Indented));
        configFile.commit();
    }

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(QFileInfo(m_request.backendScriptPath).absolutePath());
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &SignalCleaningProcessRunner::readProcessOutput);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &SignalCleaningProcessRunner::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &SignalCleaningProcessRunner::handleProcessError);
    m_process->setProgram(pythonExecutableOverride.isEmpty()
                              ? pythonExecutablePath()
                              : pythonExecutableOverride);
    QStringList arguments = {
        m_request.backendScriptPath,
        QStringLiteral("predict"),
        QStringLiteral("--model"),
        m_request.modelPath,
        QStringLiteral("--input-root"),
        m_request.inputRoot,
        QStringLiteral("--output-csv"),
        m_result.outputCsv,
        QStringLiteral("--threshold"),
        QString::number(m_request.threshold, 'f', 6),
        QStringLiteral("--batch-size"),
        QString::number(m_request.batchSize),
        QStringLiteral("--cache-files"),
        QString::number(m_request.cacheFiles),
        QStringLiteral("--num-workers"),
        QString::number(m_request.numWorkers),
    };
    if (m_request.useCpu) {
        arguments.append(QStringLiteral("--cpu"));
    }
    m_process->setArguments(arguments);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    m_process->setProcessEnvironment(environment);
    m_process->start();
    if (!m_process->waitForStarted(3000)) {
        m_lastError = QStringLiteral("无法启动信号清洗 Python 进程：%1")
                          .arg(m_process->errorString());
        finishResult(-1, QProcess::CrashExit);
        return false;
    }

    emit started();
    emit progressChanged(5, QStringLiteral("推理进程已启动，正在读取候选窗口。"));
    return true;
}

void SignalCleaningProcessRunner::cancel()
{
    if (!m_process || m_process->state() == QProcess::NotRunning || m_cancelRequested) {
        return;
    }
    m_cancelRequested = true;
    m_process->kill();
    emit progressChanged(0, QStringLiteral("已停止清洗进程，已保留已经写入的结果。"));
}

bool SignalCleaningProcessRunner::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

QString SignalCleaningProcessRunner::pythonExecutablePath()
{
    return defaultPythonPath();
}

QString SignalCleaningProcessRunner::backendScriptPath()
{
    const QByteArray configured = qgetenv("AIRCRAFT_SIGNAL_NOISE_BACKEND");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QFileInfo(QString::fromLocal8Bit(configured)).absoluteFilePath();
    }
    const QStringList candidates = {
        QDir(ProjectPaths::cppAppRoot()).filePath(QStringLiteral("tools/signal_noise_binary.pyw")),
        QDir(ProjectPaths::signalNoiseModelDirectory()).filePath(QStringLiteral("signal_noise_binary.pyw")),
        QStringLiteral("D:/下载/signal_noise_binary.pyw"),
        QDir(QDir::homePath()).filePath(QStringLiteral("Downloads/signal_noise_binary.pyw")),
    };
    return firstExistingFile(candidates);
}

QString SignalCleaningProcessRunner::modelPath()
{
    const QByteArray configured = qgetenv("AIRCRAFT_SIGNAL_NOISE_MODEL");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QFileInfo(QString::fromLocal8Bit(configured)).absoluteFilePath();
    }
    const QStringList candidates = {
        QDir(ProjectPaths::signalNoiseModelDirectory()).filePath(QStringLiteral("best_model.pt")),
        QStringLiteral("D:/下载/best_model.pt"),
        QDir(QDir::homePath()).filePath(QStringLiteral("Downloads/best_model.pt")),
    };
    return firstExistingFile(candidates);
}

void SignalCleaningProcessRunner::readProcessOutput()
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

void SignalCleaningProcessRunner::consumeLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        appendLog(QString::fromUtf8(line));
        return;
    }
    const QJsonObject json = document.object();
    if (json.contains(QStringLiteral("records"))) {
        emit progressChanged(95, QStringLiteral("预测完成，正在整理窗口结果。"));
        return;
    }
    const QString type = json.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("started")) {
        m_startedEventSeen = true;
        emit progressChanged(10, QStringLiteral("模型已加载，开始单窗口推理。"));
    } else if (type == QStringLiteral("failed")) {
        m_lastError = json.value(QStringLiteral("error")).toString(
            QStringLiteral("信号清洗后端返回失败。"));
    } else {
        appendLog(QString::fromUtf8(line));
    }
}

bool SignalCleaningProcessRunner::loadResultFiles(QString *error)
{
    QFile summaryFile(m_result.summaryPath);
    if (!summaryFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("预测结果摘要不存在或无法读取：%1").arg(m_result.summaryPath);
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(summaryFile.readAll(), &parseError);
    if (!document.isObject()) {
        if (error) {
            *error = QStringLiteral("预测结果摘要格式无效：%1").arg(m_result.summaryPath);
        }
        return false;
    }
    const QJsonObject summary = document.object();
    const QJsonValue recordsValue = summary.value(QStringLiteral("records"));
    if (!recordsValue.isDouble()) {
        if (error) {
            *error = QStringLiteral("预测结果摘要缺少有效 records 字段。");
        }
        return false;
    }
    const double summaryRecords = recordsValue.toDouble();
    if (!std::isfinite(summaryRecords) || summaryRecords < 1.0
        || std::floor(summaryRecords) != summaryRecords) {
        if (error) {
            *error = QStringLiteral("预测结果摘要 records 必须是正整数。");
        }
        return false;
    }
    const QJsonValue thresholdValue = summary.value(QStringLiteral("threshold"));
    if (!thresholdValue.isDouble() || !std::isfinite(thresholdValue.toDouble())
        || thresholdValue.toDouble() < 0.0 || thresholdValue.toDouble() > 1.0) {
        if (error) {
            *error = QStringLiteral("预测结果摘要 threshold 必须在 0 到 1 之间。");
        }
        return false;
    }
    m_result.threshold = thresholdValue.toDouble();
    m_result.totalRecords = static_cast<int>(summaryRecords);
    m_result.device = summary.value(QStringLiteral("device")).toString();

    QFile csvFile(m_result.outputCsv);
    if (!csvFile.exists() || !csvFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("预测结果 CSV 不存在或无法读取：%1").arg(m_result.outputCsv);
        }
        return false;
    }

    QTextStream stream(&csvFile);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
    const QString header = stream.readLine();
    const QStringList columns = parseCsvLine(header);
    const QStringList requiredColumns = {
        QStringLiteral("path"),
        QStringLiteral("index"),
        QStringLiteral("sample_id"),
        QStringLiteral("group"),
        QStringLiteral("prob_signal"),
        QStringLiteral("prediction"),
    };
    if (columns.size() != requiredColumns.size()) {
        if (error) {
            *error = QStringLiteral("预测结果 CSV 表头无效：%1").arg(m_result.outputCsv);
        }
        return false;
    }
    QHash<QString, int> columnIndex;
    for (int index = 0; index < columns.size(); ++index) {
        if (!requiredColumns.contains(columns.at(index)) || columnIndex.contains(columns.at(index))) {
            if (error) {
                *error = QStringLiteral("预测结果 CSV 表头无效：%1").arg(m_result.outputCsv);
            }
            return false;
        }
        columnIndex.insert(columns.at(index), index);
    }
    for (const QString &column : requiredColumns) {
        if (!columnIndex.contains(column)) {
            if (error) {
                *error = QStringLiteral("预测结果 CSV 缺少列 %1。").arg(column);
            }
            return false;
        }
    }
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QStringList fields = parseCsvLine(line);
        if (fields.size() != requiredColumns.size()) {
            if (error) {
                *error = QStringLiteral("预测结果 CSV 行字段数无效。");
            }
            return false;
        }
        SignalCleaningPrediction prediction;
        prediction.path = csvField(fields, columnIndex.value(QStringLiteral("path"), -1));
        bool indexOk = false;
        prediction.index = csvField(fields, columnIndex.value(QStringLiteral("index"), -1)).toInt(&indexOk);
        prediction.sampleId = csvField(fields, columnIndex.value(QStringLiteral("sample_id"), -1));
        prediction.group = csvField(fields, columnIndex.value(QStringLiteral("group"), -1));
        bool probabilityOk = false;
        prediction.probability = csvField(fields, columnIndex.value(QStringLiteral("prob_signal"), -1))
                                     .toDouble(&probabilityOk);
        prediction.label = csvField(fields, columnIndex.value(QStringLiteral("prediction"), -1));
        if (prediction.path.trimmed().isEmpty() || !indexOk || prediction.index < 0
            || prediction.sampleId.trimmed().isEmpty() || prediction.group.trimmed().isEmpty()
            || !probabilityOk || !std::isfinite(prediction.probability)
            || prediction.probability < 0.0 || prediction.probability > 1.0
            || (prediction.label != QStringLiteral("signal") && prediction.label != QStringLiteral("noise"))) {
            if (error) {
                *error = QStringLiteral("预测结果 CSV 包含无效窗口记录。");
            }
            return false;
        }
        m_result.predictions.append(prediction);
        if (prediction.label == QStringLiteral("signal")) {
            ++m_result.signalRecords;
        } else {
            ++m_result.noiseRecords;
        }
    }
    if (m_result.predictions.isEmpty()) {
        if (error) {
            *error = QStringLiteral("预测结果 CSV 不包含任何窗口记录。");
        }
        return false;
    }
    if (m_result.totalRecords != m_result.predictions.size()) {
        if (error) {
            *error = QStringLiteral("预测结果摘要与 CSV 窗口数量不一致。");
        }
        return false;
    }
    return true;
}

void SignalCleaningProcessRunner::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readProcessOutput();
    finishResult(exitCode, exitStatus);
}

void SignalCleaningProcessRunner::handleProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        m_lastError = m_process ? m_process->errorString() : QStringLiteral("信号清洗进程启动失败");
    }
}

void SignalCleaningProcessRunner::finishResult(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_resultEmitted) {
        return;
    }
    m_resultEmitted = true;
    m_result.cancelled = m_cancelRequested;
    QString resultError;
    const bool filesLoaded = loadResultFiles(&resultError);
    m_result.success = !m_result.cancelled
        && exitCode == 0
        && exitStatus == QProcess::NormalExit
        && filesLoaded;
    m_result.message = m_result.cancelled
        ? QStringLiteral("清洗已停止，已保留已经写入的结果。")
        : (m_result.success
               ? QStringLiteral("单窗口清洗完成：共 %1 个窗口，图传 %2 个，纯噪声 %3 个。")
                     .arg(m_result.totalRecords)
                     .arg(m_result.signalRecords)
                     .arg(m_result.noiseRecords)
               : (m_lastError.isEmpty()
                      ? (resultError.isEmpty()
                             ? QStringLiteral("信号清洗进程失败，退出码 %1。").arg(exitCode)
                             : resultError)
                      : m_lastError));
    m_result.logs = m_logs;
    emit progressChanged(m_result.success ? 100 : 0, m_result.message);
    if (m_result.cancelled) {
        emit cancelled();
    } else if (!m_result.success) {
        emit failed(m_result.message);
    }
    emit finished(m_result);
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void SignalCleaningProcessRunner::appendLog(const QString &message)
{
    const QString text = message.trimmed();
    if (!text.isEmpty()) {
        m_logs.append(text);
    }
}
