#include "core/Records.h"

#include <QJsonArray>

namespace
{
double jsonDouble(const QJsonObject &json, const QString &key, double fallback = 0.0)
{
    const QJsonValue value = json.value(key);
    if (value.isDouble()) {
        return value.toDouble();
    }
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

bool jsonBool(const QJsonObject &json, const QString &key, bool fallback = false)
{
    const QJsonValue value = json.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == QStringLiteral("true") || text == QStringLiteral("1")) {
            return true;
        }
        if (text == QStringLiteral("false") || text == QStringLiteral("0")) {
            return false;
        }
    }
    if (value.isDouble()) {
        return !qFuzzyIsNull(value.toDouble());
    }
    return fallback;
}

void insertString(QJsonObject *json, const QString &key, const QString &value)
{
    if (json && !value.isEmpty()) {
        json->insert(key, value);
    }
}

QJsonObject labelCountsToJson(const QMap<QString, int> &counts)
{
    QJsonObject json;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        json.insert(it.key(), it.value());
    }
    return json;
}

QMap<QString, int> labelCountsFromJson(const QJsonObject &json)
{
    QMap<QString, int> counts;
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) {
        counts.insert(it.key(), it.value().toInt());
    }
    return counts;
}
} // namespace

QJsonObject SampleRecord::toJson() const
{
    QJsonObject json;
    insertString(&json, QStringLiteral("sampleId"), sampleId);
    insertString(&json, QStringLiteral("batchId"), batchId);
    insertString(&json, QStringLiteral("candidateId"), candidateId);
    insertString(&json, QStringLiteral("sourceType"), sourceType);
    insertString(&json, QStringLiteral("rawFilePath"), rawFilePath);
    insertString(&json, QStringLiteral("sampleFilePath"), sampleFilePath);
    insertString(&json, QStringLiteral("spectrogramFilePath"), spectrogramFilePath);
    insertString(&json, QStringLiteral("candidatePreviewFilePath"), candidatePreviewFilePath);
    insertString(&json, QStringLiteral("labelType"), labelType);
    insertString(&json, QStringLiteral("labelIndividual"), labelIndividual);
    json.insert(QStringLiteral("sampleRateHz"), sampleRateHz);
    json.insert(QStringLiteral("centerFrequencyHz"), centerFrequencyHz);
    json.insert(QStringLiteral("bandwidthHz"), bandwidthHz);
    json.insert(QStringLiteral("snrDb"), snrDb);
    json.insert(QStringLiteral("score"), score);
    json.insert(QStringLiteral("occupancy"), occupancy);
    insertString(&json, QStringLiteral("dataFormat"), dataFormat);
    json.insert(QStringLiteral("sampleCount"), sampleCount);
    json.insert(QStringLiteral("sampleIndex"), sampleIndex);
    json.insert(QStringLiteral("windowIndex"), windowIndex);
    insertString(&json, QStringLiteral("deviceId"), deviceId);
    json.insert(QStringLiteral("startSample"), startSample);
    json.insert(QStringLiteral("endSample"), endSample);
    insertString(&json, QStringLiteral("status"), status);
    insertString(&json, QStringLiteral("processingState"), processingState);
    insertString(&json, QStringLiteral("qualityStatus"), qualityStatus);
    insertString(&json, QStringLiteral("pipelineVersion"), pipelineVersion);
    insertString(&json, QStringLiteral("sourceName"), sourceName);
    if (signalNoiseProbability >= 0.0) {
        json.insert(QStringLiteral("signalNoiseProbability"), signalNoiseProbability);
    }
    json.insert(QStringLiteral("signalNoiseThreshold"), signalNoiseThreshold);
    insertString(&json, QStringLiteral("signalNoiseLabel"), signalNoiseLabel);
    insertString(&json, QStringLiteral("signalNoiseModelPath"), signalNoiseModelPath);
    insertString(&json, QStringLiteral("signalNoiseRunId"), signalNoiseRunId);
    return json;
}

