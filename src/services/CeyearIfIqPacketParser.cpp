#include "services/CeyearIfIqPacketParser.h"

#include <QFile>

namespace
{
constexpr int kMinimumPrefixLength = 28;
constexpr quint32 kMagic = 0x000EB200;
constexpr quint16 kIfPacketHeader = 901;
constexpr quint16 kPscanPacketHeader = 1201;

quint16 readUInt16BE(const QByteArray &bytes, int offset)
{
    return static_cast<quint16>((static_cast<quint8>(bytes.at(offset)) << 8)
                                | static_cast<quint8>(bytes.at(offset + 1)));
}

quint32 readUInt32BE(const QByteArray &bytes, int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) << 24)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 8)
        | static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3)));
}

qint16 readInt16LE(const QByteArray &bytes, int offset)
{
    const quint16 value = static_cast<quint16>(static_cast<quint8>(bytes.at(offset)))
        | static_cast<quint16>(static_cast<quint8>(bytes.at(offset + 1)) << 8);
    return static_cast<qint16>(value);
}
} // namespace

QVector<QPair<qint16, qint16>> CeyearIfIqPacketParser::extractSamples(const QByteArray &rawBytes, QString *error)
{
    QVector<QPair<qint16, qint16>> samples;
    int offset = 0;
    int packetCount = 0;

    while (offset + kMinimumPrefixLength <= rawBytes.size()) {
        if (readUInt32BE(rawBytes, offset) != kMagic) {
            ++offset;
            continue;
        }

        const int packetLength = static_cast<int>(readUInt32BE(rawBytes, offset + 12));
        if (packetLength < kMinimumPrefixLength) {
            if (!samples.isEmpty()) {
                break;
            }
            if (error) {
                *error = QStringLiteral("Invalid 3943B IF packet length: %1").arg(packetLength);
            }
            return {};
        }
        if (offset + packetLength > rawBytes.size()) {
            if (!samples.isEmpty()) {
                break;
            }
            if (error) {
                *error = QStringLiteral("Incomplete 3943B IF packet: need %1 bytes, have %2 bytes")
                    .arg(packetLength)
                    .arg(rawBytes.size() - offset);
            }
            return {};
        }

        const int packetHeader = static_cast<int>(readUInt16BE(rawBytes, offset + 16));
        if (packetHeader != kIfPacketHeader) {
            offset += packetLength;
            ++packetCount;
            continue;
        }

        const int optionalHeaderLength = static_cast<quint8>(rawBytes.at(offset + 23));
        const int payloadOffset = offset + kMinimumPrefixLength + optionalHeaderLength;
        const int payloadLength = packetLength - (kMinimumPrefixLength + optionalHeaderLength);
        if (payloadOffset > offset + packetLength || payloadLength < 0) {
            if (error) {
                *error = QStringLiteral("3943B IF packet payload is invalid: %1 bytes").arg(payloadLength);
            }
            return {};
        }
        if (payloadLength % 4 != 0) {
            if (error) {
                *error = QStringLiteral("3943B IF payload length is not aligned to int16 I/Q pairs: %1")
                    .arg(payloadLength);
            }
            return {};
        }

        for (int i = 0; i < payloadLength; i += 4) {
            samples.append({readInt16LE(rawBytes, payloadOffset + i),
                            readInt16LE(rawBytes, payloadOffset + i + 2)});
        }

        offset += packetLength;
        ++packetCount;
    }

    if (samples.isEmpty()) {
        if (error) {
            *error = packetCount == 0
                ? QStringLiteral("No 3943B IF packets with magic 0x000EB200 were found.")
                : QStringLiteral("3943B IF packets contained no IQ samples.");
        }
        return {};
    }
    return samples;
}

