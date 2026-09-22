#include "services/PreprocessProcessRunner.h"

#include "app/ProjectPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QDateTime>
#include <QVariant>

namespace
{
SampleRecord sampleRecordFromJson(const QJsonObject &json, const QString &inputPath, const PreprocessRunResult &result)
{
    SampleRecord record;
    const QFileInfo inputInfo(inputPath);
    record.sampleId = json.value("sample_id").toString();
    record.batchId = json.value("batch_id").toString();
    record.candidateId = json.value("candidate_id").toString();
    record.sourceType = QStringLiteral("iqraw_cv_preprocess");
    record.rawFilePath = json.value("source_file").toString(inputPath);
    record.sampleFilePath = json.value("sample_file_path").toString();
    record.spectrogramFilePath = json.value("spectrogram_path").toString();
    record.candidatePreviewFilePath = json.value("candidate_preview_path").toString();
    record.labelType = json.value("label").toString();
    record.sampleRateHz = json.value("sample_rate_hz").toDouble(result.sampleRateHz);
    record.centerFrequencyHz = json.value("center_frequency_hz").toDouble(result.centerFrequencyHz);
    record.bandwidthHz = json.value("bandwidth_hz").toDouble(result.bandwidthHz);
    record.snrDb = json.value("snr_db").toDouble();
    record.score = json.value("score").toDouble();
    record.occupancy = json.value("occupancy").toDouble();
    record.dataFormat = json.value("data_format").toString(QStringLiteral("npz_iq_float32"));
    record.sampleCount = json.value("valid_samples").toInt();
    if (record.sampleCount <= 0) {
        record.sampleCount = json.value("window_samples").toInt();
    }
    record.sampleIndex = json.value("sample_index").toInt();
    record.windowIndex = json.value("window_index").toInt();
    record.deviceId = inputInfo.completeBaseName();
    record.startSample = json.value("source_start_sample").toInt();
    record.endSample = json.value("source_end_sample").toInt();
    record.status = json.value("status").toString(QStringLiteral("待复核"));
    record.processingState = json.value("processing_state").toString();
    record.qualityStatus = json.value("quality_status").toString(QStringLiteral("待复核"));
    record.pipelineVersion = json.value("pipeline_version").toString();
    record.sourceName = QStringLiteral("CV 预处理输出");
    return record;
}
} // namespace

PreprocessProcessRunner::PreprocessProcessRunner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<PreprocessProgress>("PreprocessProgress");
    qRegisterMetaType<PreprocessRunResult>("PreprocessRunResult");
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(350);
    connect(m_progressTimer, &QTimer::timeout, this, &PreprocessProcessRunner::readProgressFile);
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        if (!isRunning()) {
            return;
        }
        m_timedOut = true;
        emit progressChanged(PreprocessProgress{
            m_request.taskId,
            QStringLiteral("整理结果"),
            0,
            0,
            QFileInfo(m_request.inputFilePath).size(),
            0,
            0,
            0,
            -1,
            QStringLiteral("任务超过预设时限，正在保留已生成结果")
        });
        cancel();
        QTimer::singleShot(5000, this, [this]() {
            if (isRunning()) {
                m_process->kill();
            }
        });
    });
}

PreprocessProcessRunner::~PreprocessProcessRunner()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

QString preprocessTaskStateToString(PreprocessTaskState state)
{
    switch (state) {
    case PreprocessTaskState::Preparing: return QStringLiteral("准备中");
    case PreprocessTaskState::Reading: return QStringLiteral("读取中");
    case PreprocessTaskState::Detecting: return QStringLiteral("检测中");
    case PreprocessTaskState::Exporting: return QStringLiteral("导出中");
    case PreprocessTaskState::Finalizing: return QStringLiteral("整理结果");
    case PreprocessTaskState::Completed: return QStringLiteral("已完成");
    case PreprocessTaskState::CompletedWithWarnings: return QStringLiteral("完成但有提示");
    case PreprocessTaskState::Cancelled: return QStringLiteral("已取消");
    case PreprocessTaskState::TimedOut: return QStringLiteral("已超时");
    case PreprocessTaskState::Failed: return QStringLiteral("失败");
    case PreprocessTaskState::Idle:
    default: return QStringLiteral("待执行");
    }
}

