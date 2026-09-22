#pragma once

#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

struct SampleRecord
{
    QString sampleId;
    QString batchId;
    QString candidateId;
    QString sourceType;
    QString rawFilePath;
    QString sampleFilePath;
    QString spectrogramFilePath;
    QString candidatePreviewFilePath;
    QString labelType;
    QString labelIndividual;
    double sampleRateHz = 0.0;
    double centerFrequencyHz = 0.0;
    double bandwidthHz = 0.0;
    double snrDb = 0.0;
    double score = 0.0;
    double occupancy = 0.0;
    QString dataFormat;
    int sampleCount = 0;
    int sampleIndex = 0;
    int windowIndex = 0;
    QString deviceId;
    int startSample = 0;
    int endSample = 0;
    QString status = QStringLiteral("已标注");
    QString processingState;
    QString qualityStatus;
    QString pipelineVersion;
    QString sourceName;

    QJsonObject toJson() const;
    static SampleRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(SampleRecord)

struct CandidateReviewRecord
{
    QString reviewKey;
    QString sourceFilePath;
    QString batchId;
    QString candidateId;
    QString candidatePreviewPath;
    QString representativeSampleId;
    QString labelType;
    int windowCount = 0;
    int validWindowCount = 0;
    double averageScore = 0.0;
    double minimumScore = 0.0;
    double maximumScore = 0.0;
    double averageSnrDb = 0.0;
    double averageOccupancy = 0.0;
    double ruleScore = 0.0;
    QString recommendation = QStringLiteral("待人工复核");
    QString reviewState = QStringLiteral("待复核");
    QString reason;
    QString algorithmVersion;
    QString reviewedAt;

    QJsonObject toJson() const;
    static CandidateReviewRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(CandidateReviewRecord)

struct PreprocessBatchRecord
{
    QString batchId;
    QString taskId;
    QString inputFilePath;
    QString outputDirectory;
    QString summaryPath;
    QString profileName;
    QString state;
    QString createdAt;
    int candidateCount = 0;
    int sampleCount = 0;
    bool partial = false;

    QJsonObject toJson() const;
    static PreprocessBatchRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(PreprocessBatchRecord)

struct DatasetVersionRecord
{
    QString versionId;
    QString taskType;
    int sampleCount = 0;
    QString strategy;
    QString createdAt;
    QString sourceSummary;
    QString outputDirectory;
    QString manifestPath;
    QStringList selectedLabels;
    bool includePartial = false;
    QMap<QString, int> labelCounts;

    QJsonObject toJson() const;
    static DatasetVersionRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(DatasetVersionRecord)

struct TrainingRunRecord
{
    QString runId;
    QString datasetVersionId;
    QString datasetVersionDirectory;
    QString outputDirectory;
    QString state;
    QString createdAt;
    QString completedAt;
    QString summaryPath;
    QString bestModelPath;
    QString finalModelPath;
    QString message;
    int epoch = 0;
    int epochs = 0;
    int trainWindows = 0;
    int valWindows = 0;
    double bestValAccuracy = 0.0;
    double finalValAccuracy = 0.0;
    bool cancelled = false;
    bool partial = false;
    bool timedOut = false;
    bool requireCuda = true;
    QString gpuDeviceName;
    QString device;
    QString pipelineVersion;
    QString featureMode;
    QString modelSize;
    QString cacheDirectory;
    QString cacheSummaryPath;
    int sampleLength = 4096;
    QString modelMode = QStringLiteral("closed_set");
    bool modelAvailable = false;
    QString splitSummaryPath;

    QJsonObject toJson() const;
    static TrainingRunRecord fromJson(const QJsonObject &json);
};

Q_DECLARE_METATYPE(TrainingRunRecord)

struct AppState
{
    QVector<SampleRecord> samples;
    QVector<CandidateReviewRecord> candidateReviews;
    QVector<PreprocessBatchRecord> preprocessBatches;
    QVector<DatasetVersionRecord> datasetVersions;
    QVector<TrainingRunRecord> trainingRuns;
    QString activeTrainingRunId;

    QJsonObject toJson() const;
    static AppState fromJson(const QJsonObject &json);
};
