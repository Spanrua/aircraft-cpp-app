#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

struct SampleRecord
{
    QString sampleId;
    QString sourceType;
    QString rawFilePath;
    QString sampleFilePath;
    QString labelType;
    QString labelIndividual;
    double sampleRateHz = 0.0;
    double centerFrequencyHz = 0.0;
    QString dataFormat;
    int sampleCount = 0;
    QString deviceId;
    int startSample = 0;
    int endSample = 0;
    QString status = QStringLiteral("已标注");
    QString sourceName;

    QJsonObject toJson() const;
    static SampleRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(SampleRecord)

struct DatasetVersionRecord
{
    QString versionId;
    QString taskType;
    int sampleCount = 0;
    QString strategy;
    QString createdAt;
    QString sourceSummary;
    QMap<QString, int> labelCounts;

    QJsonObject toJson() const;
    static DatasetVersionRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(DatasetVersionRecord)

struct AppState
{
    QVector<SampleRecord> samples;
    QVector<DatasetVersionRecord> datasetVersions;

    QJsonObject toJson() const;
    static AppState fromJson(const QJsonObject &json);
};