PreprocessTaskState preprocessTaskStateFromString(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("preparing") || normalized == QStringLiteral("准备中")) return PreprocessTaskState::Preparing;
    if (normalized == QStringLiteral("reading") || normalized == QStringLiteral("读取中")) return PreprocessTaskState::Reading;
    if (normalized == QStringLiteral("detecting") || normalized == QStringLiteral("检测中")) return PreprocessTaskState::Detecting;
    if (normalized == QStringLiteral("exporting") || normalized == QStringLiteral("导出中")) return PreprocessTaskState::Exporting;
    if (normalized == QStringLiteral("finalizing") || normalized == QStringLiteral("整理结果")) return PreprocessTaskState::Finalizing;
    if (normalized == QStringLiteral("completed") || normalized == QStringLiteral("已完成")) return PreprocessTaskState::Completed;
    if (normalized == QStringLiteral("completed_with_warnings") || normalized == QStringLiteral("完成但有提示")) return PreprocessTaskState::CompletedWithWarnings;
    if (normalized == QStringLiteral("cancelled") || normalized == QStringLiteral("已取消")) return PreprocessTaskState::Cancelled;
    if (normalized == QStringLiteral("timed_out") || normalized == QStringLiteral("已超时")) return PreprocessTaskState::TimedOut;
    if (normalized == QStringLiteral("failed") || normalized == QStringLiteral("失败")) return PreprocessTaskState::Failed;
    return PreprocessTaskState::Idle;
}

PreprocessProgress PreprocessProgress::fromJson(const QJsonObject &json)
{
    PreprocessProgress progress;
    progress.taskId = json.value("task_id").toString(json.value("taskId").toString());
    progress.phase = json.value("phase").toString();
    progress.percent = qBound(0, json.value("percent").toInt(), 100);
    progress.bytesRead = json.value("bytes_read").toVariant().toLongLong();
    progress.totalBytes = json.value("total_bytes").toVariant().toLongLong();
    progress.chunksProcessed = json.value("chunks_processed").toInt();
    progress.candidateCount = json.value("candidate_count").toInt();
    progress.sampleCount = json.value("sample_count").toInt();
    progress.etaMs = json.value("eta_ms").toVariant().toLongLong();
    progress.message = json.value("message").toString();
    return progress;
}

QJsonObject PreprocessSegment::toJson() const
{
    QJsonObject json;
    json.insert("segment_id", segmentId);
    json.insert("candidate_id", candidateId);
    json.insert("start_sample", startSample);
    json.insert("end_sample", endSample);
    json.insert("duration_ms", durationMs);
    json.insert("center_freq_hz", centerFreqHz);
    json.insert("bandwidth_hz", bandwidthHz);
    json.insert("snr_db", snrDb);
    json.insert("score", score);
    json.insert("occupancy", occupancy);
    json.insert("output_file_path", outputFilePath);
    json.insert("candidate_preview_path", candidatePreviewFilePath);
    json.insert("detection_mode_detail", detectionModeDetail);
    json.insert("status", status);
    return json;
}

PreprocessSegment PreprocessSegment::fromJson(const QJsonObject &json)
{
    PreprocessSegment segment;
    segment.segmentId = json.value("segment_id").toString();
    segment.candidateId = json.value("candidate_id").toString(segment.segmentId);
    segment.startSample = json.value("start_sample").toInt();
    segment.endSample = json.value("end_sample").toInt();
    segment.durationMs = json.value("duration_ms").toDouble();
    segment.centerFreqHz = json.value("center_freq_hz").toDouble();
    segment.bandwidthHz = json.value("bandwidth_hz").toDouble();
    segment.snrDb = json.value("snr_db").toDouble();
    segment.score = json.value("score").toDouble();
    segment.occupancy = json.value("occupancy").toDouble();
    segment.outputFilePath = json.value("output_file_path").toString();
    segment.candidatePreviewFilePath = json.value("candidate_preview_path").toString();
    segment.detectionModeDetail = json.value("detection_mode_detail").toString();
    segment.status = json.value("status").toString();
    return segment;
}

bool PreprocessSegment::isValid() const
{
    return status.compare(QStringLiteral("valid"), Qt::CaseInsensitive) == 0;
}

