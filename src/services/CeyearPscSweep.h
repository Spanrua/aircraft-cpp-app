#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct CeyearPscSweepSegment
{
    double startHz = 0.0;
    double stopHz = 0.0;
    double stepHz = 0.0;
};

struct CeyearPscSweepConfig
{
    QString pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    int pcReceivePort = 19712;
    double hitThresholdDbuv = -60.0;
    QVector<CeyearPscSweepSegment> segments;
};

struct CeyearPscBin
{
    double frequencyHz = 0.0;
    double levelDbuv = 0.0;
};

struct CeyearPscFrame
{
    QVector<CeyearPscBin> bins;
};

struct CeyearPscCandidate
{
    double frequencyHz = 0.0;
    double peakLevelDbuv = 0.0;
    int hitCount = 0;
};

class CeyearPscPacketParser
{
public:
    static QVector<CeyearPscFrame> parse(const QByteArray &bytes);
};

class CeyearPscCandidateExtractor
{
public:
    explicit CeyearPscCandidateExtractor(double hitThresholdDbuv = -60.0);

    QVector<CeyearPscCandidate> update(const CeyearPscFrame &frame);
    QVector<CeyearPscCandidate> getCandidates() const;

private:
    double m_hitThresholdDbuv = -60.0;
    QVector<CeyearPscCandidate> m_candidates;
};

QStringList ceyearPscSweepCommandLines(const CeyearPscSweepConfig &config);
QStringList ceyearPscSweepStopCommandLines();
bool saveCeyearPscFramesCsv(const QString &filePath,
                            const QVector<CeyearPscFrame> &frames,
                            QString *error = nullptr);
