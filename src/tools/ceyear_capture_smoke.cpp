#include "app/ProjectPaths.h"
#include "services/CeyearVisaService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

namespace
{
QString valueAfter(const QStringList &args, const QString &name, const QString &fallback)
{
    const int index = args.indexOf(name);
    if (index >= 0 && index + 1 < args.size()) {
        return args.at(index + 1);
    }
    return fallback;
}

int intAfter(const QStringList &args, const QString &name, int fallback)
{
    bool ok = false;
    const int value = valueAfter(args, name, QString::number(fallback)).toInt(&ok);
    return ok ? value : fallback;
}

double doubleAfter(const QStringList &args, const QString &name, double fallback)
{
    bool ok = false;
    const double value = valueAfter(args, name, QString::number(fallback)).toDouble(&ok);
    return ok ? value : fallback;
}

QString defaultOutputPath()
{
    const QString rawDir = QDir(ProjectPaths::dataDirectory()).filePath(QStringLiteral("raw"));
    QDir().mkpath(rawDir);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    return QDir(rawDir).filePath(QStringLiteral("3943b_smoke_%1.iqraw").arg(stamp));
}

QString protocolOutputPath(const QString &baseOutputPath, const QString &protocol)
{
    const QFileInfo info(baseOutputPath);
    const QString suffix = QStringLiteral("_%1").arg(protocol.trimmed().toLower());
    return info.dir().filePath(info.completeBaseName() + suffix + QStringLiteral(".") + info.completeSuffix());
}

CeyearCaptureResult runCaptureAttempt(const QString &resource, const CeyearCaptureConfig &config)
{
    CeyearVisaService service;
    return service.startCapture(resource, config);
}

CeyearCaptureResult runPathConfiguration(const QString &resource, const CeyearCaptureConfig &config)
{
    CeyearVisaService service;
    return service.configureCapturePath(resource, config);
}

CeyearCaptureResult runManualIqReport(const QString &resource, const CeyearCaptureConfig &config)
{
    CeyearVisaService service;
    return service.startManualIqReport(resource, config);
}

CeyearCaptureResult runTriggerIqReport(const QString &resource, const CeyearCaptureConfig &config)
{
    CeyearVisaService service;
    return service.triggerIqReport(resource, config);
}

void printCommandLines(QTextStream &out, const QString &title, const QStringList &lines)
{
    out << title << "\n";
    for (const QString &line : lines) {
        out << "SCPI: " << line << "\n";
    }
}

QStringList riskyCaptureCommandLines(const CeyearCaptureConfig &config)
{
    QStringList lines;
    for (const auto &command : preferredCeyearCaptureCommands(config)) {
        lines.append(QString::fromLatin1(command.command));
    }
    return lines;
}

void printUsage(QTextStream &out)
{
    out << "Usage: ceyear_capture_smoke [options]\n"
        << "Options:\n"
        << "  --resource <visa>      Default: " << defaultCeyearVisaResource() << "\n"
        << "  --pc-ip <address>      Default: 192.168.50.20\n"
        << "  --pc-port <port>       Default: 5030\n"
        << "  --protocol <TCP|UDP|AUTO>   Default: UDP\n"
        << "  --output <path>        Default: data/raw/3943b_smoke_<timestamp>.iqraw\n"
        << "  --freq-hz <hz>         Center frequency. Default: 2400000000\n"
        << "  --span-hz <hz>         Demodulation bandwidth. Default: 20000000\n"
        << "  --points <count>       IQ sample points requested from 3943B. Default: 1000000\n"
        << "  --timeout-ms <ms>      PC receive timeout. Default: 5000\n"
        << "  --initiate             Append INITiate after IF stream setup; risky on observed 3943B V2.1.0\n"
        << "  --allow-risky-initiate Required with --execute --initiate after external device-side confirmation\n"
        << "  --allow-risky-free-run Required before sending IF remote SHORT, FREE report, and TRIGger:IQ:REPort:STATe ON\n"
        << "  --configure-path-only  With --execute, only write TRACe:<protocol>:TAG/FLAG and TRACe:PATH\n"
        << "  --manual-iq-report     Send the vendor MAN IQ network report sequence with IF remote SHORT\n"
        << "  --allow-manual-iq-report Required with --execute --manual-iq-report after vendor/front-panel confirmation\n"
        << "  --trigger-iq-report    Send the vendor MAN IQ report sequence with IF remote SHORT, without FREE or INITiate\n"
        << "  --allow-trigger-iq-report Required with --execute --trigger-iq-report after receiver/Wireshark is ready\n"
        << "  --allow-reboot-observed-if-short Required for any live SYSTem:IF:REMote:MODE SHORT path; observed to reboot this 3943B\n"
        << "  --dry-run              Print SCPI only; this is the default\n"
        << "  --execute              Actually start the PC receiver and write SCPI to 3943B\n"
        << "  --trace-scpi           Print start/stop SCPI before execution\n";
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        printUsage(out);
        return 0;
    }