QJsonObject CandidateReviewRecord::toJson() const
{
    QJsonObject json;
    insertString(&json, QStringLiteral("reviewKey"), reviewKey);
    insertString(&json, QStringLiteral("sourceFilePath"), sourceFilePath);
    insertString(&json, QStringLiteral("batchId"), batchId);
    insertString(&json, QStringLiteral("candidateId"), candidateId);
    insertString(&json, QStringLiteral("candidatePreviewPath"), candidatePreviewPath);
    insertString(&json, QStringLiteral("representativeSampleId"), representativeSampleId);
    insertString(&json, QStringLiteral("labelType"), labelType);
    json.insert(QStringLiteral("windowCount"), windowCount);
    json.insert(QStringLiteral("validWindowCount"), validWindowCount);
    json.insert(QStringLiteral("averageScore"), averageScore);
    json.insert(QStringLiteral("minimumScore"), minimumScore);
    json.insert(QStringLiteral("maximumScore"), maximumScore);
    json.insert(QStringLiteral("averageSnrDb"), averageSnrDb);
    json.insert(QStringLiteral("averageOccupancy"), averageOccupancy);
    json.insert(QStringLiteral("ruleScore"), ruleScore);
    insertString(&json, QStringLiteral("recommendation"), recommendation);
    insertString(&json, QStringLiteral("reviewState"), reviewState);
    insertString(&json, QStringLiteral("reason"), reason);
    insertString(&json, QStringLiteral("algorithmVersion"), algorithmVersion);
    insertString(&json, QStringLiteral("reviewedAt"), reviewedAt);
    return json;
}

CandidateReviewRecord CandidateReviewRecord::fromJson(const QJsonObject &json)
{
    CandidateReviewRecord record;
    record.reviewKey = json.value(QStringLiteral("reviewKey")).toString();
    record.sourceFilePath = json.value(QStringLiteral("sourceFilePath")).toString();
    record.batchId = json.value(QStringLiteral("batchId")).toString();
    record.candidateId = json.value(QStringLiteral("candidateId")).toString();
    record.candidatePreviewPath = json.value(QStringLiteral("candidatePreviewPath")).toString();
    record.representativeSampleId = json.value(QStringLiteral("representativeSampleId")).toString();
    record.labelType = json.value(QStringLiteral("labelType")).toString();
    record.windowCount = json.value(QStringLiteral("windowCount")).toInt();
    record.validWindowCount = json.value(QStringLiteral("validWindowCount")).toInt();
    record.averageScore = jsonDouble(json, QStringLiteral("averageScore"));
    record.minimumScore = jsonDouble(json, QStringLiteral("minimumScore"));
    record.maximumScore = jsonDouble(json, QStringLiteral("maximumScore"));
    record.averageSnrDb = jsonDouble(json, QStringLiteral("averageSnrDb"));
    record.averageOccupancy = jsonDouble(json, QStringLiteral("averageOccupancy"));
    record.ruleScore = jsonDouble(json, QStringLiteral("ruleScore"));
    record.recommendation = json.value(QStringLiteral("recommendation")).toString(QStringLiteral("待人工复核"));
    record.reviewState = json.value(QStringLiteral("reviewState")).toString(QStringLiteral("待复核"));
    record.reason = json.value(QStringLiteral("reason")).toString();
    record.algorithmVersion = json.value(QStringLiteral("algorithmVersion")).toString();
    record.reviewedAt = json.value(QStringLiteral("reviewedAt")).toString();
    return record;
}

SampleRecord SampleRecord::fromJson(const QJsonObject &json)
{
    SampleRecord record;
    record.sampleId = json.value("sampleId").toString();
    record.batchId = json.value("batchId").toString();
    record.candidateId = json.value("candidateId").toString();
    record.sourceType = json.value("sourceType").toString();
    record.rawFilePath = json.value("rawFilePath").toString();
    record.sampleFilePath = json.value("sampleFilePath").toString();
    record.spectrogramFilePath = json.value("spectrogramFilePath").toString();
    record.candidatePreviewFilePath = json.value("candidatePreviewFilePath").toString();
    record.labelType = json.value("labelType").toString();
    record.labelIndividual = json.value("labelIndividual").toString();
    record.sampleRateHz = jsonDouble(json, "sampleRateHz");
    record.centerFrequencyHz = jsonDouble(json, "centerFrequencyHz");
    record.bandwidthHz = jsonDouble(json, "bandwidthHz");
    record.snrDb = jsonDouble(json, "snrDb");
    record.score = jsonDouble(json, "score");
    record.occupancy = jsonDouble(json, "occupancy");
    record.dataFormat = json.value("dataFormat").toString();
    record.sampleCount = json.value("sampleCount").toInt();
    record.sampleIndex = json.value("sampleIndex").toInt();
    record.windowIndex = json.value("windowIndex").toInt();
    record.deviceId = json.value("deviceId").toString();
    record.startSample = json.value("startSample").toInt();
    record.endSample = json.value("endSample").toInt();
    record.status = json.value("status").toString(QStringLiteral("已标注"));
    record.processingState = json.value("processingState").toString();
    record.qualityStatus = json.value("qualityStatus").toString();
    record.pipelineVersion = json.value("pipelineVersion").toString();
    record.sourceName = json.value("sourceName").toString();
    record.signalNoiseProbability = jsonDouble(json, QStringLiteral("signalNoiseProbability"), -1.0);
    record.signalNoiseThreshold = jsonDouble(json, QStringLiteral("signalNoiseThreshold"), 0.5);
    record.signalNoiseLabel = json.value(QStringLiteral("signalNoiseLabel")).toString();
    record.signalNoiseModelPath = json.value(QStringLiteral("signalNoiseModelPath")).toString();
    record.signalNoiseRunId = json.value(QStringLiteral("signalNoiseRunId")).toString();
    return record;
}

