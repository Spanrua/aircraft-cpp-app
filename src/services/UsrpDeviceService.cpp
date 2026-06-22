#include "services/UsrpDeviceService.h"

#include <uhd/stream.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <vector>

namespace
{
constexpr int kCapHeaderLength = 0x200;
constexpr quint32 kDefaultBlockSize = 1024;

void writeDoubleBE(std::array<char, kCapHeaderLength> &buffer, int offset, double value)
{
    static_assert(sizeof(double) == sizeof(quint64));
    quint64 raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    for (int i = 0; i < 8 && offset + i < static_cast<int>(buffer.size()); ++i) {
        buffer[static_cast<size_t>(offset + i)] = static_cast<char>((raw >> ((7 - i) * 8)) & 0xFF);
    }
}

void writeUInt32BE(std::array<char, kCapHeaderLength> &buffer, int offset, quint32 value)
{
    for (int i = 0; i < 4 && offset + i < static_cast<int>(buffer.size()); ++i) {
        buffer[static_cast<size_t>(offset + i)] = static_cast<char>((value >> ((3 - i) * 8)) & 0xFF);
    }
}

QString uhdError(const std::exception &ex)
{
    return QString::fromLocal8Bit(ex.what());
}

QString defaultOutputPath()
{
    const QString dir = QDir::current().absoluteFilePath(QStringLiteral("captures"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("usrp_%1.cap").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
}

} // namespace

QString UsrpDeviceInfo::displayName() const
{
    QStringList parts;
    if (!product.isEmpty()) {
        parts << product;
    }
    if (!name.isEmpty()) {
        parts << name;
    }
    if (!serial.isEmpty()) {
        parts << QStringLiteral("SN %1").arg(serial);
    }
    return parts.isEmpty() ? QStringLiteral("USRP Device") : parts.join(QStringLiteral(" / "));
}

QVector<UsrpDeviceInfo> UsrpDeviceService::discover(QString *error) const
{
    QVector<UsrpDeviceInfo> devices;
    try {
        const uhd::device_addrs_t found = uhd::device::find(uhd::device_addr_t());
        for (const auto &addr : found) {
            UsrpDeviceInfo info;
            info.serial = QString::fromStdString(addr.get("serial", ""));
            info.name = QString::fromStdString(addr.get("name", ""));
            info.product = QString::fromStdString(addr.get("product", ""));
            info.type = QString::fromStdString(addr.get("type", ""));
            devices.append(info);
        }
    } catch (const std::exception &ex) {
        if (error) {
            *error = QStringLiteral("UHD 设备发现失败：%1").arg(uhdError(ex));
        }
    }
    return devices;
}

UsrpCaptureResult UsrpDeviceService::captureToCap(const UsrpCaptureConfig &config) const
{
    UsrpCaptureResult result;
    UsrpCaptureConfig effective = config;
    if (effective.outputFilePath.isEmpty()) {
        effective.outputFilePath = defaultOutputPath();
    }

    try {
        result.logs << QStringLiteral("创建 USRP：%1").arg(effective.deviceArgs.isEmpty() ? QStringLiteral("自动发现") : effective.deviceArgs);
        auto usrp = uhd::usrp::multi_usrp::make(effective.deviceArgs.toStdString());

        const size_t channel = static_cast<size_t>(qMax(0, effective.channel));
        usrp->set_rx_rate(effective.sampleRateHz, channel);
        usrp->set_rx_freq(uhd::tune_request_t(effective.centerFrequencyHz), channel);
        usrp->set_rx_gain(effective.gainDb, channel);
        if (!effective.antenna.isEmpty()) {
            usrp->set_rx_antenna(effective.antenna.toStdString(), channel);
        }
        if (effective.bandwidthHz > 0.0) {
            usrp->set_rx_bandwidth(effective.bandwidthHz, channel);
        }

        const QString cpuFormat = effective.cpuFormat.trimmed().isEmpty() ? QStringLiteral("sc16") : effective.cpuFormat.trimmed();
        const QString wireFormat = effective.wireFormat.trimmed().isEmpty() ? QStringLiteral("sc16") : effective.wireFormat.trimmed();
        result.logs << QStringLiteral("接收通道：%1").arg(channel);
        result.logs << QStringLiteral("实际采样率：%1 MSps").arg(usrp->get_rx_rate(channel) / 1000000.0, 0, 'f', 3);
        result.logs << QStringLiteral("实际中心频率：%1 MHz").arg(usrp->get_rx_freq(channel) / 1000000.0, 0, 'f', 3);
        result.logs << QStringLiteral("实际增益：%1 dB").arg(usrp->get_rx_gain(channel), 0, 'f', 1);
        result.logs << QStringLiteral("接收天线：%1").arg(effective.antenna.isEmpty() ? QStringLiteral("设备默认") : effective.antenna);
        result.logs << QStringLiteral("采集格式：%1/%2").arg(cpuFormat, wireFormat);

        uhd::stream_args_t streamArgs(cpuFormat.toStdString(), wireFormat.toStdString());
        streamArgs.channels = {channel};
        auto rxStream = usrp->get_rx_stream(streamArgs);
        const size_t maxSamples = rxStream->get_max_num_samps();
        std::vector<UsrpSc16Sample> buffer(maxSamples);
        const int targetSamples = qMax(1, static_cast<int>(effective.sampleRateHz * effective.durationSeconds));
        std::vector<UsrpSc16Sample> samples(static_cast<size_t>(targetSamples));

        uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
        command.num_samps = static_cast<size_t>(targetSamples);
        command.stream_now = true;
        rxStream->issue_stream_cmd(command);

        uhd::rx_metadata_t metadata;
        size_t receivedTotal = 0;
        while (receivedTotal < static_cast<size_t>(targetSamples)) {
            const size_t wanted = std::min(maxSamples, static_cast<size_t>(targetSamples) - receivedTotal);
            std::vector<void *> buffers{buffer.data()};
            const size_t received = rxStream->recv(buffers, wanted, metadata, 3.0);
            if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                result.message = QStringLiteral("USRP 采集超时");
                return result;
            }
            if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                result.message = QStringLiteral("USRP 采集错误：%1").arg(QString::fromStdString(metadata.strerror()));
                return result;
            }
            std::copy_n(buffer.begin(), static_cast<std::ptrdiff_t>(received), samples.begin() + static_cast<std::ptrdiff_t>(receivedTotal));
            receivedTotal += received;
        }

        QString error;
        if (!writeCapFileFromSc16(effective, samples, &error)) {
            result.message = error;
            return result;
        }

        result.success = true;
        result.message = QStringLiteral("采集完成");
        result.outputFilePath = effective.outputFilePath;
        result.sampleCount = samples.size();
        result.fileSize = QFileInfo(effective.outputFilePath).size();
        result.logs << QStringLiteral("写入 CAP：%1").arg(effective.outputFilePath);
    } catch (const std::exception &ex) {
        result.message = QStringLiteral("USRP 采集失败：%1").arg(uhdError(ex));
    }
    return result;
}

