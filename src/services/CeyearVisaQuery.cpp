#include "services/CeyearVisaQuery.h"
#include "services/CeyearPscSweep.h"

#include <QLocale>

namespace {

QString integerText(double value)
{
    return QLocale::c().toString(value, 'f', 0);
}

QString megahertzText(double valueHz)
{
    QString value = QLocale::c().toString(valueHz / 1000000.0, 'f', 6);
    while (value.contains(QLatin1Char('.')) && value.endsWith(QLatin1Char('0'))) {
        value.chop(1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
        value.chop(1);
    }
    return value + QStringLiteral("MHz");
}

} // namespace

QString defaultCeyearVisaResource()
{
    return QStringLiteral("TCPIP::192.168.50.54::5025::SOCKET");
}

std::vector<CeyearVisaQuery> defaultCeyearStatusQueries()
{
    return {
        {"Identity", "*IDN?"},
        {"SCPI version", "SYSTem:VERSion?"},
        {"Measure mode", "MEASure:MODE?"},
        {"Center frequency", "FREQuency?"},
        {"Frequency span", "FREQuency:SPAN?"},
        {"Demodulation", "DEModulation?"},
        {"Detector", "DETector?"},
        {"Display window", "DISPlay:WINDow?"},
        {"Socket address", "SYSTem:COMMunicate:SOCKet:ADDRess?"},
        {"LAN subnet mask", "SYSTem:COMMunicate:LAN:SUBMask?"},
        {"LAN gateway", "SYSTem:COMMunicate:LAN:GATeway?"},
        {"System error", "SYSTem:ERRor?"},
    };
}

std::vector<CeyearVisaQuery> defaultCeyearIfIqProbeQueries()
{
    return {
        {"Identity", "*IDN?"},
        {"System error before probe", "SYSTem:ERRor?"},
        {"Frequency mode", "FREQuency:MODE?"},
        {"Center frequency", "FREQuency?"},
        {"Frequency span", "FREQuency:SPAN?"},
        {"Measure mode", "MEASure:MODE?"},
        {"Measure time", "MEASure:TIME?"},
        {"IF remote mode", "SYSTem:IF:REMote:MODE?"},
        {"IQ report mode", "TRACe:IQ:DATA:REPort?"},
        {"Record source", "TRACe:RECord:SOURce?"},
        {"Record selectivity", "TRACe:RECord:SELectivity?"},
        {"Record state", "TRACe:RECord:STATe?"},
        {"Report path", "TRACe:PATH?"},
        {"UDP report targets", "TRACe:UDP?"},
        {"TCP report targets", "TRACe:TCP?"},
        {"System error after probe", "SYSTem:ERRor?"},
    };
}

std::vector<CeyearVisaQuery> defaultCeyearControlCommands(const CeyearControlConfig &config)
{
    const auto number = [](double value) {
        return QLocale::c().toString(value, 'f', 0).toLatin1();
    };

    return {
        {"Write center frequency", QByteArray("FREQuency ") + number(config.centerFrequencyHz), false},
        {"Write frequency span", QByteArray("FREQuency:SPAN ") + number(config.frequencySpanHz), false},
        {"Write demodulation", QByteArray("DEModulation ") + config.demodulation.trimmed().toLatin1(), false},
        {"Write detector", QByteArray("DETector:FUNCtion ") + config.detector.trimmed().toLatin1(), false},
    };
}

std::vector<CeyearVisaQuery> defaultCeyearCaptureCommands(const CeyearCaptureConfig &config)
{
    std::vector<CeyearVisaQuery> commands;
    const QStringList lines = ceyearTriggerIqReportCommandLines(config);
    commands.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        commands.push_back({QByteArray("Trigger IQ report"), line.toLatin1(), false});
    }
    return commands;
}

std::vector<CeyearVisaQuery> preferredCeyearCaptureCommands(const CeyearCaptureConfig &config)
{
    return defaultCeyearCaptureCommands(config);
}