bool CeyearIfIqPacketParser::validateRawBytes(const QByteArray &rawBytes,
                                              CeyearIfIqRawSummary *summary,
                                              QString *error)
{
    CeyearIfIqRawSummary localSummary;
    localSummary.bytesRead = rawBytes.size();

    if (rawBytes.isEmpty()) {
        if (summary) {
            *summary = localSummary;
        }
        if (error) {
            *error = QStringLiteral("3943B IQRAW file is empty.");
        }
        return false;
    }

    int offset = 0;
    while (offset + kMinimumPrefixLength <= rawBytes.size()) {
        if (readUInt32BE(rawBytes, offset) != kMagic) {
            ++offset;
            continue;
        }

        const int packetLength = static_cast<int>(readUInt32BE(rawBytes, offset + 12));
        if (packetLength < kMinimumPrefixLength) {
            localSummary.trailingBytes = rawBytes.size() - offset;
            if (localSummary.ifPacketCount > 0 && localSummary.sampleCount > 0) {
                break;
            }
            if (summary) {
                *summary = localSummary;
            }
            if (error) {
                *error = QStringLiteral("Invalid 3943B packet length: %1").arg(packetLength);
            }
            return false;
        }
        if (offset + packetLength > rawBytes.size()) {
            localSummary.trailingBytes = rawBytes.size() - offset;
            if (localSummary.ifPacketCount > 0 && localSummary.sampleCount > 0) {
                break;
            }
            if (summary) {
                *summary = localSummary;
            }
            if (error) {
                *error = QStringLiteral("Incomplete 3943B packet: need %1 bytes, have %2 bytes")
                    .arg(packetLength)
                    .arg(rawBytes.size() - offset);
            }
            return false;
        }

        const int packetHeader = static_cast<int>(readUInt16BE(rawBytes, offset + 16));
        if (localSummary.packetCount == 0) {
            localSummary.firstPacketHeader = packetHeader;
        }

        if (packetHeader == kIfPacketHeader) {
            const int optionalHeaderLength = static_cast<quint8>(rawBytes.at(offset + 23));
            const int payloadOffset = offset + kMinimumPrefixLength + optionalHeaderLength;
            const int payloadLength = packetLength - (kMinimumPrefixLength + optionalHeaderLength);
            if (payloadOffset > offset + packetLength || payloadLength < 0 || payloadLength % 4 != 0) {
                if (summary) {
                    *summary = localSummary;
                }
                if (error) {
                    *error = QStringLiteral("3943B IF packet payload is invalid: %1 bytes").arg(payloadLength);
                }
                return false;
            }
            localSummary.ifPacketCount += 1;
            localSummary.sampleCount += payloadLength / 4;
        } else {
            localSummary.nonIfPacketCount += 1;
            if (localSummary.firstNonIfPacketHeader < 0) {
                localSummary.firstNonIfPacketHeader = packetHeader;
            }
        }

        localSummary.packetCount += 1;
        offset += packetLength;
    }

    if (summary) {
        *summary = localSummary;
    }
    if (localSummary.ifPacketCount > 0 && localSummary.sampleCount > 0) {
        return true;
    }

    if (error) {
        if (localSummary.packetCount == 0) {
            *error = QStringLiteral("No 3943B packet with magic 0x000EB200 was found.");
        } else if (localSummary.firstNonIfPacketHeader == kPscanPacketHeader) {
            *error = QStringLiteral("Received 3943B PSCAN packets (tag 1201), not IF/IQ packets (tag 901).");
        } else if (localSummary.ifPacketCount > 0) {
            *error = QStringLiteral("3943B IF/IQ packets contained no IQ samples.");
        } else {
            *error = QStringLiteral("No 3943B IF/IQ packet with tag 901 was found. First packet tag: %1.")
                .arg(localSummary.firstPacketHeader);
        }
    }
    return false;
}

bool CeyearIfIqPacketParser::validateRawFile(const QString &rawFilePath,
                                             CeyearIfIqRawSummary *summary,
                                             QString *error)
{
    QFile rawFile(rawFilePath);
    if (!rawFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot open 3943B raw IQ file: %1").arg(rawFile.errorString());
        }
        return false;
    }

    return validateRawBytes(rawFile.readAll(), summary, error);
}

bool CeyearIfIqPacketParser::validateRawFilePrefix(const QString &rawFilePath,
                                                   CeyearIfIqRawSummary *summary,
                                                   QString *error,
                                                   qint64 maxBytes)
{
    QFile rawFile(rawFilePath);
    if (!rawFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot open 3943B raw IQ file: %1").arg(rawFile.errorString());
        }
        return false;
    }

    const qint64 bytesToRead = maxBytes > 0 ? maxBytes : 1024 * 1024;
    return validateRawBytes(rawFile.read(bytesToRead), summary, error);
}

bool CeyearIfIqPacketParser::convertRawFileToCap(const QString &rawFilePath,
                                                 const UsrpCaptureConfig &capConfig,
                                                 QString *error)
{
    QFile rawFile(rawFilePath);
    if (!rawFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot open 3943B raw IQ file: %1").arg(rawFile.errorString());
        }
        return false;
    }

    const QByteArray rawBytes = rawFile.readAll();
    const QVector<QPair<qint16, qint16>> samples = extractSamples(rawBytes, error);
    if (samples.isEmpty()) {
        return false;
    }

    return UsrpDeviceService::writeCapFile(capConfig, samples, error);
}
