#include "services/CapProbeService.h"

#include <QFile>
#include <QFileInfo>
#include <QDataStream>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace
{
constexpr int kHeaderLength = 0x200;
constexpr int kPreviewPairCount = 12;
constexpr int kStatisticsWindowBytes = 1024 * 1024;

QVector<QPair<qint16, qint16>> decodePairs(const QByteArray &bytes)
{
    QVector<QPair<qint16, qint16>> pairs;
    if (bytes.size() < 4) {
        return pairs;
    }
    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::BigEndian);
    while (!stream.atEnd()) {
        qint16 iValue = 0;
        qint16 qValue = 0;
        stream >> iValue >> qValue;
        if (stream.status() != QDataStream::Ok) {
            break;
        }
        pairs.append({iValue, qValue});
    }
    return pairs;
}

QVector<QPair<qint16, qint16>> decodePairsLittleEndian(const QByteArray &bytes)
{
    QVector<QPair<qint16, qint16>> pairs;
    if (bytes.size() < 4) {
        return pairs;
    }
    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::LittleEndian);
    while (!stream.atEnd()) {
        qint16 iValue = 0;
        qint16 qValue = 0;
        stream >> iValue >> qValue;
        if (stream.status() != QDataStream::Ok) {
            break;
        }
        pairs.append({iValue, qValue});
    }
    return pairs;
}

quint16 readUInt16BE(const QByteArray &bytes, int offset)
{
    if (offset < 0 || offset + 2 > bytes.size()) {
        return 0;
    }
    return (static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset))) << 8)
        | static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset + 1)));
}

quint32 readUInt32BE(const QByteArray &bytes, int offset)
{
    if (offset < 0 || offset + 4 > bytes.size()) {
        return 0;
    }
    return (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset))) << 24)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 1))) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 2))) << 8)
        | static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 3)));
}

bool isIqrawMagic(const QByteArray &bytes, int offset)
{
    return offset >= 0 && offset + 4 <= bytes.size() && bytes.mid(offset, 4) == QByteArray::fromHex("000eb200");
}

double populationStd(const QVector<qint16> &values, double mean)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    double variance = 0.0;
    for (qint16 value : values) {
        const double delta = value - mean;
        variance += delta * delta;
    }
    variance /= values.size();
    return std::sqrt(variance);
}