std::vector<CeyearVisaQuery> preferredCeyearStopCommands()
{
    return {
        {QByteArray("Delete UDP report paths"), QByteArray("TRACe:UDP:DEL ALL"), false},
        {QByteArray("Delete TCP report paths"), QByteArray("TRACe:TCP:DEL ALL"), false},
    };
}

std::vector<CeyearVisaQuery> defaultCeyearStopCommands()
{
    return {
        {QByteArray("Delete UDP report paths"), QByteArray("TRACe:UDP:DEL ALL"), false},
        {QByteArray("Delete TCP report paths"), QByteArray("TRACe:TCP:DEL ALL"), false},
    };
}

std::vector<CeyearVisaQuery> defaultCeyearCaptureStatusQueries()
{
    return {
        {"Record state", "TRACe:RECord:STATe?"},
        {"Frequency mode", "FREQuency:MODE?"},
        {"IF remote mode", "SYSTem:IF:REMote:MODE?"},
        {"Record source", "TRACe:RECord:SOURce?"},
        {"Record selectivity", "TRACe:RECord:SELectivity?"},
        {"Report path", "TRACe:PATH?"},
        {"IQ report mode", "TRACe:IQ:DATA:REPort?"},
        {"TCP report targets", "TRACe:TCP?"},
        {"UDP report targets", "TRACe:UDP?"},
        {"System error", "SYSTem:ERRor?"},
    };
}

QStringList ceyearCaptureSmokeCommandLines(const CeyearCaptureConfig &config)
{
    return ceyearLowRiskPathSetupCommandLines(config);
}

QStringList ceyearCaptureSmokeStopCommandLines()
{
    QStringList lines;
    for (const auto &command : preferredCeyearStopCommands()) {
        lines.append(QString::fromLatin1(command.command));
    }
    return lines;
}

QStringList ceyearLowRiskPathSetupCommandLines(const CeyearCaptureConfig &config)
{
    const QString protocol = config.pcReceiveProtocol.trimmed().toUpper();
    const QString tracePath = config.reportPath.trimmed().toUpper();
    return {
        QStringLiteral("TRACe:UDP:DEL ALL"),
        QStringLiteral("TRACe:TCP:DEL ALL"),
        QStringLiteral("TRACe:%1:TAG \"%2\",%3,IF")
            .arg(protocol, config.pcReceiveIpAddress.trimmed(), QString::number(config.pcReceivePort)),
        QStringLiteral("TRACe:%1:FLAG \"%2\",%3,\"opt\"")
            .arg(protocol, config.pcReceiveIpAddress.trimmed(), QString::number(config.pcReceivePort)),
        QStringLiteral("TRACe:PATH %1").arg(tracePath),
    };
}

QStringList ceyearRiskyStreamStartCommandLines(const CeyearCaptureConfig &config)
{
    QStringList lines{
        QStringLiteral("TRACe:RECord:STATe OFF"),
        QStringLiteral("TRACe:RECord:SELectivity REPort"),
        QStringLiteral("SYSTem:IF:REMote:MODE SHORT"),
        QStringLiteral("TRACe:IQ:DATA:REPort FREE"),
        QStringLiteral("TRIGger:IQ:REPort:STATe ON"),
    };
    if (config.sendInitiate) {
        lines.append(QStringLiteral("INITiate"));
    }
    return lines;
}

QStringList ceyearManualIqReportCommandLines(const CeyearCaptureConfig &config)
{
    return ceyearTriggerIqReportCommandLines(config);
}