bool UsrpDeviceService::writeCapFile(const UsrpCaptureConfig &config, const QVector<QPair<qint16, qint16>> &samples, QString *error)
{
    const QString parentDir = QDir::cleanPath(config.outputFilePath + QStringLiteral("/.."));
    if (!QDir().mkpath(parentDir)) {
        if (error) {
            *error = QStringLiteral("无法创建采集目录：%1").arg(parentDir);
        }
        return false;
    }

    QFile file(config.outputFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    std::array<char, kCapHeaderLength> header{};
    constexpr std::array<char, 7> version{'B', '.', '0', '1', '.', '0', '0'};
    std::copy(version.begin(), version.end(), header.begin());
    writeDoubleBE(header, 0x0010, config.bandwidthHz);
    writeDoubleBE(header, 0x0018, config.centerFrequencyHz);
    writeUInt32BE(header, 0x0110, static_cast<quint32>(samples.size()));
    writeUInt32BE(header, 0x0114, kDefaultBlockSize);
    if (file.write(header.data(), static_cast<qint64>(header.size())) != static_cast<qint64>(header.size())) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    for (const auto &sample : samples) {
        const std::array<char, 4> iqBytes{
            static_cast<char>((static_cast<quint16>(sample.first) >> 8) & 0xFF),
            static_cast<char>(static_cast<quint16>(sample.first) & 0xFF),
            static_cast<char>((static_cast<quint16>(sample.second) >> 8) & 0xFF),
            static_cast<char>(static_cast<quint16>(sample.second) & 0xFF),
        };
        if (file.write(iqBytes.data(), static_cast<qint64>(iqBytes.size())) != static_cast<qint64>(iqBytes.size())) {
            if (error) {
                *error = file.errorString();
            }
            return false;
        }
    }
    return true;
}

bool UsrpDeviceService::writeCapFileFromSc16(const UsrpCaptureConfig &config, const std::vector<UsrpSc16Sample> &samples, QString *error)
{
    QVector<QPair<qint16, qint16>> pairs;
    pairs.reserve(static_cast<qsizetype>(samples.size()));
    for (const auto &sample : samples) {
        pairs.append({sample.i, sample.q});
    }
    return writeCapFile(config, pairs, error);
}