QJsonObject PreprocessProcessRunner::requestToJson(const PreprocessRequest &request)
{
    QJsonObject json;
    json.insert("input_file_path", request.inputFilePath);
    json.insert("task_id", request.taskId);
    json.insert("batch_id", request.batchId);
    json.insert("window_samples", request.windowSamples);
    json.insert("chunk_samples", request.chunkSamples);
    json.insert("payload_endian", request.payloadEndian);
    json.insert("detection_mode", request.detectionMode);
    json.insert("frequency_span_mhz", request.frequencySpanMhz);
    json.insert("energy_offset_db", request.energyOffsetDb);
    json.insert("target_bandwidth_mhz", request.targetBandwidthMhz);
    json.insert("min_bandwidth_mhz", request.minBandwidthMhz);
    json.insert("min_wideband_fill", request.minWidebandFill);
    json.insert("min_wideband_snr_db", request.minWidebandSnrDb);
    json.insert("morph_frequency_close_mhz", request.morphFrequencyCloseMhz);
    json.insert("shape_split_quantile", request.shapeSplitQuantile);
    json.insert("shape_split_merge_gap_ms", request.shapeSplitMergeGapMs);
    json.insert("max_segment_duration_ms", request.maxSegmentDurationMs);
    json.insert("band_window_ms", request.bandWindowMs);
    json.insert("band_window_overlap_ms", request.bandWindowOverlapMs);
    json.insert("min_band_score", request.minBandScore);
    json.insert("cv_column_quantile", request.cvColumnQuantile);
    json.insert("cv_min_window_occupancy", request.cvMinWindowOccupancy);
    json.insert("cv_morph_frequency_mhz", request.cvMorphFrequencyMhz);
    json.insert("cv_morph_time_ms", request.cvMorphTimeMs);
    json.insert("min_duration_ms", request.minDurationMs);
    json.insert("merge_gap_ms", request.mergeGapMs);
    json.insert("nperseg", request.nperseg);
    json.insert("noverlap", request.noverlap);
    json.insert("export_spectrogram_png", request.exportSpectrogramPng);
    json.insert("spectrogram_nperseg", request.spectrogramNperseg);
    json.insert("spectrogram_noverlap", request.spectrogramNoverlap);
    json.insert("spectrogram_image_size", request.spectrogramImageSize);
    json.insert("spectrogram_dynamic_range_db", request.spectrogramDynamicRangeDb);
    json.insert("spectrogram_frequency_span_mhz", request.spectrogramFrequencySpanMhz);
    json.insert("sample_output_dir", request.sampleOutputDir);
    json.insert("progress_path", request.progressPath);
    json.insert("cancel_path", request.cancelPath);
    json.insert("response_path", QDir(request.sampleOutputDir).filePath(QStringLiteral("result.json")));
    json.insert("label", request.label);
    json.insert("compute_backend", request.computeBackend);
    json.insert("gpu_device", request.gpuDevice);
    json.insert("allow_cpu_fallback", request.allowCpuFallback);
    if (request.maxChunks > 0) {
        json.insert("max_chunks", request.maxChunks);
    }
    json.insert("timeout_ms", request.timeoutMs);
    return json;
}