QJsonObject PreprocessBatchRecord::toJson() const
{
    QJsonObject json;
    insertString(&json, QStringLiteral("batchId"), batchId);
    insertString(&json, QStringLiteral("taskId"), taskId);
    insertString(&json, QStringLiteral("inputFilePath"), inputFilePath);
    insertString(&json, QStringLiteral("outputDirectory"), outputDirectory);
    insertString(&json, QStringLiteral("summaryPath"), summaryPath);
    insertString(&json, QStringLiteral("profileName"), profileName);
    insertString(&json, QStringLiteral("state"), state);
    insertString(&json, QStringLiteral("createdAt"), createdAt);
    json.insert(QStringLiteral("candidateCount"), candidateCount);
    json.insert(QStringLiteral("sampleCount"), sampleCount);
    json.insert(QStringLiteral("partial"), partial ? QStringLiteral("true") : QStringLiteral("false"));
    return json;
}

PreprocessBatchRecord PreprocessBatchRecord::fromJson(const QJsonObject &json)
{
    PreprocessBatchRecord record;
    record.batchId = json.value("batchId").toString();
    record.taskId = json.value("taskId").toString();
    record.inputFilePath = json.value("inputFilePath").toString();
    record.outputDirectory = json.value("outputDirectory").toString();
    record.summaryPath = json.value("summaryPath").toString();
    record.profileName = json.value("profileName").toString();
    record.state = json.value("state").toString();
    record.createdAt = json.value("createdAt").toString();
    record.candidateCount = json.value("candidateCount").toInt();
    record.sampleCount = json.value("sampleCount").toInt();
    record.partial = jsonBool(json, "partial");
    return record;
}

QJsonObject DatasetVersionRecord::toJson() const
{
    QJsonObject json;
    insertString(&json, QStringLiteral("versionId"), versionId);
    insertString(&json, QStringLiteral("taskType"), taskType);
    json.insert(QStringLiteral("sampleCount"), sampleCount);
    insertString(&json, QStringLiteral("strategy"), strategy);
    insertString(&json, QStringLiteral("createdAt"), createdAt);
    insertString(&json, QStringLiteral("sourceSummary"), sourceSummary);
    insertString(&json, QStringLiteral("outputDirectory"), outputDirectory);
    insertString(&json, QStringLiteral("manifestPath"), manifestPath);
    QJsonArray selectedLabelsJson;
    for (const QString &label : selectedLabels) {
        selectedLabelsJson.append(label);
    }
    json.insert(QStringLiteral("selectedLabels"), selectedLabelsJson);
    json.insert(QStringLiteral("includePartial"),
               includePartial ? QStringLiteral("true") : QStringLiteral("false"));
    json.insert(QStringLiteral("labelCounts"), labelCountsToJson(labelCounts));
    return json;
}

DatasetVersionRecord DatasetVersionRecord::fromJson(const QJsonObject &json)
{
    DatasetVersionRecord record;
    record.versionId = json.value("versionId").toString();
    record.taskType = json.value("taskType").toString();
    record.sampleCount = json.value("sampleCount").toInt();
    record.strategy = json.value("strategy").toString();
    record.createdAt = json.value("createdAt").toString();
    record.sourceSummary = json.value("sourceSummary").toString();
    record.outputDirectory = json.value("outputDirectory").toString();
    record.manifestPath = json.value("manifestPath").toString();
    const QJsonArray selectedLabelsJson = json.value("selectedLabels").toArray();
    for (const QJsonValue &value : selectedLabelsJson) {
        const QString label = value.toString().trimmed();
        if (!label.isEmpty()) {
            record.selectedLabels.append(label);
        }
    }
    record.includePartial = jsonBool(json, "includePartial");
    record.labelCounts = labelCountsFromJson(json.value("labelCounts").toObject());
    return record;
}

