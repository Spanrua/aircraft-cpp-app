#include "services/CeyearPscSweep.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QTextStream>
#include <QtGlobal>

namespace
{
constexpr int kMinimumPacketHeaderLength = 28;
constexpr quint32 kMagic = 0x000EB200;
constexpr quint16 kPscanHeader = 1201;
constexpr quint32 kLevelFlag = 0x00000001;
constexpr quint32 kFreqLowFlag = 0x00020000;
constexpr quint32 kFreqHighFlag = 0x00200000;
constexpr int kDefaultMinimumHits = 2;
constexpr double kDefaultFrequencyToleranceHz = 25000.0;

QString hzText(double hz)
{
    return QLocale::c().toString(hz, 'f', 0) + QStringLiteral(" Hz");
}

quint16 readUInt16BE(const QByteArray &bytes, int offset)
{
    return static_cast<quint16>((static_cast<quint8>(bytes.at(offset)) << 8)
                                | static_cast<quint8>(bytes.at(offset + 1)));
}

qint16 readInt16BE(const QByteArray &bytes, int offset)
{
    return static_cast<qint16>(readUInt16BE(bytes, offset));
}

quint32 readUInt32BE(const QByteArray &bytes, int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) << 24)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 8)
        | static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3)));
}

bool hasRequiredPscFlags(quint32 flags)
{
    return (flags & kLevelFlag) && (flags & kFreqLowFlag) && (flags & kFreqHighFlag);
}

double combineFrequencyHz(quint32 low, quint32 high)
{
    const quint64 value = (static_cast<quint64>(high) << 32) | static_cast<quint64>(low);
    return static_cast<double>(value);
}

double binStepHz(const CeyearPscFrame &frame)
{
    if (frame.bins.size() < 2) {
        return kDefaultFrequencyToleranceHz * 2.0;
    }
    return qAbs(frame.bins.at(1).frequencyHz - frame.bins.at(0).frequencyHz);
}

int findCandidateIndex(const QVector<CeyearPscCandidate> &candidates, double frequencyHz, double toleranceHz)
{
    for (int i = 0; i < candidates.size(); ++i) {
        if (qAbs(candidates.at(i).frequencyHz - frequencyHz) <= toleranceHz) {
            return i;
        }
    }
    return -1;
}
} // namespace

QStringList ceyearPscSweepCommandLines(const CeyearPscSweepConfig &config)
{
    const QString ip = config.pcReceiveIpAddress.trimmed();
    const QString port = QString::number(config.pcReceivePort);
    const CeyearPscSweepSegment segment = config.segments.isEmpty()
        ? CeyearPscSweepSegment{900000000.0, 1100000000.0, 25000.0}
        : config.segments.first();

    return {
        QStringLiteral("FREQ:MODE PSC"),
        QStringLiteral("TRAC:UDP:TAG:ON \"%1\",%2,PSC").arg(ip, port),
        QStringLiteral("TRAC:UDP:FLAG:ON \"%1\",%2,\"VOLT:AC\",\"FREQ:RX\",\"FREQ:HIGH:RX\",\"OPT\"").arg(ip, port),
        QStringLiteral("FREQ:PSC:STAR %1").arg(hzText(segment.startHz)),
        QStringLiteral("FREQ:PSC:STOP %1").arg(hzText(segment.stopHz)),
        QStringLiteral("PSC:STEP %1").arg(hzText(segment.stepHz)),
        QStringLiteral("INIT"),
    };
}

QStringList ceyearPscSweepStopCommandLines()
{
    return {
        QStringLiteral("ABORt"),
        QStringLiteral("*CLS"),
        QStringLiteral("TRAC:UDP:DEL ALL"),
        QStringLiteral("TRAC:TCP:DEL ALL"),
        QStringLiteral("FREQ:MODE FFM"),
    };
}

bool saveCeyearPscFramesCsv(const QString &filePath, const QVector<CeyearPscFrame> &frames, QString *error)
{
    if (frames.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No PSC sweep frames to save.");
        }
        return false;
    }

    const QString parentDir = QFileInfo(filePath).absolutePath();
    if (!QDir().mkpath(parentDir)) {
        if (error) {
            *error = QStringLiteral("Cannot create PSC sweep output directory: %1").arg(parentDir);
        }
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Cannot open PSC sweep output file: %1").arg(file.errorString());
        }
        return false;
    }

    QTextStream out(&file);
    out << "frame_index,bin_index,frequency_hz,level_dbuv\n";
    for (int frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const CeyearPscFrame &frame = frames.at(frameIndex);
        for (int binIndex = 0; binIndex < frame.bins.size(); ++binIndex) {
            const CeyearPscBin &bin = frame.bins.at(binIndex);
            out << frameIndex << ','
                << binIndex << ','
                << QLocale::c().toString(bin.frequencyHz, 'f', 0) << ','
                << QLocale::c().toString(bin.levelDbuv, 'f', 1) << '\n';
        }
    }
    out.flush();
    if (out.status() != QTextStream::Ok) {
        if (error) {
            *error = QStringLiteral("Cannot write PSC sweep output file.");
        }
        return false;
    }

    file.close();
    if (file.error() != QFileDevice::NoError) {
        if (error) {
            *error = QStringLiteral("Cannot close PSC sweep output file: %1").arg(file.errorString());
        }
        return false;
    }

    return true;
}

