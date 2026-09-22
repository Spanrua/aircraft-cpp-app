#pragma once

#include "services/UsrpDeviceService.h"

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

struct CeyearIfIqRawSummary
{
    int packetCount = 0;
    int ifPacketCount = 0;
    int nonIfPacketCount = 0;
    int firstPacketHeader = -1;
    int firstNonIfPacketHeader = -1;
    int sampleCount = 0;
    qint64 bytesRead = 0;
    qint64 trailingBytes = 0;
};

class CeyearIfIqPacketParser
{
public:
    static QVector<QPair<qint16, qint16>> extractSamples(const QByteArray &rawBytes, QString *error = nullptr);
    static bool validateRawBytes(const QByteArray &rawBytes,
                                 CeyearIfIqRawSummary *summary = nullptr,
                                 QString *error = nullptr);
    static bool validateRawFile(const QString &rawFilePath,
                                CeyearIfIqRawSummary *summary = nullptr,
                                QString *error = nullptr);
    static bool validateRawFilePrefix(const QString &rawFilePath,
                                      CeyearIfIqRawSummary *summary = nullptr,
                                      QString *error = nullptr,
                                      qint64 maxBytes = 1024 * 1024);
    static bool convertRawFileToCap(const QString &rawFilePath,
                                    const UsrpCaptureConfig &capConfig,
                                    QString *error = nullptr);
};