QJsonObject TrainingRunRecord::toJson() const
{
    QJsonObject json;
    insertString(&json, QStringLiteral("runId"), runId);
    insertString(&json, QStringLiteral("datasetVersionId"), datasetVersionId);
    insertString(&json, QStringLiteral("datasetVersionDirectory"), datasetVersionDirectory);
    insertString(&json, QStringLiteral("outputDirectory"), outputDirectory);
    insertString(&json, QStringLiteral("state"), state);
    insertString(&json, QStringLiteral("createdAt"), createdAt);
    insertString(&json, QStringLiteral("completedAt"), completedAt);
    insertString(&json, QStringLiteral("summaryPath"), summaryPath);
    insertString(&json, QStringLiteral("bestModelPath"), bestModelPath);
    insertString(&json, QStringLiteral("finalModelPath"), finalModelPath);
    insertString(&json, QStringLiteral("message"), message);
    json.insert(QStringLiteral("epoch"), epoch);
    json.insert(QStringLiteral("epochs"), epochs);
    json.insert(QStringLiteral("trainWindows"), trainWindows);
    json.insert(QStringLiteral("valWindows"), valWindows);
    json.insert(QStringLiteral("bestValAccuracy"), bestValAccuracy);
    json.insert(QStringLiteral("finalValAccuracy"), finalValAccuracy);
    json.insert(QStringLiteral("cancelled"),
               cancelled ? QStringLiteral("true") : QStringLiteral("false"));
    json.insert(QStringLiteral("partial"),
               partial ? QStringLiteral("true") : QStringLiteral("false"));
    json.insert(QStringLiteral("timedOut"),
               timedOut ? QStringLiteral("true") : QStringLiteral("false"));
    json.insert(QStringLiteral("requireCuda"),
               requireCuda ? QStringLiteral("true") : QStringLiteral("false"));
    insertString(&json, QStringLiteral("gpuDeviceName"), gpuDeviceName);
    insertString(&json, QStringLiteral("device"), device);
    insertString(&json, QStringLiteral("pipelineVersion"), pipelineVersion);
    insertString(&json, QStringLiteral("featureMode"), featureMode);
    insertString(&json, QStringLiteral("modelSize"), modelSize);
    insertString(&json, QStringLiteral("cacheDirectory"), cacheDirectory);
    insertString(&json, QStringLiteral("cacheSummaryPath"), cacheSummaryPath);
    json.insert(QStringLiteral("sampleLength"), sampleLength);
    insertString(&json, QStringLiteral("modelMode"), modelMode);
    json.insert(QStringLiteral("modelAvailable"),
               modelAvailable ? QStringLiteral("true") : QStringLiteral("false"));
    insertString(&json, QStringLiteral("splitSummaryPath"), splitSummaryPath);
    insertString(&json, QStringLiteral("modelVerificationStatus"), modelVerificationStatus);
    insertString(&json, QStringLiteral("modelVerificationMessage"), modelVerificationMessage);
    QJsonArray modelClassNamesJson;
    for (const QString &className : modelClassNames) {
        modelClassNamesJson.append(className);
    }
    json.insert(QStringLiteral("modelClassNames"), modelClassNamesJson);
    return json;
}