bool PreprocessProcessRunner::writeRequestFile(const QString &path, const PreprocessRequest &request, QString *error)
{
    QFile file(path);
    const QString parentDir = QFileInfo(file).absolutePath();
    if (!QDir().mkpath(parentDir)) {
        if (error) {
            *error = QStringLiteral("无法创建请求目录：%1").arg(parentDir);
        }
        return false;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QJsonDocument doc(requestToJson(request));
    if (file.write(doc.toJson(QJsonDocument::Indented)) < 0) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

bool PreprocessProcessRunner::start(
    const PreprocessRequest &request,
    const CapProbeResult &capInfo,
    const QString &pythonExecutableOverride,
    const QString &runnerScriptOverride)
{
    if (isRunning()) {
        return false;
    }

    const QFileInfo inputInfo(request.inputFilePath);
    if (!inputInfo.exists()) {
        emit failed(QStringLiteral("输入文件不存在：%1").arg(request.inputFilePath));
        return false;
    }
    if (request.sampleOutputDir.trimmed().isEmpty()) {
        emit failed(QStringLiteral("样本输出目录不能为空。"));
        return false;
    }
    if (!QDir().mkpath(request.sampleOutputDir)) {
        emit failed(QStringLiteral("无法创建样本输出目录：%1").arg(request.sampleOutputDir));
        return false;
    }

    m_request = request;
    m_capInfo = capInfo;
    if (m_request.taskId.isEmpty()) {
        m_request.taskId = QStringLiteral("pp_%1").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    }
    if (m_request.batchId.isEmpty()) {
        m_request.batchId = QFileInfo(m_request.sampleOutputDir).fileName();
    }
    const QString taskDir = m_request.sampleOutputDir;
    m_requestPath = QDir(taskDir).filePath(QStringLiteral("request.json"));
    m_responsePath = QDir(taskDir).filePath(QStringLiteral("result.json"));
    m_progressReadPath = m_request.progressPath.isEmpty()
        ? QDir(taskDir).filePath(QStringLiteral("progress.jsonl"))
        : m_request.progressPath;
    m_request.progressPath = m_progressReadPath;
    m_request.cancelPath = m_request.cancelPath.isEmpty()
        ? QDir(taskDir).filePath(QStringLiteral("cancel.flag"))
        : m_request.cancelPath;
    m_progressOffset = 0;
    m_stdoutBuffer.clear();
    m_cancelRequested = false;
    m_timedOut = false;

    QFile::remove(m_responsePath);
    QFile::remove(m_progressReadPath);
    QFile::remove(m_request.cancelPath);

    QString writeError;
    if (!writeRequestFile(m_requestPath, m_request, &writeError)) {
        emit failed(writeError);
        return false;
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &PreprocessProcessRunner::readProcessOutput);
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &PreprocessProcessRunner::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &PreprocessProcessRunner::handleProcessError);
    m_process->setProgram(pythonExecutableOverride.isEmpty() ? pythonExecutable() : pythonExecutableOverride);
    m_process->setArguments({
        runnerScriptOverride.isEmpty() ? runnerScriptPath() : runnerScriptOverride,
        m_requestPath,
        m_responsePath,
    });
    m_process->start();
    if (!m_process->waitForStarted(3000)) {
        const QString error = QStringLiteral("无法启动 Python IQRAW/CAP 预处理进程：%1").arg(m_process->errorString());
        m_process->deleteLater();
        m_process = nullptr;
        emit failed(error);
        return false;
    }

    m_progressTimer->start();
    if (m_timeoutTimer && request.timeoutMs > 0) {
        m_timeoutTimer->start(request.timeoutMs);
    }
    emit progressChanged(PreprocessProgress{
        m_request.taskId,
        QStringLiteral("准备中"),
        0,
        0,
        inputInfo.size(),
        0,
        0,
        0,
        -1,
        QStringLiteral("正在启动预处理进程")
    });
    return true;
}

void PreprocessProcessRunner::cancel()
{
    if (!isRunning() || m_cancelRequested) {
        return;
    }
    m_cancelRequested = true;
    QFile flag(m_request.cancelPath);
    if (flag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        flag.write("cancel\n");
        flag.close();
    }
    emit progressChanged(PreprocessProgress{
        m_request.taskId,
        QStringLiteral("整理结果"),
        0,
        0,
        QFileInfo(m_request.inputFilePath).size(),
        0,
        0,
        0,
        -1,
        QStringLiteral("正在请求取消，保留已生成结果")
    });
}

bool PreprocessProcessRunner::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

QString PreprocessProcessRunner::taskDirectory() const
{
    return m_request.sampleOutputDir;
}

void PreprocessProcessRunner::readProcessOutput()
{
    if (!m_process) {
        return;
    }
    m_stdoutBuffer += m_process->readAll();
    int newlineIndex = -1;
    while ((newlineIndex = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newlineIndex).trimmed();
        m_stdoutBuffer.remove(0, newlineIndex + 1);
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (document.isObject()) {
            const PreprocessProgress progress = PreprocessProgress::fromJson(document.object());
            if (!progress.taskId.isEmpty() || !progress.phase.isEmpty()) {
                emit progressChanged(progress);
            }
        }
    }
}

void PreprocessProcessRunner::readProgressFile()
{
    QFile file(m_progressReadPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    if (!file.seek(m_progressOffset)) {
        return;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (!document.isObject()) {
            continue;
        }
        const PreprocessProgress progress = PreprocessProgress::fromJson(document.object());
        m_progressOffset = file.pos();
        emit progressChanged(progress);
    }
    file.close();
    emitPartialResult();
}

void PreprocessProcessRunner::emitPartialResult()
{
    auto result = readPartialResult();
    if (!result.has_value()) {
        return;
    }
    emit partialResultAvailable(*result);
}

std::optional<PreprocessRunResult> PreprocessProcessRunner::readPartialResult() const
{
    const QString manifestPath = QDir(m_request.sampleOutputDir).filePath(QStringLiteral("manifest.jsonl"));
    QFile manifest(manifestPath);
    if (!manifest.exists() || !manifest.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QVector<QJsonObject> records;
    while (!manifest.atEnd()) {
        const QByteArray line = manifest.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (document.isObject()) {
            records.append(document.object());
        }
    }
    if (records.isEmpty()) {
        return std::nullopt;
    }

    PreprocessRunResult result;
    result.success = false;
    result.partial = true;
    result.taskId = m_request.taskId;
    result.taskState = PreprocessTaskState::Reading;
    result.batchDirectory = m_request.sampleOutputDir;
    result.sampleOutputDir = m_request.sampleOutputDir;
    result.manifestPath = manifestPath;
    result.progressPath = m_progressReadPath;
    result.taskManifestPath = manifestPath;
    result.sourceFormat = QFileInfo(m_request.inputFilePath).suffix().toLower();
    result.totalBytes = QFileInfo(m_request.inputFilePath).size();
    result.outputSampleCount = records.size();
    result.exportedWindows = records.size();

    QMap<int, PreprocessSegment> segments;
    for (const QJsonObject &json : records) {
        const SampleRecord sample = sampleRecordFromJson(json, m_request.inputFilePath, result);
        result.sampleRecords.append(sample);
        if (result.sampleRateHz <= 0.0) {
            result.sampleRateHz = sample.sampleRateHz;
            result.centerFrequencyHz = sample.centerFrequencyHz;
            result.bandwidthHz = sample.bandwidthHz;
        }
        const int index = json.value("segment_index").toInt();
        auto &segment = segments[index];
        if (segment.segmentId.isEmpty()) {
            segment.segmentId = QStringLiteral("SEG_%1").arg(index, 5, 10, QLatin1Char('0'));
            segment.candidateId = json.value("candidate_id").toString(segment.segmentId);
            segment.startSample = json.value("source_start_sample").toInt();
            segment.endSample = json.value("source_end_sample").toInt();
            segment.centerFreqHz = json.value("center_frequency_hz").toDouble();
            segment.bandwidthHz = json.value("bandwidth_hz").toDouble();
            segment.snrDb = json.value("snr_db").toDouble();
            segment.score = json.value("score").toDouble();
            segment.occupancy = json.value("occupancy").toDouble();
            segment.detectionModeDetail = json.value("detection_mode_detail").toString();
            segment.outputFilePath = json.value("sample_file_path").toString(json.value("path").toString());
            segment.status = QStringLiteral("valid");
        } else {
            segment.startSample = qMin(segment.startSample, json.value("source_start_sample").toInt());
            segment.endSample = qMax(segment.endSample, json.value("source_end_sample").toInt());
            segment.snrDb = qMax(segment.snrDb, json.value("snr_db").toDouble());
            segment.score = qMax(segment.score, json.value("score").toDouble());
        }
    }
    result.segments = segments.values().toVector();
    result.candidateSegmentCount = result.segments.size();
    result.detectedSegmentCount = result.segments.size();
    result.message = QStringLiteral("已恢复 %1 个已生成样本，任务仍在处理中。").arg(result.sampleRecords.size());
    return result;
}

void PreprocessProcessRunner::finishWithResult(const std::optional<PreprocessRunResult> &parsed, const QString &error)
{
    if (m_progressTimer) {
        m_progressTimer->stop();
    }
    if (m_timeoutTimer) {
        m_timeoutTimer->stop();
    }
    if (parsed.has_value()) {
        PreprocessRunResult result = *parsed;
        result.sampleRecords = buildSampleRecords(m_request.inputFilePath, m_capInfo, result);
        if (result.taskId.isEmpty()) {
            result.taskId = m_request.taskId;
        }
        if (m_timedOut && !result.cancelled) {
            result.timedOut = true;
            result.partial = true;
            result.taskState = PreprocessTaskState::TimedOut;
        }
        if (result.batchDirectory.isEmpty()) {
            result.batchDirectory = m_request.sampleOutputDir;
        }
        if (result.cancelled || result.taskState == PreprocessTaskState::Cancelled) {
            emit cancelled();
        }
        emit finished(result);
    } else {
        const auto partial = readPartialResult();
        if (m_timedOut || partial.has_value()) {
            PreprocessRunResult result = partial.value_or(PreprocessRunResult{});
            result.taskId = m_request.taskId;
            result.partial = true;
            result.batchDirectory = m_request.sampleOutputDir;
            result.taskState = m_timedOut ? PreprocessTaskState::TimedOut : PreprocessTaskState::Failed;
            result.timedOut = m_timedOut;
            result.message = m_timedOut
                ? QStringLiteral("预处理超时，已保留可恢复的部分结果。")
                : QStringLiteral("预处理进程异常退出，已保留可恢复的部分结果。");
            emit finished(result);
        } else {
            emit failed(error.isEmpty() ? QStringLiteral("预处理未生成有效结果。") : error);
        }
    }
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void PreprocessProcessRunner::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode)
    Q_UNUSED(exitStatus)
    readProcessOutput();
    readProgressFile();
    QString responseError;
    const auto result = readResponseFile(m_responsePath, &responseError);
    if (result.has_value()) {
        finishWithResult(result, responseError);
        return;
    }
    finishWithResult(std::nullopt, responseError);
}

void PreprocessProcessRunner::handleProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        finishWithResult(std::nullopt, m_process ? m_process->errorString() : QStringLiteral("进程启动失败"));
    }
}

