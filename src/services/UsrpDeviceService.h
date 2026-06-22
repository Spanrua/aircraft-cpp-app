#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

struct UsrpDeviceInfo
{
    QString serial;
    QString name;
    QString product;
    QString type;
    QString displayName() const;
};

struct UsrpCaptureConfig
{
    QString deviceArgs;
    QString antenna = QStringLiteral("TX/RX");
    int channel = 0;
    double centerFrequencyHz = 1090000000.0;
    double sampleRateHz = 12800000.0;
    double bandwidthHz = 10000000.0;
    double gainDb = 30.0;
    double durationSeconds = 3.0;
    QString cpuFormat = QStringLiteral("sc16");
    QString wireFormat = QStringLiteral("sc16");
    QString outputFilePath;
};

struct UsrpCaptureResult
{
    bool success = false;
    QString message;
    QString outputFilePath;
    int sampleCount = 0;
    qint64 fileSize = 0;
    QStringList logs;
};

struct UsrpSc16Sample
{
    qint16 i = 0;
    qint16 q = 0;
};

class IUsrpDeviceService
{
public:
    virtual ~IUsrpDeviceService() = default;

    virtual QVector<UsrpDeviceInfo> discover(QString *error = nullptr) const = 0;
    virtual UsrpCaptureResult captureToCap(const UsrpCaptureConfig &config) const = 0;
};

class UsrpDeviceService final : public IUsrpDeviceService
{
public:
    QVector<UsrpDeviceInfo> discover(QString *error = nullptr) const override;
    UsrpCaptureResult captureToCap(const UsrpCaptureConfig &config) const override;

    static bool writeCapFile(
        const UsrpCaptureConfig &config,
        const QVector<QPair<qint16, qint16>> &samples,
        QString *error = nullptr);
    static bool writeCapFileFromSc16(
        const UsrpCaptureConfig &config,
        const std::vector<UsrpSc16Sample> &samples,
        QString *error = nullptr);
};