TrainingRunRecord TrainingRunRecord::fromJson(const QJsonObject &json)
{
    TrainingRunRecord record;
    record.runId = json.value("runId").toString();
    record.datasetVersionId = json.value("datasetVersionId").toString();
    record.datasetVersionDirectory = json.value("datasetVersionDirectory").toString();
    record.outputDirectory = json.value("outputDirectory").toString();
    record.state = json.value("state").toString();
    record.createdAt = json.value("createdAt").toString();
    record.completedAt = json.value("completedAt").toString();
    record.summaryPath = json.value("summaryPath").toString();
    record.bestModelPath = json.value("bestModelPath").toString();
    record.finalModelPath = json.value("finalModelPath").toString();
    record.message = json.value("message").toString();
    record.epoch = json.value("epoch").toInt();
    record.epochs = json.value("epochs").toInt();
    record.trainWindows = json.value("trainWindows").toInt();
    record.valWindows = json.value("valWindows").toInt();
    record.bestValAccuracy = jsonDouble(json, "bestValAccuracy");
    record.finalValAccuracy = jsonDouble(json, "finalValAccuracy");
    record.cancelled = jsonBool(json, QStringLiteral("cancelledState"),
                                jsonBool(json, QStringLiteral("cancelled")));
    record.partial = jsonBool(json, QStringLiteral("partial"));
    record.timedOut = jsonBool(json, QStringLiteral("timedOut"));
    record.requireCuda = jsonBool(json, QStringLiteral("requireCuda"), true);
    record.gpuDeviceName = json.value(QStringLiteral("gpuDeviceName")).toString();
    record.device = json.value(QStringLiteral("device")).toString();
    record.pipelineVersion = json.value(QStringLiteral("pipelineVersion")).toString();
    record.featureMode = json.value(QStringLiteral("featureMode")).toString();
    record.modelSize = json.value(QStringLiteral("modelSize")).toString();
    record.cacheDirectory = json.value(QStringLiteral("cacheDirectory")).toString();
    record.cacheSummaryPath = json.value(QStringLiteral("cacheSummaryPath")).toString();
    record.sampleLength = json.value(QStringLiteral("sampleLength")).toInt(4096);
    record.modelMode = json.value(QStringLiteral("modelMode")).toString(QStringLiteral("closed_set"));
    record.modelAvailable = jsonBool(
        json, QStringLiteral("modelAvailable"),
        jsonBool(json, QStringLiteral("available"),
                 jsonBool(json, QStringLiteral("model_available"))));
    record.splitSummaryPath = json.value(QStringLiteral("splitSummaryPath")).toString();
    record.modelVerificationStatus = json.value(QStringLiteral("modelVerificationStatus")).toString();
    if (record.modelVerificationStatus.isEmpty()) {
        record.modelVerificationStatus = record.pipelineVersion == QStringLiteral("external-import")
            ? QStringLiteral("unverified") : QStringLiteral("verified");
    }
    record.modelVerificationMessage = json.value(QStringLiteral("modelVerificationMessage")).toString();
    const QJsonArray modelClassNamesJson = json.value(QStringLiteral("modelClassNames")).toArray();
    for (const QJsonValue &value : modelClassNamesJson) {
        const QString className = value.toString().trimmed();
        if (!className.isEmpty()) {
            record.modelClassNames.append(className);
        }
    }
    return record;
}

QJsonObject AppState::toJson() const
{
    QJsonObject json;
    QJsonArray sampleArray;
    for (const auto &sample : samples) {
        sampleArray.append(sample.toJson());
    }
    QJsonArray candidateReviewArray;
    for (const auto &review : candidateReviews) {
        candidateReviewArray.append(review.toJson());
    }
    QJsonArray batchArray;
    for (const auto &batch : preprocessBatches) {
        batchArray.append(batch.toJson());
    }
    QJsonArray versionArray;
    for (const auto &version : datasetVersions) {
        versionArray.append(version.toJson());
    }
    QJsonArray trainingArray;
    for (const auto &trainingRun : trainingRuns) {
        trainingArray.append(trainingRun.toJson());
    }
    json.insert("samples", sampleArray);
    json.insert("candidateReviews", candidateReviewArray);
    json.insert("preprocessBatches", batchArray);
    json.insert("datasetVersions", versionArray);
    json.insert("trainingRuns", trainingArray);
    insertString(&json, QStringLiteral("activeTrainingRunId"), activeTrainingRunId);
    return json;
}

AppState AppState::fromJson(const QJsonObject &json)
{
    AppState state;
    const QJsonArray sampleArray = json.value("samples").toArray();
    for (const QJsonValue &value : sampleArray) {
        state.samples.append(SampleRecord::fromJson(value.toObject()));
    }
    const QJsonArray candidateReviewArray = json.value("candidateReviews").toArray();
    for (const QJsonValue &value : candidateReviewArray) {
        state.candidateReviews.append(CandidateReviewRecord::fromJson(value.toObject()));
    }
    const QJsonArray batchArray = json.value("preprocessBatches").toArray();
    for (const QJsonValue &value : batchArray) {
        state.preprocessBatches.append(PreprocessBatchRecord::fromJson(value.toObject()));
    }
    const QJsonArray versionArray = json.value("datasetVersions").toArray();
    for (const QJsonValue &value : versionArray) {
        state.datasetVersions.append(DatasetVersionRecord::fromJson(value.toObject()));
    }
    const QJsonArray trainingArray = json.value("trainingRuns").toArray();
    for (const QJsonValue &value : trainingArray) {
        state.trainingRuns.append(TrainingRunRecord::fromJson(value.toObject()));
    }
    state.activeTrainingRunId = json.value("activeTrainingRunId").toString();
    return state;
}