std::optional<CapProbeResult> probeIqraw(QFile &file, const QString &path)
{
    constexpr int kPrefixLength = 28;
    constexpr quint16 kIfIqPacketHeader = 901;
    const qint64 fileSize = file.size();
    const qint64 scanLimit = qMin<qint64>(fileSize, 8 * 1024 * 1024);
    if (!file.seek(0)) {
        return std::nullopt;
    }
    const QByteArray bytes = file.read(scanLimit);
    int packetOffset = -1;
    quint32 packetLength = 0;
    int headerLength = 0;
    for (int offset = 0; offset + kPrefixLength <= bytes.size(); ++offset) {
        if (!isIqrawMagic(bytes, offset)) {
            continue;
        }
        const quint32 candidateLength = readUInt32BE(bytes, offset + 12);
        const quint16 packetHeader = readUInt16BE(bytes, offset + 16);
        const int optionalLength = static_cast<unsigned char>(bytes.at(offset + 23));
        const int candidateHeaderLength = kPrefixLength + optionalLength;
        if (candidateLength < static_cast<quint32>(candidateHeaderLength)
            || offset + static_cast<qint64>(candidateLength) > fileSize
            || packetHeader != kIfIqPacketHeader) {
            continue;
        }
        packetOffset = offset;
        packetLength = candidateLength;
        headerLength = candidateHeaderLength;
        break;
    }
    if (packetOffset < 0 || packetLength <= static_cast<quint32>(headerLength)) {
        return std::nullopt;
    }

    const QByteArray optionalHeader = bytes.mid(packetOffset + kPrefixLength, headerLength - kPrefixLength);
    if (optionalHeader.size() < 58) {
        return std::nullopt;
    }
    const quint32 sampleRate = readUInt32BE(optionalHeader, 4);
    const quint32 bandwidth = readUInt32BE(optionalHeader, 12);
    const quint32 lowFrequency = readUInt32BE(optionalHeader, 8);
    const quint32 highFrequency = readUInt32BE(optionalHeader, 40);
    const quint64 centerFrequency = (static_cast<quint64>(highFrequency) << 32) | lowFrequency;
    const qint64 payloadLength = packetLength - headerLength;
    const qint64 previewLength = qMin<qint64>(payloadLength, 12 * 4);
    if (!file.seek(packetOffset + headerLength)) {
        return std::nullopt;
    }
    const QByteArray previewBytes = file.read(previewLength);
    const auto previewPairs = decodePairsLittleEndian(previewBytes);
    if (previewPairs.isEmpty()) {
        return std::nullopt;
    }

    QVector<qint16> iValues;
    QVector<qint16> qValues;
    for (const auto &pair : previewPairs) {
        iValues.append(pair.first);
        qValues.append(pair.second);
    }
    const auto average = [](const QVector<qint16> &values) {
        if (values.isEmpty()) {
            return 0.0;
        }
        double total = 0.0;
        for (const qint16 value : values) {
            total += value;
        }
        return total / values.size();
    };

    CapProbeResult result;
    result.filePath = QFileInfo(path).absoluteFilePath();
    result.fileSize = fileSize;
    result.version = QStringLiteral("IQRAW");
    result.headerLength = headerLength;
    result.bandwidthHz = bandwidth;
    result.sampleRateHz = sampleRate;
    result.centerFrequencyHz = static_cast<double>(centerFrequency);
    result.frameSampleCount = static_cast<quint32>(payloadLength / 4);
    result.blockSize = packetLength;
    result.previewPairs = previewPairs;
    result.statistics.sampleCount = previewPairs.size();
    result.statistics.iMean = average(iValues);
    result.statistics.qMean = average(qValues);
    result.statistics.iStd = populationStd(iValues, result.statistics.iMean);
    result.statistics.qStd = populationStd(qValues, result.statistics.qMean);
    result.statistics.iMin = *std::min_element(iValues.constBegin(), iValues.constEnd());
    result.statistics.iMax = *std::max_element(iValues.constBegin(), iValues.constEnd());
    result.statistics.qMin = *std::min_element(qValues.constBegin(), qValues.constEnd());
    result.statistics.qMax = *std::max_element(qValues.constBegin(), qValues.constEnd());
    return result;
}
} // namespace