std::optional<PreprocessRunResult> PreprocessProcessRunner::readResponseFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        if (error) {
            *error = QStringLiteral("响应文件不存在");
        }
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isObject()) {
        if (error) {
            *error = QStringLiteral("响应 JSON 无效：%1").arg(parseError.errorString());
        }
        return std::nullopt;
    }

    const QJsonObject json = doc.object();
    PreprocessRunResult result;
    result.success = json.value("success").toBool();
    result.message = json.value("message").toString();
    result.taskId = json.value("task_id").toString();
    result.taskState = preprocessTaskStateFromString(json.value("task_state").toString());
    result.partial = json.value("partial").toBool();
    result.cancelled = json.value("cancelled").toBool();
    result.timedOut = json.value("timed_out").toBool();
    result.bytesRead = json.value("bytes_read").toVariant().toLongLong();
    result.totalBytes = json.value("total_bytes").toVariant().toLongLong();
    result.averageSnrDb = json.value("average_snr_db").toDouble();
    result.averageBandwidthHz = json.value("average_bandwidth_hz").toDouble();
    result.qualitySummary = json.value("quality_summary").toString();
    result.batchDirectory = json.value("batch_directory").toString();
    result.progressPath = json.value("progress_path").toString();
    result.taskManifestPath = json.value("task_manifest_path").toString();
    result.computeBackendRequested = json.value("compute_backend_requested").toString();
    result.computeBackend = json.value("compute_backend").toString();
    result.gpuAvailable = json.value("gpu_available").toBool();
    result.gpuFallback = json.value("gpu_fallback").toBool();
    result.gpuDevice = json.value("gpu_device").toInt();
    result.gpuDeviceName = json.value("gpu_device_name").toString();
    result.sourceFormat = json.value("source_format").toString();
    result.sampleRateHz = json.value("sample_rate_hz").toDouble();
    result.centerFrequencyHz = json.value("center_frequency_hz").toDouble();
    result.bandwidthHz = json.value("bandwidth_hz").toDouble();
    result.detectedSegmentCount = json.value("detected_segment_count").toInt();
    result.candidateSegmentCount = json.value("candidate_segment_count").toInt();
    result.outputSampleCount = json.value("output_sample_count").toInt();
    result.sampleOutputDir = json.value("sample_output_dir").toString();
    result.manifestPath = json.value("manifest_path").toString();
    result.summaryPath = json.value("summary_path").toString();
    result.exportedSegments = json.value("exported_segments").toInt(result.candidateSegmentCount);
    result.exportedWindows = json.value("exported_windows").toInt(result.outputSampleCount);

    const QJsonArray segmentsJson = json.value("segments").toArray();
    for (const QJsonValue &value : segmentsJson) {
        result.segments.append(PreprocessSegment::fromJson(value.toObject()));
    }

    const QJsonArray samplesJson = json.value("samples").toArray();
    for (const QJsonValue &value : samplesJson) {
        result.sampleRecords.append(sampleRecordFromJson(value.toObject(), QString(), result));
    }

    const QJsonArray logsJson = json.value("logs").toArray();
    for (const QJsonValue &value : logsJson) {
        result.logs.append(value.toString());
    }
    return result;
}