    CeyearCaptureConfig config;
    config.centerFrequencyHz = doubleAfter(args, QStringLiteral("--freq-hz"), 2400000000.0);
    config.frequencySpanHz = doubleAfter(args, QStringLiteral("--span-hz"), 20000000.0);
    config.pcReceiveIpAddress = valueAfter(args, QStringLiteral("--pc-ip"), QStringLiteral("192.168.50.20"));
    config.pcReceivePort = intAfter(args, QStringLiteral("--pc-port"), 5030);
    config.outputFilePath = valueAfter(args, QStringLiteral("--output"), defaultOutputPath());
    config.iqPointCount = intAfter(args, QStringLiteral("--points"), 1000000);
    config.measurementTimeSeconds = 0.0;
    config.pcReceiveTimeoutMs = intAfter(args, QStringLiteral("--timeout-ms"), 5000);
    config.sendInitiate = args.contains(QStringLiteral("--initiate"));
    config.recordEnabled = true;
    config.recordSource = QStringLiteral("IQAudio");
    const QString protocolArg = valueAfter(args, QStringLiteral("--protocol"), QStringLiteral("UDP")).trimmed().toUpper();
    const QStringList protocols = protocolArg == QStringLiteral("AUTO")
        ? QStringList{QStringLiteral("TCP"), QStringLiteral("UDP")}
        : QStringList{protocolArg};

    const QString resource = valueAfter(args, QStringLiteral("--resource"), defaultCeyearVisaResource());
    const bool execute = args.contains(QStringLiteral("--execute"));
    const bool allowRiskyInitiate = args.contains(QStringLiteral("--allow-risky-initiate"));
    const bool allowRiskyFreeRun = args.contains(QStringLiteral("--allow-risky-free-run"));
    const bool configurePathOnly = args.contains(QStringLiteral("--configure-path-only"));
    const bool manualIqReport = args.contains(QStringLiteral("--manual-iq-report"));
    const bool allowManualIqReport = args.contains(QStringLiteral("--allow-manual-iq-report"));
    const bool triggerIqReport = args.contains(QStringLiteral("--trigger-iq-report"));
    const bool allowTriggerIqReport = args.contains(QStringLiteral("--allow-trigger-iq-report"));
    const bool allowRebootObservedIfShort = args.contains(QStringLiteral("--allow-reboot-observed-if-short"));
    const bool traceScpi = args.contains(QStringLiteral("--trace-scpi")) || !execute || args.contains(QStringLiteral("--dry-run"));

    out << "RESOURCE: " << resource << "\n"
        << "PC_TARGET: " << protocolArg << " "
        << config.pcReceiveIpAddress << ":" << config.pcReceivePort << "\n"
        << "OUTPUT: " << config.outputFilePath << "\n"
        << "FREQ_HZ: " << QString::number(config.centerFrequencyHz, 'f', 0) << "\n"
        << "SPAN_HZ: " << QString::number(config.frequencySpanHz, 'f', 0) << "\n"
        << "IQ_POINTS: " << config.iqPointCount << "\n"
        << "TIMEOUT_MS: " << config.pcReceiveTimeoutMs << "\n"
        << "MODE: " << (execute ? "EXECUTE" : "DRY_RUN") << "\n";
    out.flush();

