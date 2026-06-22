#include "services/PreprocessProcessRunner.h"

#include "app/ProjectPaths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QProcess>

QJsonObject PreprocessSegment::toJson() const
{
    QJsonObject json;
    json.insert("segment_id", segmentId);
    json.insert("start_sample", startSample);
    json.insert("end_sample", endSample);
    json.insert("duration_ms", durationMs);
    json.insert("center_freq_hz", centerFreqHz);
    json.insert("bandwidth_hz", bandwidthHz);
    json.insert("snr_db", snrDb);
    json.insert("score", score);
    json.insert("output_file_path", outputFilePath);
    json.insert("status", status);
    return json;
}

PreprocessSegment PreprocessSegment::fromJson(const QJsonObject &json)
{
    PreprocessSegment segment;
    segment.segmentId = json.value("segment_id").toString();
    segment.startSample = json.value("start_sample").toInt();
    segment.endSample = json.value("end_sample").toInt();
    segment.durationMs = json.value("duration_ms").toDouble();
    segment.centerFreqHz = json.value("center_freq_hz").toDouble();
    segment.bandwidthHz = json.value("bandwidth_hz").toDouble();
    segment.snrDb = json.value("snr_db").toDouble();
    segment.score = json.value("score").toDouble();
    segment.outputFilePath = json.value("output_file_path").toString();
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
    json.insert("slice_length", request.sliceLength);
    json.insert("energy_threshold_db", request.energyThresholdDb);
    json.insert("noise_floor_dbm", request.noiseFloorDbm);
    json.insert("min_bandwidth_mhz", request.minBandwidthMhz);
    json.insert("min_duration_ms", request.minDurationMs);
    json.insert("enable_bandpass", request.enableBandpass);
    json.insert("sample_output_dir", request.sampleOutputDir);
    json.insert("model_weights_path", request.modelWeightsPath);
    json.insert("ai_confidence_threshold", request.aiConfidenceThreshold);
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
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        if (error) {
            *error = QStringLiteral("响应 JSON 无效");
        }
        return std::nullopt;
    }

    const QJsonObject json = doc.object();
    const QJsonArray segmentsJson = json.value("segments").toArray();
    QVector<PreprocessSegment> segments;
    for (const QJsonValue &value : segmentsJson) {
        segments.append(PreprocessSegment::fromJson(value.toObject()));
    }

    QStringList logs;
    const QJsonArray logsJson = json.value("logs").toArray();
    for (const QJsonValue &value : logsJson) {
        logs.append(value.toString());
    }

    PreprocessRunResult result;
    result.success = json.value("success").toBool();
    result.message = json.value("message").toString();
    result.detectedSegmentCount = json.value("detected_segment_count").toInt();
    result.candidateSegmentCount = json.value("candidate_segment_count").toInt();
    result.outputSampleCount = json.value("output_sample_count").toInt();
    result.sampleOutputDir = json.value("sample_output_dir").toString();
    result.segments = segments;
    result.logs = logs;
    return result;
}

QVector<SampleRecord> PreprocessProcessRunner::buildSampleRecords(const QString &inputCapPath, const CapProbeResult &capInfo, const PreprocessRunResult &result)
{
    QVector<SampleRecord> records;
    if (!result.success) {
        return records;
    }

    const QString inputStem = QFileInfo(inputCapPath).completeBaseName();
    for (int index = 0; index < result.segments.size(); ++index) {
        const PreprocessSegment &segment = result.segments.at(index);
        if (!segment.isValid()) {
            continue;
        }
        if (!QFileInfo::exists(segment.outputFilePath)) {
            continue;
        }
        SampleRecord record;
        record.sampleId = QStringLiteral("pp_%1_%2").arg(inputStem, segment.segmentId.isEmpty() ? QString::number(index + 1) : segment.segmentId);
        record.sourceType = QStringLiteral("local_preprocess");
        record.rawFilePath = inputCapPath;
        record.sampleFilePath = segment.outputFilePath;
        record.sampleRateHz = capInfo.sampleRateHz;
        record.centerFrequencyHz = segment.centerFreqHz > 0.0 ? segment.centerFreqHz : capInfo.centerFrequencyHz;
        record.dataFormat = QStringLiteral("complex64_npy");
        record.sampleCount = 0;
        record.deviceId = inputStem;
        record.startSample = segment.startSample;
        record.endSample = segment.endSample;
        record.status = QStringLiteral("待复核");
        record.sourceName = QStringLiteral("预处理输出");
        records.append(record);
    }
    return records;
}

