#include "services/CapProbeService.h"

#include <QFile>
#include <QFileInfo>
#include <QDataStream>
#include <QtMath>
#include <algorithm>

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
    if (fileSize <= kHeaderLength) {
        m_lastError = QStringLiteral("文件长度不足，未达到有效 CAP 头部和 IQ 数据区");
        return std::nullopt;
    }

    const QByteArray header = file.read(kHeaderLength);
    if (header.size() < kHeaderLength) {
        m_lastError = QStringLiteral("读取 CAP 头部失败");
        return std::nullopt;
    }

    const QString version = QString::fromLatin1(header.constData(), 8).split('\0').first();
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
    result.unresolvedFields = {
        QStringLiteral("0x0008 未确认参数"),
        QStringLiteral("0x0020 - 0x00FF 其他设备参数"),
        QStringLiteral("带宽/增益/时间戳编码方式仍需更多样本交叉验证"),
    };
    return result;
}

QString CapProbeService::lastError() const
{
    return m_lastError;
}