    if (ceyearCaptureSmokeRequiresRiskAcknowledgement(config, execute, allowRiskyInitiate)) {
        err << "Refusing to execute --initiate without --allow-risky-initiate. "
            << "This IF stream initiation combination has caused observed 3943B V2.1.0 reboots.\n";
        return 2;
    }
    if (ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config,
                                                         execute,
                                                         allowRiskyFreeRun,
                                                         configurePathOnly || manualIqReport || triggerIqReport)) {
        err << "Refusing to execute IF/IQ FREE-run stream setup without --allow-risky-free-run. "
            << "Use --probe-if-iq first and confirm front-panel IF/IQ prerequisites.\n";
        return 2;
    }
    if (ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(execute, manualIqReport, allowManualIqReport)) {
        err << "Refusing to execute --manual-iq-report without --allow-manual-iq-report. "
            << "Confirm front-panel IF/IQ prerequisites and keep Wireshark/receiver ready first.\n";
        return 2;
    }
    if (ceyearCaptureSmokeRequiresTriggerIqReportAcknowledgement(execute, triggerIqReport, allowTriggerIqReport)) {
        err << "Refusing to execute --trigger-iq-report without --allow-trigger-iq-report. "
            << "Keep PC receiver/Wireshark ready first; this is the minimal live IQ report trigger.\n";
        return 2;
    }
    if (ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(execute,
                                                                      manualIqReport || triggerIqReport || (!configurePathOnly && allowRiskyFreeRun),
                                                                       allowRebootObservedIfShort)) {
        err << "Refusing to execute SYSTem:IF:REMote:MODE SHORT path without --allow-reboot-observed-if-short. "
            << "This path intentionally uses IF remote SHORT; confirm device state and keep the PC receiver ready first.\n";
        return 2;
    }

    CeyearCaptureResult finalResult;
    for (const QString &protocol : protocols) {
        config.pcReceiveProtocol = protocol;
        config.reportPath = protocol;
        config.outputFilePath = protocols.size() > 1
            ? protocolOutputPath(valueAfter(args, QStringLiteral("--output"), defaultOutputPath()), protocol)
            : valueAfter(args, QStringLiteral("--output"), defaultOutputPath());

        out << "\nTRY: " << protocol << "\n"
            << "OUTPUT: " << config.outputFilePath << "\n";
        if (traceScpi) {
            QString traceTitle = QStringLiteral("PATH_SETUP_SCPI:");
            QStringList traceLines = ceyearCaptureSmokeCommandLines(config);
            if (manualIqReport) {
                traceTitle = QStringLiteral("MANUAL_IQ_REPORT_SCPI:");
                traceLines = ceyearManualIqReportCommandLines(config);
            } else if (triggerIqReport) {
                traceTitle = QStringLiteral("TRIGGER_IQ_REPORT_SCPI:");
                traceLines = ceyearTriggerIqReportCommandLines(config);
            } else if (allowRiskyFreeRun && !configurePathOnly) {
                traceTitle = QStringLiteral("RISKY_START_SCPI:");
                traceLines = riskyCaptureCommandLines(config);
            }
            printCommandLines(out, traceTitle, traceLines);
            if (triggerIqReport) {
                printCommandLines(out, QStringLiteral("STOP_SCPI:"), ceyearTriggerIqReportStopCommandLines());
            } else if (!configurePathOnly) {
                printCommandLines(out, QStringLiteral("STOP_SCPI:"), ceyearCaptureSmokeStopCommandLines());
            }
        }
        out.flush();

        if (!execute) {
            finalResult.success = true;
            finalResult.message = QStringLiteral("Dry run only. Re-run with --execute to write SCPI to 3943B.");
            finalResult.outputFilePath = config.outputFilePath;
            continue;
        }

        CeyearCaptureResult result = configurePathOnly
            ? runPathConfiguration(resource, config)
            : manualIqReport
            ? runManualIqReport(resource, config)
            : (triggerIqReport || !allowRiskyFreeRun)
            ? runTriggerIqReport(resource, config)
            : runCaptureAttempt(resource, config);

        out << "SUCCESS: " << (result.success ? "true" : "false") << "\n"
            << "MESSAGE: " << result.message << "\n"
            << "BYTES_RECEIVED: " << result.bytesReceived << "\n"
            << "SAVED_FILE: " << result.outputFilePath << "\n";

        for (const QString &log : result.logs) {
            out << "LOG: " << log << "\n";
        }
        for (const auto &item : result.items) {
            out << "STATUS: " << item.label << " [" << item.command << "] = ";
            if (item.success) {
                out << item.response << "\n";
            } else {
                out << "ERROR " << item.error << "\n";
            }
        }

        finalResult = result;
        if (result.success && result.bytesReceived > 0) {
            break;
        }
    }

    if (!execute) {
        out << "SUCCESS: true\n"
            << "MESSAGE: " << finalResult.message << "\n"
            << "BYTES_RECEIVED: 0\n"
            << "SAVED_FILE: " << finalResult.outputFilePath << "\n";
        return 0;
    }

    if (!finalResult.success
        || (ceyearCaptureSmokeRequiresReceivedBytes(execute, configurePathOnly) && finalResult.bytesReceived <= 0)) {
        err << "Capture smoke failed: " << finalResult.message << "\n";
        return 1;
    }
    return 0;
}
