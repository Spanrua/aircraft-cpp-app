#include "core/Records.h"

#include <QJsonArray>

namespace
{
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
    json.insert("sampleId", sampleId);
    json.insert("sourceType", sourceType);
    json.insert("rawFilePath", rawFilePath);
    json.insert("sampleFilePath", sampleFilePath);
    json.insert("labelType", labelType);
    json.insert("labelIndividual", labelIndividual);
    json.insert("sampleRateHz", sampleRateHz);
    json.insert("centerFrequencyHz", centerFrequencyHz);
    json.insert("dataFormat", dataFormat);
    json.insert("sampleCount", sampleCount);
    json.insert("deviceId", deviceId);
    json.insert("startSample", startSample);
    json.insert("endSample", endSample);
    json.insert("status", status);
    json.insert("sourceName", sourceName);
    return json;
}

SampleRecord SampleRecord::fromJson(const QJsonObject &json)
{
    SampleRecord record;
    record.sampleId = json.value("sampleId").toString();
    record.sourceType = json.value("sourceType").toString();
    record.rawFilePath = json.value("rawFilePath").toString();
    record.sampleFilePath = json.value("sampleFilePath").toString();
    record.labelType = json.value("labelType").toString();
    record.labelIndividual = json.value("labelIndividual").toString();
    record.sampleRateHz = json.value("sampleRateHz").toDouble();
    record.centerFrequencyHz = json.value("centerFrequencyHz").toDouble();
    record.dataFormat = json.value("dataFormat").toString();
    record.sampleCount = json.value("sampleCount").toInt();
    record.deviceId = json.value("deviceId").toString();
    record.startSample = json.value("startSample").toInt();
    record.endSample = json.value("endSample").toInt();
    record.status = json.value("status").toString(QStringLiteral("已标注"));
    record.sourceName = json.value("sourceName").toString();
    return record;
}

QJsonObject DatasetVersionRecord::toJson() const
{
    QJsonObject json;
    json.insert("versionId", versionId);
    json.insert("taskType", taskType);
    json.insert("sampleCount", sampleCount);
    json.insert("strategy", strategy);
    json.insert("createdAt", createdAt);
    json.insert("sourceSummary", sourceSummary);
    json.insert("labelCounts", labelCountsToJson(labelCounts));
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
    record.labelCounts = labelCountsFromJson(json.value("labelCounts").toObject());
    return record;
}

QJsonObject AppState::toJson() const
{
    QJsonObject json;
    QJsonArray sampleArray;
    for (const auto &sample : samples) {
        sampleArray.append(sample.toJson());
    }
    QJsonArray versionArray;
    for (const auto &version : datasetVersions) {
        versionArray.append(version.toJson());
    }
    json.insert("samples", sampleArray);
    json.insert("datasetVersions", versionArray);
    return json;
}

AppState AppState::fromJson(const QJsonObject &json)
{
    AppState state;
    const QJsonArray sampleArray = json.value("samples").toArray();
    for (const QJsonValue &value : sampleArray) {
        state.samples.append(SampleRecord::fromJson(value.toObject()));
    }
    const QJsonArray versionArray = json.value("datasetVersions").toArray();
    for (const QJsonValue &value : versionArray) {
        state.datasetVersions.append(DatasetVersionRecord::fromJson(value.toObject()));
    }
    return state;
}