QStringList ceyearTriggerIqReportCommandLines(const CeyearCaptureConfig &config)
{
    const QString protocol = config.pcReceiveProtocol.trimmed().toUpper();
    const QString tracePath = config.reportPath.trimmed().toUpper();
    QStringList lines{
        QStringLiteral("TRACe:UDP:DEL ALL"),
        QStringLiteral("TRACe:TCP:DEL ALL"),
        QStringLiteral("FREQuency:MODE FFM"),
        QStringLiteral("INPut:ATTenuation:STATe AUTO"),
        QStringLiteral("TRACe:IQ:DATA:REPort MAN"),
        QStringLiteral("TRACe:RECord:SOURce %1").arg(config.recordSource.trimmed().isEmpty()
                                                        ? QStringLiteral("IQAudio")
                                                        : config.recordSource.trimmed()),
        QStringLiteral("FREQuency %1").arg(megahertzText(config.centerFrequencyHz)),
        QStringLiteral("BANDwidth %1").arg(megahertzText(config.frequencySpanHz)),
        QStringLiteral("SYSTem:IF:REMote:MODE SHORT"),
    };
    if (config.iqPointCount > 0) {
        lines.append(QStringLiteral("TRACe:IQ:DATA:NUMBer %1").arg(config.iqPointCount));
    }
    lines.append(QStringLiteral("TRACe:PATH %1").arg(tracePath));
    lines.append(QStringLiteral("TRACe:%1:FLAG \"%2\",%3,\"opt\"")
                     .arg(protocol, config.pcReceiveIpAddress.trimmed(), QString::number(config.pcReceivePort)));
    lines.append(QStringLiteral("TRACe:%1:TAG \"%2\",%3,IF")
                     .arg(protocol, config.pcReceiveIpAddress.trimmed(), QString::number(config.pcReceivePort)));
    lines.append(QStringLiteral("TRIGger:IQ:REPort:STATe ON"));
    return lines;
}

QStringList ceyearTriggerIqReportStopCommandLines()
{
    return {
        QStringLiteral("TRACe:UDP:DEL ALL"),
        QStringLiteral("TRACe:TCP:DEL ALL"),
    };
}

std::vector<CeyearVisaQuery> pscSweepStartQueries(const CeyearPscSweepConfig &config)
{
    const QStringList lines = ceyearPscSweepCommandLines(config);
    std::vector<CeyearVisaQuery> queries;
    queries.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        queries.push_back({QByteArray("Start PSC sweep"), line.toLatin1(), false});
    }
    return queries;
}

std::vector<CeyearVisaQuery> pscSweepStopQueries()
{
    const QStringList lines = ceyearPscSweepStopCommandLines();
    std::vector<CeyearVisaQuery> queries;
    queries.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        queries.push_back({QByteArray("Stop PSC sweep"), line.toLatin1(), false});
    }
    return queries;
}

bool ceyearCaptureSmokeRequiresRiskAcknowledgement(const CeyearCaptureConfig &config,
                                                   bool execute,
                                                   bool allowRiskyInitiate)
{
    return execute && config.sendInitiate && !allowRiskyInitiate;
}

bool ceyearCaptureSmokeRequiresFreeRunAcknowledgement(const CeyearCaptureConfig &config,
                                                      bool execute,
                                                      bool allowRiskyFreeRun)
{
    return ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config,
                                                            execute,
                                                            allowRiskyFreeRun,
                                                            false);
}

bool ceyearCaptureSmokeRequiresFreeRunAcknowledgement(const CeyearCaptureConfig &config,
                                                      bool execute,
                                                      bool allowRiskyFreeRun,
                                                      bool configurePathOnly)
{
    Q_UNUSED(config)
    return execute && !configurePathOnly && !allowRiskyFreeRun;
}

bool ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(bool execute,
                                                             bool manualIqReport,
                                                             bool allowManualIqReport)
{
    return execute && manualIqReport && !allowManualIqReport;
}

bool ceyearCaptureSmokeRequiresTriggerIqReportAcknowledgement(bool execute,
                                                              bool triggerIqReport,
                                                              bool allowTriggerIqReport)
{
    return execute && triggerIqReport && !allowTriggerIqReport;
}

bool ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(bool execute,
                                                                    bool usesIfRemoteShort,
                                                                    bool allowRebootObservedIfShort)
{
    return execute && usesIfRemoteShort && !allowRebootObservedIfShort;
}

bool ceyearCaptureSmokeRequiresReceivedBytes(bool execute, bool configurePathOnly)
{
    return execute && !configurePathOnly;
}
