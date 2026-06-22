#pragma once

#include <optional>

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

struct IQStatistics
{
    int sampleCount = 0;
    double iMean = 0.0;
    double qMean = 0.0;
    double iStd = 0.0;
    double qStd = 0.0;
    qint16 iMin = 0;
    qint16 iMax = 0;
    qint16 qMin = 0;
    qint16 qMax = 0;
};

struct CapProbeResult
{
    QString filePath;
    qint64 fileSize = 0;
    bool isPartialCapture = false;
    QString version;
    int headerLength = 0;
    double bandwidthHz = 0.0;
    double sampleRateHz = 0.0;
    double centerFrequencyHz = 0.0;
    quint32 frameSampleCount = 0;
    quint32 blockSize = 0;
    QVector<QPair<qint16, qint16>> previewPairs;
    IQStatistics statistics;
    QStringList unresolvedFields;
};

class CapProbeService
{
public:
    std::optional<CapProbeResult> probe(const QString &path);
    QString lastError() const;

private:
    QString m_lastError;
};