QVector<SampleRecord> PreprocessProcessRunner::buildSampleRecords(
    const QString &inputCapPath,
    const CapProbeResult &capInfo,
    const PreprocessRunResult &result)
{
    if (!result.success && !result.partial) {
        return {};
    }

    if (!result.sampleRecords.isEmpty()) {
        QVector<SampleRecord> records = result.sampleRecords;
        const QFileInfo inputInfo(inputCapPath);
        for (SampleRecord &record : records) {
            if (record.batchId.isEmpty()) {
                record.batchId = result.batchDirectory.isEmpty() ? QString() : QFileInfo(result.batchDirectory).fileName();
            }
            if (record.rawFilePath.isEmpty()) {
                record.rawFilePath = inputCapPath;
            }
            if (record.sampleRateHz <= 0.0) {
                record.sampleRateHz = result.sampleRateHz > 0.0 ? result.sampleRateHz : capInfo.sampleRateHz;
            }
            if (record.centerFrequencyHz <= 0.0) {
                record.centerFrequencyHz = result.centerFrequencyHz > 0.0 ? result.centerFrequencyHz : capInfo.centerFrequencyHz;
            }
            if (record.bandwidthHz <= 0.0) {
                record.bandwidthHz = result.bandwidthHz > 0.0 ? result.bandwidthHz : capInfo.bandwidthHz;
            }
            if (record.deviceId.isEmpty()) {
                record.deviceId = inputInfo.completeBaseName();
            }
        }
        return records;
    }

    QVector<SampleRecord> records;
    const QFileInfo inputInfo(inputCapPath);
    for (int index = 0; index < result.segments.size(); ++index) {
        const PreprocessSegment &segment = result.segments.at(index);
        if (!segment.isValid() || segment.outputFilePath.isEmpty() || !QFileInfo::exists(segment.outputFilePath)) {
            continue;
        }
        SampleRecord record;
        record.sampleId = QStringLiteral("pp_%1_%2").arg(inputInfo.completeBaseName(),
                                                        segment.segmentId.isEmpty() ? QString::number(index + 1) : segment.segmentId);
        record.batchId = result.batchDirectory.isEmpty() ? QString() : QFileInfo(result.batchDirectory).fileName();
        record.candidateId = segment.candidateId;
        record.sourceType = QStringLiteral("iqraw_cv_preprocess");
        record.rawFilePath = inputCapPath;
        record.sampleFilePath = segment.outputFilePath;
        record.sampleRateHz = result.sampleRateHz > 0.0 ? result.sampleRateHz : capInfo.sampleRateHz;
        record.centerFrequencyHz = segment.centerFreqHz > 0.0 ? segment.centerFreqHz : capInfo.centerFrequencyHz;
        record.bandwidthHz = segment.bandwidthHz;
        record.snrDb = segment.snrDb;
        record.score = segment.score;
        record.occupancy = segment.occupancy;
        record.candidatePreviewFilePath = segment.candidatePreviewFilePath;
        record.dataFormat = QStringLiteral("npz_iq_float32");
        record.sampleCount = qMax(0, segment.endSample - segment.startSample);
        record.deviceId = inputInfo.completeBaseName();
        record.startSample = segment.startSample;
        record.endSample = segment.endSample;
        record.status = QStringLiteral("待复核");
        record.processingState = preprocessTaskStateToString(result.taskState);
        record.qualityStatus = QStringLiteral("待复核");
        record.sourceName = QStringLiteral("CV 预处理输出");
        records.append(record);
    }
    return records;
}