std::optional<CapProbeResult> CapProbeService::probe(const QString &path)
{
    QFile file(path);
    if (!file.exists()) {
        m_lastError = QStringLiteral("文件不存在，无法执行导入预览");
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        return std::nullopt;
    }

    const qint64 fileSize = file.size();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("iqraw")) {
        const auto result = probeIqraw(file, path);
        if (!result.has_value()) {
            m_lastError = QStringLiteral("未找到有效的 3943B IQRAW 数据包，无法读取元数据");
        }
        return result;
    }
    if (fileSize <= kHeaderLength) {
        m_lastError = QStringLiteral("文件长度不足，未达到有效 CAP 头部和 IQ 数据区");
        return std::nullopt;
    }

    const QByteArray header = file.read(kHeaderLength);
    if (header.size() < kHeaderLength) {
        m_lastError = QStringLiteral("读取 CAP 头部失败");
        return std::nullopt;
    }

    const QString version = QString::fromLatin1(header.constData(), 8).split(QChar('\0')).first();
    if (!version.startsWith(QStringLiteral("B."))) {
        m_lastError = QStringLiteral("文件头版本标记异常，不符合当前 CAP 预览规则");
        return std::nullopt;
    }

    auto readDoubleBE = [&header](int offset) -> double {
        double value = 0.0;
        QByteArray bytes = header.mid(offset, sizeof(double));
        QDataStream stream(bytes);
        stream.setByteOrder(QDataStream::BigEndian);
        stream >> value;
        return value;
    };

    auto readUInt32BE = [&header](int offset) -> quint32 {
        quint32 value = 0;
        QByteArray bytes = header.mid(offset, sizeof(quint32));
        QDataStream stream(bytes);
        stream.setByteOrder(QDataStream::BigEndian);
        stream >> value;
        return value;
    };

    const double bandwidthHz = readDoubleBE(0x0010);
    const double sampleRateHz = bandwidthHz * 1.28;
    const double centerFrequencyHz = readDoubleBE(0x0018);
    const quint32 frameSampleCount = readUInt32BE(0x0110);
    const quint32 blockSize = readUInt32BE(0x0114);

    const QByteArray previewBytes = file.read(qMin<qint64>(kPreviewPairCount * 4, fileSize - kHeaderLength));
    const QVector<QPair<qint16, qint16>> previewPairs = decodePairs(previewBytes);

    file.seek(kHeaderLength);
    const QByteArray statisticsBytes = file.read(qMin<qint64>(kStatisticsWindowBytes, fileSize - kHeaderLength));
    const QVector<QPair<qint16, qint16>> statisticsPairs = decodePairs(statisticsBytes);
    if (statisticsPairs.isEmpty()) {
        m_lastError = QStringLiteral("IQ 数据区为空，无法生成导入预览");
        return std::nullopt;
    }

    QVector<qint16> iValues;
    QVector<qint16> qValues;
    iValues.reserve(statisticsPairs.size());
    qValues.reserve(statisticsPairs.size());
    for (const auto &pair : statisticsPairs) {
        iValues.append(pair.first);
        qValues.append(pair.second);
    }

    const auto average = [](const QVector<qint16> &values) -> double {
        if (values.isEmpty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (qint16 value : values) {
            sum += value;
        }
        return sum / values.size();
    };

    IQStatistics statistics;
    statistics.sampleCount = statisticsPairs.size();
    statistics.iMean = average(iValues);
    statistics.qMean = average(qValues);
    statistics.iStd = populationStd(iValues, statistics.iMean);
    statistics.qStd = populationStd(qValues, statistics.qMean);
    statistics.iMin = *std::min_element(iValues.constBegin(), iValues.constEnd());
    statistics.iMax = *std::max_element(iValues.constBegin(), iValues.constEnd());
    statistics.qMin = *std::min_element(qValues.constBegin(), qValues.constEnd());
    statistics.qMax = *std::max_element(qValues.constBegin(), qValues.constEnd());

    CapProbeResult result;
    const QFileInfo inputFileInfo(path);
    result.filePath = inputFileInfo.absoluteFilePath();
    result.fileSize = fileSize;
    result.isPartialCapture = inputFileInfo.fileName().compare(QStringLiteral("head.cap"), Qt::CaseInsensitive) == 0;
    result.version = version;
    result.headerLength = kHeaderLength;
    result.bandwidthHz = bandwidthHz;
    result.sampleRateHz = sampleRateHz;
    result.centerFrequencyHz = centerFrequencyHz;
    result.frameSampleCount = frameSampleCount;
    result.blockSize = blockSize;
    result.previewPairs = previewPairs;
    result.statistics = statistics;
    result.unresolvedFields.clear();
    result.unresolvedFields.append(QStringLiteral("0x0008 未确认参数"));
    result.unresolvedFields.append(QStringLiteral("0x0020 - 0x00FF 其他设备参数"));
    result.unresolvedFields.append(QStringLiteral("带宽/增益/时间戳编码方式仍需更多样本交叉验证"));
    return result;
}

QString CapProbeService::lastError() const
{
    return m_lastError;
}