PreprocessRunResult PreprocessProcessRunner::runDemo(const PreprocessRequest &request, const CapProbeResult &capInfo)
{
    QDir outputDir(request.sampleOutputDir);
    outputDir.mkpath(QStringLiteral("."));

    PreprocessRunResult result;
    result.success = true;
    result.message = QStringLiteral("演示模式已生成 4 个样本");
    result.detectedSegmentCount = 5;
    result.candidateSegmentCount = 4;
    result.outputSampleCount = 4;
    result.sampleOutputDir = outputDir.absolutePath();
    result.logs = {
        QStringLiteral("演示模式：跳过 Python runner 和模型权重检查。"),
        QStringLiteral("演示模式：基于 CAP 头信息生成候选片段。"),
        QStringLiteral("演示模式：样本已写入本地输出目录。"),
    };

    for (int index = 0; index < 4; ++index) {
        const QString segmentId = QStringLiteral("DEMO_%1").arg(index + 1, 3, 10, QLatin1Char('0'));
        const QString fileName = QStringLiteral("demo_segment_%1.npy").arg(index + 1, 3, 10, QLatin1Char('0'));
        const QString outputPath = outputDir.filePath(fileName);

        QFile sampleFile(outputPath);
        if (sampleFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sampleFile.write("demo-npy-placeholder");
            sampleFile.close();
        }

        PreprocessSegment segment;
        segment.segmentId = segmentId;
        segment.startSample = 4096 * index;
        segment.endSample = segment.startSample + request.sliceLength;
        segment.durationMs = request.sliceLength / qMax(capInfo.sampleRateHz, 1.0) * 1000.0;
        segment.centerFreqHz = capInfo.centerFrequencyHz;
        segment.bandwidthHz = capInfo.bandwidthHz;
        segment.snrDb = 18.0 + index;
        segment.score = 0.93 - index * 0.02;
        segment.outputFilePath = outputPath;
        segment.status = QStringLiteral("valid");
        result.segments.append(segment);
    }

    result.sampleRecords = buildSampleRecords(request.inputFilePath, capInfo, result);
    for (SampleRecord &record : result.sampleRecords) {
        record.sourceType = QStringLiteral("demo_preprocess");
        record.sourceName = QStringLiteral("演示模式输出");
        record.status = QStringLiteral("演示样本");
    }
    return result;
}

std::optional<PreprocessRunResult> PreprocessProcessRunner::run(
    const PreprocessRequest &request,
    const CapProbeResult &capInfo,
    QString *error,
    const QString &pythonExecutableOverride,
    const QString &runnerScriptOverride)
{
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
            *error = QStringLiteral("无法启动 Python 预处理进程");
        }
        return std::nullopt;
    }
    if (!process.waitForFinished(request.timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (error) {
            *error = QStringLiteral("Python 预处理进程超时");
        }
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("Python 预处理进程失败：%1").arg(QString::fromUtf8(process.readAll()));
        }
        return std::nullopt;
    }

    auto parsed = readResponseFile(responsePath, error);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    parsed->sampleRecords = buildSampleRecords(request.inputFilePath, capInfo, *parsed);
    return parsed;
}

QString PreprocessProcessRunner::pythonExecutable()
{
    return QStringLiteral("python");
}

QString PreprocessProcessRunner::runnerScriptPath()
{
    return QDir(ProjectPaths::workspaceRoot()).filePath("cpp_app/tools/preprocess_runner.py");
}