std::optional<PreprocessRunResult> PreprocessProcessRunner::run(
    const PreprocessRequest &request,
    const CapProbeResult &capInfo,
    QString *error,
    const QString &pythonExecutableOverride,
    const QString &runnerScriptOverride)
{
    const QFileInfo inputInfo(request.inputFilePath);
    if (!inputInfo.exists()) {
        if (error) {
            *error = QStringLiteral("输入文件不存在：%1").arg(request.inputFilePath);
        }
        return std::nullopt;
    }
    const QString suffix = inputInfo.suffix().toLower();
    if (suffix != QStringLiteral("cap") && suffix != QStringLiteral("iqraw")) {
        if (error) {
            *error = QStringLiteral("预处理输入必须是 .cap 或 .iqraw 文件。");
        }
        return std::nullopt;
    }
    if (request.sampleOutputDir.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("样本输出目录不能为空。");
        }
        return std::nullopt;
    }
    if (!QDir().mkpath(request.sampleOutputDir)) {
        if (error) {
            *error = QStringLiteral("无法创建样本输出目录：%1").arg(request.sampleOutputDir);
        }
        return std::nullopt;
    }

    const QString runtimeDir = QDir(ProjectPaths::stateDirectory()).filePath("preprocess_runtime");
    if (!QDir().mkpath(runtimeDir)) {
        if (error) {
            *error = QStringLiteral("无法创建预处理运行目录：%1").arg(runtimeDir);
        }
        return std::nullopt;
    }

    const QString requestPath = QDir(runtimeDir).filePath("request.json");
    const QString responsePath = QDir(runtimeDir).filePath("response.json");
    if (!writeRequestFile(requestPath, request, error)) {
        return std::nullopt;
    }

    QProcess process;
    process.setProgram(pythonExecutableOverride.isEmpty() ? pythonExecutable() : pythonExecutableOverride);
    process.setArguments({
        runnerScriptOverride.isEmpty() ? runnerScriptPath() : runnerScriptOverride,
        requestPath,
        responsePath,
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted()) {
        if (error) {
            *error = QStringLiteral("无法启动 Python IQRAW/CAP 预处理进程：%1").arg(process.errorString());
        }
        return std::nullopt;
    }
    if (!process.waitForFinished(request.timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (error) {
            *error = QStringLiteral("Python IQRAW/CAP 预处理进程超时");
        }
        return std::nullopt;
    }

    QString responseError;
    auto parsed = readResponseFile(responsePath, &responseError);
    if (parsed.has_value()) {
        parsed->sampleRecords = buildSampleRecords(request.inputFilePath, capInfo, *parsed);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (parsed.has_value() && !parsed->message.isEmpty()) {
            return parsed;
        }
        if (error) {
            *error = QStringLiteral("Python IQRAW/CAP 预处理进程失败：%1")
                         .arg(QString::fromUtf8(process.readAll()));
        }
        return std::nullopt;
    }
    if (!parsed.has_value()) {
        if (error) {
            *error = responseError;
        }
        return std::nullopt;
    }
    return parsed;
}

QString PreprocessProcessRunner::pythonExecutable()
{
    const QByteArray configured = qgetenv("AIRCRAFT_PREPROCESS_PYTHON");
    if (!configured.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(configured))) {
        return QString::fromLocal8Bit(configured);
    }
    const QStringList pythonCandidates = {
        QDir(ProjectPaths::aircraftProjectRoot())
            .filePath(QStringLiteral("python_envs/aircraft_preprocess_gpu/Scripts/python.exe")),
        QDir(ProjectPaths::aircraftProjectRoot())
            .filePath(QStringLiteral("../python_envs/aircraft_preprocess_gpu/Scripts/python.exe")),
        QDir(ProjectPaths::aircraftProjectRoot())
            .filePath(QStringLiteral("python_envs/aircraft_preprocess_gpu/bin/python")),
        QDir(ProjectPaths::aircraftProjectRoot())
            .filePath(QStringLiteral("../python_envs/aircraft_preprocess_gpu/bin/python")),
    };
    for (const QString &candidate : pythonCandidates) {
        const QString absoluteCandidate = QFileInfo(candidate).absoluteFilePath();
        if (QFileInfo::exists(absoluteCandidate)) {
            return absoluteCandidate;
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()
        ? QStringLiteral("python")
        : QStringLiteral("python3");
}

QString PreprocessProcessRunner::runnerScriptPath()
{
    return QDir(ProjectPaths::cppAppRoot()).filePath("tools/preprocess_runner.py");
}
