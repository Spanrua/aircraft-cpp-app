#pragma once

#include "core/Records.h"
#include "services/CapProbeService.h"

#include <optional>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct PreprocessSegment
{
    QString segmentId;
    int startSample = 0;
    int endSample = 0;
    double durationMs = 0.0;
    double centerFreqHz = 0.0;
    double bandwidthHz = 0.0;
    double snrDb = 0.0;
    double score = 0.0;
    QString outputFilePath;
    QString status;

    QJsonObject toJson() const;
    static PreprocessSegment fromJson(const QJsonObject &json);
    bool isValid() const;
};

struct PreprocessRunResult
{
    bool success = false;
    QString message;
    int detectedSegmentCount = 0;
    int candidateSegmentCount = 0;
    int outputSampleCount = 0;
    QString sampleOutputDir;
    QVector<PreprocessSegment> segments;
    QStringList logs;
    QVector<SampleRecord> sampleRecords;
};

struct PreprocessRequest
{
    QString inputFilePath;
    int sliceLength = 4096;
    double energyThresholdDb = 1.0;
    double noiseFloorDbm = -90.0;
    double minBandwidthMhz = 6.0;
    double minDurationMs = 0.05;
    bool enableBandpass = true;
    QString sampleOutputDir;
    QString modelWeightsPath;
    double aiConfidenceThreshold = 0.85;
    int timeoutMs = 600000;
};

class PreprocessProcessRunner
{
public:
    static QJsonObject requestToJson(const PreprocessRequest &request);
    static bool writeRequestFile(const QString &path, const PreprocessRequest &request, QString *error = nullptr);
    static std::optional<PreprocessRunResult> readResponseFile(const QString &path, QString *error = nullptr);
    static QVector<SampleRecord> buildSampleRecords(const QString &inputCapPath, const CapProbeResult &capInfo, const PreprocessRunResult &result);
    static PreprocessRunResult runDemo(const PreprocessRequest &request, const CapProbeResult &capInfo);
    static std::optional<PreprocessRunResult> run(
        const PreprocessRequest &request,
        const CapProbeResult &capInfo,
        QString *error = nullptr,
        const QString &pythonExecutable = QString(),
        const QString &runnerScript = QString());

private:
    static QString pythonExecutable();
    static QString runnerScriptPath();
};