QVector<CeyearPscFrame> CeyearPscPacketParser::parse(const QByteArray &bytes)
{
    QVector<CeyearPscFrame> frames;
    int offset = 0;

    while (offset + kMinimumPacketHeaderLength <= bytes.size()) {
        if (readUInt32BE(bytes, offset) != kMagic) {
            ++offset;
            continue;
        }

        const quint32 packetLength = readUInt32BE(bytes, offset + 12);
        if (packetLength < kMinimumPacketHeaderLength
            || offset + static_cast<int>(packetLength) > bytes.size()) {
            return {};
        }

        const quint16 packetHeader = readUInt16BE(bytes, offset + 16);
        if (packetHeader != kPscanHeader) {
            offset += static_cast<int>(packetLength);
            continue;
        }

        const quint16 attributeLength = readUInt16BE(bytes, offset + 18);
        const quint16 itemCount = readUInt16BE(bytes, offset + 20);
        const quint8 optionalHeaderLength = static_cast<quint8>(bytes.at(offset + 23));
        const quint32 selectorFlags = readUInt32BE(bytes, offset + 24);
        const int payloadOffset = offset + kMinimumPacketHeaderLength + optionalHeaderLength;
        const int payloadLength = itemCount * 2 + itemCount * 4 + itemCount * 4;
        if (!hasRequiredPscFlags(selectorFlags)
            || itemCount == 0
            || attributeLength < optionalHeaderLength + payloadLength
            || payloadOffset + payloadLength > offset + static_cast<int>(packetLength)) {
            offset += static_cast<int>(packetLength);
            continue;
        }

        CeyearPscFrame frame;
        frame.bins.reserve(itemCount);
        const int lowOffset = payloadOffset + itemCount * 2;
        const int highOffset = lowOffset + itemCount * 4;
        for (int i = 0; i < itemCount; ++i) {
            CeyearPscBin bin;
            bin.levelDbuv = static_cast<double>(readInt16BE(bytes, payloadOffset + i * 2)) / 10.0;
            bin.frequencyHz = combineFrequencyHz(readUInt32BE(bytes, lowOffset + i * 4),
                                                 readUInt32BE(bytes, highOffset + i * 4));
            frame.bins.append(bin);
        }
        frames.append(frame);
        offset += static_cast<int>(packetLength);
    }

    return frames;
}

CeyearPscCandidateExtractor::CeyearPscCandidateExtractor(double hitThresholdDbuv)
    : m_hitThresholdDbuv(hitThresholdDbuv)
{
}

QVector<CeyearPscCandidate> CeyearPscCandidateExtractor::update(const CeyearPscFrame &frame)
{
    const double toleranceHz = qMax(kDefaultFrequencyToleranceHz, binStepHz(frame) / 2.0);

    for (const CeyearPscBin &bin : frame.bins) {
        if (bin.levelDbuv < m_hitThresholdDbuv) {
            continue;
        }

        const int index = findCandidateIndex(m_candidates, bin.frequencyHz, toleranceHz);
        if (index >= 0) {
            CeyearPscCandidate &candidate = m_candidates[index];
            candidate.peakLevelDbuv = qMax(candidate.peakLevelDbuv, bin.levelDbuv);
            candidate.hitCount += 1;
        } else {
            CeyearPscCandidate candidate;
            candidate.frequencyHz = bin.frequencyHz;
            candidate.peakLevelDbuv = bin.levelDbuv;
            candidate.hitCount = 1;
            m_candidates.append(candidate);
        }
    }

    QVector<CeyearPscCandidate> promoted;
    for (const CeyearPscCandidate &candidate : m_candidates) {
        if (candidate.hitCount >= kDefaultMinimumHits) {
            promoted.append(candidate);
        }
    }
    return promoted;
}

QVector<CeyearPscCandidate> CeyearPscCandidateExtractor::getCandidates() const
{
    QVector<CeyearPscCandidate> promoted;
    for (const CeyearPscCandidate &candidate : m_candidates) {
        if (candidate.hitCount >= kDefaultMinimumHits) {
            promoted.append(candidate);
        }
    }
    return promoted;
}
