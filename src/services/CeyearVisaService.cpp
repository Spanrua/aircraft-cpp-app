#include "services/CeyearVisaService.h"
#include "services/CeyearIfIqPacketParser.h"
#include "services/CeyearIqReceiver.h"

#include <visa.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QLocale>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QStringList>
#include <QThread>

namespace
{
bool iqSampleCountAcceptable(qint64 sampleCount, int targetSampleCount)
{
    Q_UNUSED(targetSampleCount)
    return sampleCount > 0;
}

QString iqShortfallText(qint64 sampleCount, int targetSampleCount)
{
    if (targetSampleCount <= 0) {
        return {};
    }
    const qint64 missing = static_cast<qint64>(targetSampleCount) - sampleCount;
    if (sampleCount <= 0) {
        return QStringLiteral("PC IQ receiver received no IQ samples.");
    }
    if (missing <= 0) {
        return QStringLiteral("PC IQ receiver reached target sample count.");
    }
    return QStringLiteral("PC IQ receiver accepted partial capture, missing %1 samples (%2 / %3).")
        .arg(missing)
        .arg(sampleCount)
        .arg(targetSampleCount);
}

void ceyearVisaDiagLog(const QString &line)
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);

    const QString appDir = QCoreApplication::applicationDirPath().isEmpty()
        ? QDir::currentPath()
        : QCoreApplication::applicationDirPath();
    const QString dirPath = QDir(appDir).filePath(QStringLiteral("logs"));
    QDir().mkpath(dirPath);

    QFile file(QDir(dirPath).filePath(QStringLiteral("aircraft_qt_debug.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString threadId = QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16);
    file.write(QStringLiteral("[%1][thread %2][CeyearVisaService] %3\n").arg(stamp, threadId, line).toUtf8());
    file.flush();
}

QMutex &activeIqReceiverMutex()
{
    static QMutex mutex;
    return mutex;
}

QPointer<CeyearIqReceiver> &activeIqReceiver()
{
    static QPointer<CeyearIqReceiver> receiver;
    return receiver;
}

bool &activeIqStopRequested()
{
    static bool requested = false;
    return requested;
}

void requestActiveIqReceiverStop()
{
    QPointer<CeyearIqReceiver> receiver;
    {
        QMutexLocker locker(&activeIqReceiverMutex());
        activeIqStopRequested() = true;
        receiver = activeIqReceiver();
    }
    if (receiver) {
        QMetaObject::invokeMethod(receiver.data(), "cancel", Qt::QueuedConnection);
    }
}

void registerActiveIqReceiver(CeyearIqReceiver *receiver)
{
    bool shouldStop = false;
    {
        QMutexLocker locker(&activeIqReceiverMutex());
        activeIqReceiver() = receiver;
        shouldStop = activeIqStopRequested();
    }
    if (shouldStop && receiver) {
        QMetaObject::invokeMethod(receiver, "cancel", Qt::QueuedConnection);
    }
}

void unregisterActiveIqReceiver(CeyearIqReceiver *receiver)
{
    QMutexLocker locker(&activeIqReceiverMutex());
    if (activeIqReceiver() == receiver) {
        activeIqReceiver().clear();
        activeIqStopRequested() = false;
    }
}

class ActiveIqReceiverRegistration
{
public:
    explicit ActiveIqReceiverRegistration(CeyearIqReceiver *receiver)
        : m_receiver(receiver)
    {
        registerActiveIqReceiver(m_receiver);
    }

    ~ActiveIqReceiverRegistration()
    {
        unregisterActiveIqReceiver(m_receiver);
    }

    Q_DISABLE_COPY(ActiveIqReceiverRegistration)

private:
    CeyearIqReceiver *m_receiver = nullptr;
};

struct VisaApi
{
    using OpenDefaultRM = ViStatus(_VI_FUNC *)(ViPSession);
    using Open = ViStatus(_VI_FUNC *)(ViSession, ViRsrc, ViAccessMode, ViUInt32, ViPSession);
    using SetAttribute = ViStatus(_VI_FUNC *)(ViObject, ViAttr, ViAttrState);
    using Write = ViStatus(_VI_FUNC *)(ViSession, ViConstBuf, ViUInt32, ViPUInt32);
    using Read = ViStatus(_VI_FUNC *)(ViSession, ViPBuf, ViUInt32, ViPUInt32);
    using Close = ViStatus(_VI_FUNC *)(ViObject);

    QLibrary library;
    OpenDefaultRM openDefaultRM = nullptr;
    Open open = nullptr;
    SetAttribute setAttribute = nullptr;
    Write write = nullptr;
    Read read = nullptr;
    Close close = nullptr;

    bool load(QString *error)
    {
        ceyearVisaDiagLog(QStringLiteral("VisaApi::load enter"));
        QStringList candidates;
        const QByteArray configured = qgetenv("AIRCRAFT_VISA_LIBRARY");
        if (!configured.isEmpty()) {
            candidates.append(QString::fromLocal8Bit(configured));
        }
#ifdef Q_OS_WIN
        candidates << QStringLiteral("visa64")
                   << QStringLiteral("visa64.dll")
                   << QStringLiteral("C:/Program Files/IVI Foundation/VISA/Win64/Bin/visa64.dll");
#else
        candidates << QStringLiteral("visa")
                   << QStringLiteral("nivisa")
                   << QStringLiteral("libvisa.so")
                   << QStringLiteral("libnivisa.so");
#endif

        QString lastError;
        for (const QString &candidate : candidates) {
            library.setFileName(candidate);
            ceyearVisaDiagLog(QStringLiteral("VisaApi::load before library.load %1").arg(candidate));
            if (library.load()) {
                break;
            }
            lastError = library.errorString();
            ceyearVisaDiagLog(QStringLiteral("VisaApi::load %1 failed: %2").arg(candidate, lastError));
        }
        if (!library.isLoaded()) {
            if (error) {
                *error = QStringLiteral("无法加载 VISA 动态库：%1").arg(lastError);
            }
            return false;
        }
        ceyearVisaDiagLog(QStringLiteral("VisaApi::load library loaded: %1").arg(library.fileName()));

        openDefaultRM = resolve<OpenDefaultRM>("NI_viOpenDefaultRM", "viOpenDefaultRM");
        open = resolve<Open>("NI_viOpen", "viOpen");
        setAttribute = resolve<SetAttribute>("NI_viSetAttribute", "viSetAttribute");
        write = resolve<Write>("NI_viWrite", "viWrite");
        read = resolve<Read>("NI_viRead", "viRead");
        close = resolve<Close>("NI_viClose", "viClose");
        ceyearVisaDiagLog(QStringLiteral("VisaApi::load resolved symbols openDefaultRM=%1 open=%2 setAttribute=%3 write=%4 read=%5 close=%6")
                              .arg(openDefaultRM ? QStringLiteral("yes") : QStringLiteral("no"),
                                   open ? QStringLiteral("yes") : QStringLiteral("no"),
                                   setAttribute ? QStringLiteral("yes") : QStringLiteral("no"),
                                   write ? QStringLiteral("yes") : QStringLiteral("no"),
                                   read ? QStringLiteral("yes") : QStringLiteral("no"),
                                   close ? QStringLiteral("yes") : QStringLiteral("no")));

        if (!openDefaultRM || !open || !setAttribute || !write || !read || !close) {
            if (error) {
                *error = QStringLiteral("VISA 动态库缺少必要的 VISA 导出。");
            }
            return false;
        }
        ceyearVisaDiagLog(QStringLiteral("VisaApi::load ok"));
        return true;
    }

private:
    template <typename T>
    T resolve(const char *primary, const char *fallback)
    {
        if (auto symbol = library.resolve(primary)) {
            return reinterpret_cast<T>(symbol);
        }
        if (auto symbol = library.resolve(fallback)) {
            return reinterpret_cast<T>(symbol);
        }
        return nullptr;
    }
};

QString visaStatusText(ViStatus status)
{
    return QStringLiteral("VISA status %1").arg(static_cast<qint64>(status));
}

ViStatus writeCommand(const VisaApi &api, ViSession device, const QByteArray &command)
{
    QByteArray bytes(command);
    if (bytes.isEmpty() || bytes.back() != '\n') {
        bytes.append('\n');
    }
    ViUInt32 written = 0;
    return api.write(device,
                     reinterpret_cast<ViConstBuf>(bytes.constData()),
                     static_cast<ViUInt32>(bytes.size()),
                     &written);
}

ViStatus queryCommand(const VisaApi &api, ViSession device, const QByteArray &command, QString *response)
{
    const ViStatus writeStatus = writeCommand(api, device, command);
    if (writeStatus < VI_SUCCESS) {
        return writeStatus;
    }

    QByteArray bytes;
    char buffer[1024] = {};
    ViUInt32 readCount = 0;
    ViStatus readStatus = VI_SUCCESS;
    do {
        readCount = 0;
        readStatus = api.read(device,
                              reinterpret_cast<ViPBuf>(buffer),
                              static_cast<ViUInt32>(sizeof(buffer) - 1),
                              &readCount);
        if (readCount > 0) {
            bytes.append(buffer, static_cast<qsizetype>(readCount));
        }
    } while (readStatus == VI_SUCCESS_MAX_CNT);

    if (readStatus < VI_SUCCESS && bytes.isEmpty()) {
        return readStatus;
    }

    while (!bytes.isEmpty() && (bytes.back() == '\n' || bytes.back() == '\r')) {
        bytes.chop(1);
    }
    if (response) {
        *response = QString::fromLocal8Bit(bytes);
    }
    return readStatus;
}

bool configureSession(const VisaApi &api, ViSession device, QString *error)
{
    ceyearVisaDiagLog(QStringLiteral("configureSession before VI_ATTR_TERMCHAR_EN"));
    const ViStatus termEnabled = api.setAttribute(device, VI_ATTR_TERMCHAR_EN, VI_TRUE);
    ceyearVisaDiagLog(QStringLiteral("configureSession after VI_ATTR_TERMCHAR_EN status=%1").arg(static_cast<qint64>(termEnabled)));
    if (termEnabled < VI_SUCCESS) {
        if (error) {
            *error = QStringLiteral("VI_ATTR_TERMCHAR_EN failed: %1").arg(visaStatusText(termEnabled));
        }
        return false;
    }

    ceyearVisaDiagLog(QStringLiteral("configureSession before VI_ATTR_TERMCHAR"));
    const ViStatus termChar = api.setAttribute(device, VI_ATTR_TERMCHAR, '\n');
    ceyearVisaDiagLog(QStringLiteral("configureSession after VI_ATTR_TERMCHAR status=%1").arg(static_cast<qint64>(termChar)));
    if (termChar < VI_SUCCESS) {
        if (error) {
            *error = QStringLiteral("VI_ATTR_TERMCHAR failed: %1").arg(visaStatusText(termChar));
        }
        return false;
    }

    ceyearVisaDiagLog(QStringLiteral("configureSession before VI_ATTR_TMO_VALUE"));
    const ViStatus timeout = api.setAttribute(device, VI_ATTR_TMO_VALUE, 2000);
    ceyearVisaDiagLog(QStringLiteral("configureSession after VI_ATTR_TMO_VALUE status=%1").arg(static_cast<qint64>(timeout)));
    if (timeout < VI_SUCCESS) {
        if (error) {
            *error = QStringLiteral("VI_ATTR_TMO_VALUE failed: %1").arg(visaStatusText(timeout));
        }
        return false;
    }

    ceyearVisaDiagLog(QStringLiteral("configureSession ok"));
    return true;
}

bool systemErrorIsClear(const QString &response)
{
    const QString normalized = response.trimmed().toLower();
    return normalized.isEmpty()
        || normalized == QStringLiteral("00")
        || normalized == QStringLiteral("0")
        || normalized.startsWith(QStringLiteral("0,"))
        || normalized.contains(QStringLiteral("no error"));
}

bool querySystemErrorClear(const QVector<CeyearStatusItem> &items)
{
    for (const auto &item : items) {
        if (item.command.compare(QStringLiteral("SYSTem:ERRor?"), Qt::CaseInsensitive) == 0) {
            return item.success && systemErrorIsClear(item.response);
        }
    }
    return false;
}

QVector<CeyearStatusItem> statusItemsFrom(const QVector<CeyearStatusItem> &items, int startIndex)
{
    QVector<CeyearStatusItem> result;
    if (startIndex < 0 || startIndex >= items.size()) {
        return result;
    }

    result.reserve(items.size() - startIndex);
    for (int i = startIndex; i < items.size(); ++i) {
        result.append(items.at(i));
    }
    return result;
}

bool hasCmdErr(const QStringList &errors)
{
    for (const QString &error : errors) {
        const QString normalized = error.trimmed().toLower();
        if (normalized.contains(QStringLiteral("402")) || normalized.contains(QStringLiteral("cmd err"))) {
            return true;
        }
    }
    return false;
}

bool statusItemsContainCommandError(const QVector<CeyearStatusItem> &items)
{
    for (const auto &item : items) {
        if (!item.success) {
            continue;
        }
        const QString normalized = item.response.trimmed().toLower();
        if (normalized.contains(QStringLiteral("402")) || normalized.contains(QStringLiteral("cmd err"))) {
            return true;
        }
    }
    return false;
}

bool querySystemErrorClear(const CeyearStatusResult &statusResult)
{
    for (const auto &item : statusResult.items) {
        if (item.command.compare(QStringLiteral("SYSTem:ERRor?"), Qt::CaseInsensitive) == 0) {
            return item.success && systemErrorIsClear(item.response);
        }
    }
    return false;
}

struct IqRawValidationResult
{
    bool ok = false;
    bool prefixFileScan = false;
    CeyearIfIqRawSummary summary;
    QString error;
};

IqRawValidationResult validateIqRawFast(const CeyearIqReceiverResult &receiverResult)
{
    IqRawValidationResult validation;
    validation.summary.bytesRead = receiverResult.bytesReceived;
    if (receiverResult.bytesReceived <= 0) {
        validation.error = QStringLiteral("3943B IQRAW file is empty.");
        return validation;
    }

    validation.prefixFileScan = true;
    validation.ok = CeyearIfIqPacketParser::validateRawFilePrefix(receiverResult.outputFilePath,
                                                                  &validation.summary,
                                                                  &validation.error);
    if (validation.ok && receiverResult.sampleCount > 0) {
        validation.summary.sampleCount = static_cast<int>(receiverResult.sampleCount);
    }
    return validation;
}

bool writeCommandSet(const VisaApi &api, ViSession device, const std::vector<CeyearVisaQuery> &commands, QStringList *errors);

void runStatusQueries(const VisaApi &api, ViSession device, CeyearStatusResult *result)
{
    for (const auto &query : defaultCeyearStatusQueries()) {
        CeyearStatusItem item;
        item.label = QString::fromLatin1(query.label);
        item.command = QString::fromLatin1(query.command);

        QString response;
        ceyearVisaDiagLog(QStringLiteral("runCaptureQueries before %1").arg(QString::fromLatin1(query.command)));
        const ViStatus status = queryCommand(api, device, query.command, &response);
        ceyearVisaDiagLog(QStringLiteral("runCaptureQueries after %1 status=%2 response=%3")
                              .arg(QString::fromLatin1(query.command))
                              .arg(static_cast<qint64>(status))
                              .arg(response));
        if (status < VI_SUCCESS) {
            item.error = visaStatusText(status);
        } else {
            item.success = true;
            item.response = response;
        }
        result->items.append(item);
    }

    result->success = ceyearStatusItemsAllSucceeded(result->items);
}

void runCaptureQueries(const VisaApi &api, ViSession device, CeyearCaptureResult *result)
{
    for (const auto &query : defaultCeyearCaptureStatusQueries()) {
        CeyearStatusItem item;
        item.label = QString::fromLatin1(query.label);
        item.command = QString::fromLatin1(query.command);

        QString response;
        ceyearVisaDiagLog(QStringLiteral("runCaptureQueries before %1").arg(QString::fromLatin1(query.command)));
        const ViStatus status = queryCommand(api, device, query.command, &response);
        ceyearVisaDiagLog(QStringLiteral("runCaptureQueries after %1 status=%2 response=%3")
                              .arg(QString::fromLatin1(query.command))
                              .arg(static_cast<qint64>(status))
                              .arg(response));
        if (status < VI_SUCCESS) {
            item.error = visaStatusText(status);
        } else {
            item.success = true;
            item.response = response;
        }
        result->items.append(item);
    }

    result->success = ceyearStatusItemsAllSucceeded(result->items);
}

std::vector<CeyearVisaQuery> fallbackCaptureCommands(const CeyearCaptureConfig &config)
{
    return defaultCeyearCaptureCommands(config);
}

std::vector<CeyearVisaQuery> fallbackStopCommands()
{
    return defaultCeyearStopCommands();
}

std::vector<CeyearVisaQuery> lowRiskCapturePathCommands(const CeyearCaptureConfig &config)
{
    std::vector<CeyearVisaQuery> commands;
    const QStringList lines = ceyearLowRiskPathSetupCommandLines(config);
    commands.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        commands.push_back({QByteArray("Configure PC report path"), line.toLatin1(), false});
    }
    return commands;
}

std::vector<CeyearVisaQuery> manualIqReportCommands(const CeyearCaptureConfig &config)
{
    std::vector<CeyearVisaQuery> commands;
    const QStringList lines = ceyearManualIqReportCommandLines(config);
    commands.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        commands.push_back({QByteArray("Start manual IQ report"), line.toLatin1(), false});
    }
    return commands;
}

std::vector<CeyearVisaQuery> triggerIqReportCommands(const CeyearCaptureConfig &config)
{
    std::vector<CeyearVisaQuery> commands;
    const QStringList lines = ceyearTriggerIqReportCommandLines(config);
    commands.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        commands.push_back({QByteArray("Trigger IQ report"), line.toLatin1(), false});
    }
    return commands;
}

std::vector<CeyearVisaQuery> triggerIqReportStopCommands()
{
    std::vector<CeyearVisaQuery> commands;
    const QStringList lines = ceyearTriggerIqReportStopCommandLines();
    commands.reserve(static_cast<std::size_t>(lines.size()));
    for (const QString &line : lines) {
        commands.push_back({QByteArray("Disable IQ report trigger"), line.toLatin1(), false});
    }
    return commands;
}

void runSingleSystemErrorQuery(const VisaApi &api, ViSession device, CeyearCaptureResult *result)
{
    CeyearStatusItem item;
    item.label = QStringLiteral("System error");
    item.command = QStringLiteral("SYSTem:ERRor?");

    QString response;
    const ViStatus status = queryCommand(api, device, QByteArray("SYSTem:ERRor?"), &response);
    if (status < VI_SUCCESS) {
        item.error = visaStatusText(status);
    } else {
        item.success = true;
        item.response = response;
    }
    result->items.append(item);
    result->success = ceyearStatusItemsAllSucceeded(result->items);
}

bool appendSystemErrorQuery(const VisaApi &api, ViSession device, const QString &label, CeyearCaptureResult *result)
{
    CeyearStatusItem item;
    item.label = label;
    item.command = QStringLiteral("SYSTem:ERRor?");

    QString response;
    const ViStatus status = queryCommand(api, device, QByteArray("SYSTem:ERRor?"), &response);
    if (status < VI_SUCCESS) {
        item.error = visaStatusText(status);
    } else {
        item.success = true;
        item.response = response;
    }
    result->items.append(item);
    return item.success && systemErrorIsClear(item.response);
}

void drainPscPreflightErrors(const VisaApi &api, ViSession device, CeyearCaptureResult *result)
{
    for (int i = 0; i < 8; ++i) {
        if (appendSystemErrorQuery(api, device, QStringLiteral("PSC preflight error queue"), result)) {
            return;
        }
    }
}

bool writePscCommandSetWithSystemErrorChecks(const VisaApi &api,
                                             ViSession device,
                                             const std::vector<CeyearVisaQuery> &commands,
                                             CeyearCaptureResult *result,
                                             QStringList *errors)
{
    bool ok = true;
    for (const auto &command : commands) {
        const QString commandText = QString::fromLatin1(command.command);
        ceyearVisaDiagLog(QStringLiteral("writePscCommandSet before %1: %2")
                              .arg(QString::fromLatin1(command.label), commandText));
        const ViStatus status = writeCommand(api, device, command.command);
        ceyearVisaDiagLog(QStringLiteral("writePscCommandSet after %1 status=%2")
                              .arg(commandText)
                              .arg(static_cast<qint64>(status)));
        if (status < VI_SUCCESS) {
            ok = false;
            if (errors) {
                errors->append(QStringLiteral("%1: %2").arg(QString::fromLatin1(command.label), visaStatusText(status)));
            }
            break;
        }

        if (!appendSystemErrorQuery(api,
                                    device,
                                    QStringLiteral("After PSC command: %1").arg(commandText),
                                    result)) {
            ok = false;
            if (errors) {
                const CeyearStatusItem &item = result->items.constLast();
                const QString detail = item.success ? item.response.trimmed() : item.error.trimmed();
                errors->append(QStringLiteral("%1 -> SYSTem:ERRor? %2").arg(commandText, detail));
            }
            break;
        }
    }
    return ok;
}

void runPscSweepCommandSet(const QString &resource,
                           const QString &captureName,
                           const std::vector<CeyearVisaQuery> &commands,
                           const QString &successMessage,
                           const QString &failurePrefix,
                           CeyearCaptureResult *result)
{
    result->resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result->captureName = captureName;

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result->message = loadError;
        return;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result->message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        return;
    }

    const QByteArray resourceBytes = result->resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result->message = QStringLiteral("viOpen %1 failed: %2").arg(result->resource, visaStatusText(status));
        api.close(resourceManager);
        return;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result->message = configError;
        api.close(device);
        api.close(resourceManager);
        return;
    }

    QStringList writeErrors;
    drainPscPreflightErrors(api, device, result);
    const bool writesOk = writePscCommandSetWithSystemErrorChecks(api, device, commands, result, &writeErrors);

    result->success = writesOk;
    if (!writesOk) {
        result->message = QStringLiteral("%1: %2").arg(failurePrefix, writeErrors.join(QStringLiteral("; ")));
    } else if (result->success) {
        result->message = successMessage;
    }

    api.close(device);
    api.close(resourceManager);
}

bool writeCommandSet(const VisaApi &api, ViSession device, const std::vector<CeyearVisaQuery> &commands, QStringList *errors)
{
    bool ok = true;
    for (const auto &command : commands) {
        ceyearVisaDiagLog(QStringLiteral("writeCommandSet before %1: %2")
                              .arg(QString::fromLatin1(command.label), QString::fromLatin1(command.command)));
        const ViStatus status = writeCommand(api, device, command.command);
        ceyearVisaDiagLog(QStringLiteral("writeCommandSet after %1 status=%2")
                              .arg(QString::fromLatin1(command.command))
                              .arg(static_cast<qint64>(status)));
        if (status < VI_SUCCESS) {
            ok = false;
            if (errors) {
                errors->append(QStringLiteral("%1: %2").arg(QString::fromLatin1(command.label), visaStatusText(status)));
            }
        }
    }
    return ok;
}

} // namespace

bool ceyearStatusContainsCommandError(const CeyearStatusResult &statusResult)
{
    return statusItemsContainCommandError(statusResult.items);
}

bool ceyearStatusContainsCommandError(const CeyearCaptureResult &captureResult)
{
    return statusItemsContainCommandError(captureResult.items);
}

bool ceyearStatusItemsAllSucceeded(const QVector<CeyearStatusItem> &items)
{
    if (items.isEmpty()) {
        return false;
    }
    for (const auto &item : items) {
        if (!item.success) {
            return false;
        }
    }
    return true;
}

CeyearStatusResult CeyearVisaService::queryStatus(const QString &resource) const
{
    CeyearStatusResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result.message = loadError;
        return result;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        return result;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        return result;
    }

    runStatusQueries(api, device, &result);
    api.close(device);
    api.close(resourceManager);
    return result;
}

CeyearStatusResult CeyearVisaService::applyControl(const QString &resource, const CeyearControlConfig &config) const
{
    CeyearStatusResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result.message = loadError;
        return result;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        return result;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        return result;
    }

    bool writesOk = true;
    QStringList writeErrors;
    for (const auto &command : defaultCeyearControlCommands(config)) {
        const ViStatus writeStatus = writeCommand(api, device, command.command);
        if (writeStatus < VI_SUCCESS) {
            writesOk = false;
            writeErrors.append(QStringLiteral("%1: %2").arg(QString::fromLatin1(command.label), visaStatusText(writeStatus)));
        }
    }

    runStatusQueries(api, device, &result);
    const bool errorQueueClear = querySystemErrorClear(result.items);
    result.success = writesOk && result.success && errorQueueClear;
    if (!writesOk) {
        result.message = QStringLiteral("3943B control write failed: %1").arg(writeErrors.join(QStringLiteral("; ")));
    } else if (!errorQueueClear) {
        result.message = QStringLiteral("3943B control wrote, but SYSTem:ERRor? is not clear");
    } else if (result.success) {
        result.message = QStringLiteral("3943B control ok");
    }

    api.close(device);
    api.close(resourceManager);
    return result;
}

CeyearCaptureResult CeyearVisaService::configureCapturePath(const QString &resource, const CeyearCaptureConfig &config) const
{
    CeyearCaptureResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result.captureName = QStringLiteral("3943B_PATH_CONFIGURATION");
    result.outputFilePath = config.outputFilePath;
    result.sampleCount = 0;

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result.message = loadError;
        return result;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        return result;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        return result;
    }

    QStringList writeErrors;
    const bool writesOk = writeCommandSet(api, device, lowRiskCapturePathCommands(config), &writeErrors);
    runCaptureQueries(api, device, &result);
    const bool errorQueueClear = querySystemErrorClear(result.items);

    result.success = writesOk && result.success && errorQueueClear;
    result.logs << QStringLiteral("3943B low-risk path setup sent without IF remote mode or FREE-run report.");
    if (!writesOk) {
        result.message = QStringLiteral("3943B path setup failed: %1").arg(writeErrors.join(QStringLiteral("; ")));
    } else if (!errorQueueClear) {
        result.message = QStringLiteral("3943B path setup wrote, but SYSTem:ERRor? is not clear");
    } else if (result.success) {
        result.message = QStringLiteral("3943B path setup ok");
    }

    api.close(device);
    api.close(resourceManager);
    return result;
}

CeyearCaptureResult CeyearVisaService::startManualIqReport(const QString &resource, const CeyearCaptureConfig &config) const
{
    CeyearCaptureResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result.captureName = config.outputFilePath.isEmpty() ? QStringLiteral("3943B_MANUAL_IQ_REPORT") : config.outputFilePath;
    result.outputFilePath = config.outputFilePath;
    result.sampleCount = 0;

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig receiverConfig;
    receiverConfig.protocol = config.pcReceiveProtocol.compare(QStringLiteral("UDP"), Qt::CaseInsensitive) == 0
        ? CeyearIqReceiveProtocol::Udp
        : CeyearIqReceiveProtocol::Tcp;
    receiverConfig.bindAddress = QStringLiteral("0.0.0.0");
    receiverConfig.port = static_cast<quint16>(config.pcReceivePort);
    receiverConfig.outputFilePath = config.outputFilePath;
    receiverConfig.timeoutMs = config.pcReceiveTimeoutMs;
    receiverConfig.targetSampleCount = config.iqPointCount;

    QString receiverError;
    if (!receiver.start(receiverConfig, &receiverError)) {
        result.message = QStringLiteral("PC IQ receiver failed: %1").arg(receiverError);
        result.logs << result.message;
        return result;
    }
    ActiveIqReceiverRegistration receiverRegistration(&receiver);
    result.logs << QStringLiteral("PC IQ receiver listening on %1:%2").arg(config.pcReceiveIpAddress).arg(receiver.boundPort());

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result.message = loadError;
        receiver.stop();
        return result;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        receiver.stop();
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        receiver.stop();
        return result;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        receiver.stop();
        return result;
    }

    QStringList writeErrors;
    const bool writesOk = writeCommandSet(api, device, manualIqReportCommands(config), &writeErrors);
    result.logs << QStringLiteral("3943B manual fixed-point IQ report command sent without FREE-run or INITiate.");
    const int startQueryIndex = result.items.size();
    runCaptureQueries(api, device, &result);
    const bool errorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, startQueryIndex));

    const CeyearIqReceiverResult receiverResult = receiver.waitForFinished(config.iqPointCount > 0 ? -1 : config.pcReceiveTimeoutMs);
    result.bytesReceived = receiverResult.bytesReceived;
    result.sampleCount = static_cast<int>(receiverResult.sampleCount);
    result.outputFilePath = receiverResult.outputFilePath;
    result.logs << QStringLiteral("PC IQ receiver saved %1 bytes to %2").arg(receiverResult.bytesReceived).arg(receiverResult.outputFilePath);
    CeyearIfIqRawSummary rawSummary;
    QString rawValidationError;
    const bool rawOk = receiverResult.bytesReceived > 0
        && CeyearIfIqPacketParser::validateRawFile(receiverResult.outputFilePath, &rawSummary, &rawValidationError);
    if (rawSummary.sampleCount > 0) {
        result.sampleCount = rawSummary.sampleCount;
    }
    if (config.iqPointCount > 0) {
        result.logs << QStringLiteral("PC IQ receiver parsed %1 / target %2 samples.")
                       .arg(result.sampleCount)
                       .arg(config.iqPointCount);
        result.logs << iqShortfallText(result.sampleCount, config.iqPointCount);
    }
    result.logs << QStringLiteral("3943B IQRAW validation: packets=%1, IF=%2, non-IF=%3, firstTag=%4")
                       .arg(rawSummary.packetCount)
                       .arg(rawSummary.ifPacketCount)
                       .arg(rawSummary.nonIfPacketCount)
                       .arg(rawSummary.firstPacketHeader);
    if (!rawOk && !rawValidationError.isEmpty()) {
        result.logs << rawValidationError;
    }

    QStringList stopErrors;
    const bool stopOk = writeCommandSet(api, device, preferredCeyearStopCommands(), &stopErrors);
    result.logs << QStringLiteral("3943B stop command sent after manual IQ receive window.");
    const int stopQueryIndex = result.items.size();
    runCaptureQueries(api, device, &result);
    const bool stopErrorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, stopQueryIndex));

    const bool sampleCountAccepted = iqSampleCountAcceptable(result.sampleCount, config.iqPointCount);
    result.success = writesOk && errorQueueClear && sampleCountAccepted && rawOk && stopOk && stopErrorQueueClear;
    if (!writesOk) {
        result.message = QStringLiteral("3943B manual IQ report start failed: %1").arg(writeErrors.join(QStringLiteral("; ")));
    } else if (!errorQueueClear) {
        result.message = QStringLiteral("3943B manual IQ report command written, but SYSTem:ERRor? is not clear");
    } else if (!sampleCountAccepted) {
        result.message = receiverResult.message;
    } else if (!rawOk) {
        result.message = QStringLiteral("3943B manual IQ report saved non-IQ data: %1").arg(rawValidationError);
    } else if (!stopOk) {
        result.message = QStringLiteral("3943B stop capture failed: %1").arg(stopErrors.join(QStringLiteral("; ")));
    } else if (!stopErrorQueueClear) {
        result.message = QStringLiteral("3943B stop command written, but SYSTem:ERRor? is not clear");
    } else if (result.success) {
        result.message = receiverResult.success
            ? QStringLiteral("3943B manual IQ report completed and stopped")
            : QStringLiteral("3943B manual IQ report completed with acceptable UDP sample loss");
    }

    api.close(device);
    api.close(resourceManager);
    return result;
}

CeyearCaptureResult CeyearVisaService::triggerIqReport(const QString &resource, const CeyearCaptureConfig &config) const
{
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport enter"));
    CeyearCaptureResult result;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after result constructed"));
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after result.resource=%1").arg(result.resource));
    result.captureName = config.outputFilePath.isEmpty() ? QStringLiteral("3943B_TRIGGER_IQ_REPORT") : config.outputFilePath;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after captureName"));
    result.outputFilePath = config.outputFilePath;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after outputFilePath=%1").arg(result.outputFilePath));
    result.sampleCount = 0;

    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before receiver construct"));
    CeyearIqReceiver receiver;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after receiver construct"));
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before receiverConfig construct"));
    CeyearIqReceiverConfig receiverConfig;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after receiverConfig construct"));
    receiverConfig.protocol = config.pcReceiveProtocol.compare(QStringLiteral("UDP"), Qt::CaseInsensitive) == 0
        ? CeyearIqReceiveProtocol::Udp
        : CeyearIqReceiveProtocol::Tcp;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after protocol"));
    receiverConfig.bindAddress = QStringLiteral("0.0.0.0");
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after bindAddress"));
    receiverConfig.port = static_cast<quint16>(config.pcReceivePort);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after port=%1").arg(receiverConfig.port));
    receiverConfig.outputFilePath = config.outputFilePath;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after receiver outputFilePath"));
    receiverConfig.timeoutMs = config.pcReceiveTimeoutMs;
    receiverConfig.targetSampleCount = config.iqPointCount;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after timeout=%1").arg(receiverConfig.timeoutMs));

    QString receiverError;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after receiverError construct"));
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before receiver.start protocol=%1 bind=%2 port=%3 file=%4 timeout=%5")
                          .arg(config.pcReceiveProtocol, receiverConfig.bindAddress)
                          .arg(receiverConfig.port)
                          .arg(receiverConfig.outputFilePath)
                          .arg(receiverConfig.timeoutMs));
    if (!receiver.start(receiverConfig, &receiverError)) {
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport receiver.start failed: %1").arg(receiverError));
        result.message = QStringLiteral("PC IQ receiver failed: %1").arg(receiverError);
        result.logs << result.message;
        return result;
    }
    ActiveIqReceiverRegistration receiverRegistration(&receiver);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after receiver.start bound=%1").arg(receiver.boundPort()));
    result.logs << QStringLiteral("PC IQ receiver listening on %1:%2").arg(config.pcReceiveIpAddress).arg(receiver.boundPort());

    VisaApi api;
    QString loadError;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before api.load"));
    if (!api.load(&loadError)) {
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport api.load failed: %1").arg(loadError));
        result.message = loadError;
        receiver.stop();
        return result;
    }
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after api.load"));

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before viOpenDefaultRM"));
    ViStatus status = api.openDefaultRM(&resourceManager);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after viOpenDefaultRM status=%1 rm=%2")
                          .arg(static_cast<qint64>(status))
                          .arg(static_cast<qulonglong>(resourceManager)));
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        receiver.stop();
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before viOpen resource=%1").arg(result.resource));
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after viOpen status=%1 device=%2")
                          .arg(static_cast<qint64>(status))
                          .arg(static_cast<qulonglong>(device)));
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        receiver.stop();
        return result;
    }

    QString configError;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before configureSession"));
    if (!configureSession(api, device, &configError)) {
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport configureSession failed: %1").arg(configError));
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        receiver.stop();
        return result;
    }
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after configureSession"));

    QStringList writeErrors;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before writeCommandSet start"));
    bool writesOk = writeCommandSet(api, device, triggerIqReportCommands(config), &writeErrors);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after writeCommandSet start ok=%1 errors=%2")
                           .arg(writesOk ? QStringLiteral("true") : QStringLiteral("false"),
                                writeErrors.join(QStringLiteral("; "))));
    result.logs << QStringLiteral("3943B vendor MAN IQ report command sent with IF remote SHORT and without FREE-run or INITiate.");

    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before receiver.waitForFinished targetSamples=%1 timeout=%2")
                          .arg(config.iqPointCount)
                          .arg(config.pcReceiveTimeoutMs));
    CeyearIqReceiverResult receiverResult = receiver.waitForFinished(config.iqPointCount > 0 ? -1 : config.pcReceiveTimeoutMs);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after receiver.waitForFinished success=%1 bytes=%2 message=%3")
                           .arg(receiverResult.success ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(receiverResult.bytesReceived)
                           .arg(receiverResult.message));
    result.bytesReceived = receiverResult.bytesReceived;
    result.sampleCount = static_cast<int>(receiverResult.sampleCount);
    result.outputFilePath = receiverResult.outputFilePath;
    result.logs << QStringLiteral("PC IQ receiver saved %1 bytes to %2").arg(receiverResult.bytesReceived).arg(receiverResult.outputFilePath);
    const IqRawValidationResult rawValidation = validateIqRawFast(receiverResult);
    const CeyearIfIqRawSummary &rawSummary = rawValidation.summary;
    const QString &rawValidationError = rawValidation.error;
    const bool rawOk = rawValidation.ok;
    if (rawSummary.sampleCount > 0) {
        result.sampleCount = rawSummary.sampleCount;
    }
    if (config.iqPointCount > 0) {
        result.logs << QStringLiteral("PC IQ receiver parsed %1 / target %2 samples.")
                       .arg(result.sampleCount)
                       .arg(config.iqPointCount);
        result.logs << iqShortfallText(result.sampleCount, config.iqPointCount);
    }
    if (rawValidation.prefixFileScan) {
        result.logs << QStringLiteral("3943B IQRAW light validation: prefixBytes=%1, packets=%2, IF=%3, non-IF=%4, firstTag=%5; final samples from receiver=%6.")
                       .arg(rawSummary.bytesRead)
                       .arg(rawSummary.packetCount)
                       .arg(rawSummary.ifPacketCount)
                       .arg(rawSummary.nonIfPacketCount)
                       .arg(rawSummary.firstPacketHeader)
                       .arg(result.sampleCount);
    } else if (receiverResult.bytesReceived > 0) {
        result.logs << QStringLiteral("3943B IQRAW stream validation: IF/IQ samples=%1, bytes=%2; full file scan skipped.")
                       .arg(rawSummary.sampleCount)
                       .arg(rawSummary.bytesRead);
    } else {
        result.logs << QStringLiteral("3943B IQRAW validation skipped: no bytes received.");
    }
    if (!rawOk && !rawValidationError.isEmpty()) {
        result.logs << rawValidationError;
    }

    const bool sampleCountAccepted = iqSampleCountAcceptable(result.sampleCount, config.iqPointCount);
    bool errorQueueClear = true;
    if (!sampleCountAccepted || !rawOk || !writesOk) {
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport before diagnostic SYSTem:ERRor?"));
        const int startQueryIndex = result.items.size();
        runSingleSystemErrorQuery(api, device, &result);
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport after diagnostic SYSTem:ERRor?"));
        errorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, startQueryIndex));
    } else {
        result.logs << QStringLiteral("3943B fast capture path skipped post-capture status queries after valid IQ samples.");
    }

    QStringList stopErrors;
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before writeCommandSet stop"));
    const bool stopOk = writeCommandSet(api, device, triggerIqReportStopCommands(), &stopErrors);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport after writeCommandSet stop ok=%1 errors=%2")
                          .arg(stopOk ? QStringLiteral("true") : QStringLiteral("false"),
                               stopErrors.join(QStringLiteral("; "))));
    result.logs << (receiverResult.success
        ? QStringLiteral("3943B trigger-only stop command sent after target IQ sample count was received.")
        : sampleCountAccepted
        ? QStringLiteral("3943B trigger-only stop command sent after accepted partial IQ capture.")
        : QStringLiteral("3943B trigger-only stop command sent after PC IQ receiver ended without reaching target sample count."));
    bool stopErrorQueueClear = true;
    if (!stopOk || !sampleCountAccepted || !rawOk) {
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport before stop diagnostic SYSTem:ERRor?"));
        const int stopQueryIndex = result.items.size();
        runSingleSystemErrorQuery(api, device, &result);
        ceyearVisaDiagLog(QStringLiteral("triggerIqReport after stop diagnostic SYSTem:ERRor?"));
        stopErrorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, stopQueryIndex));
    } else {
        result.logs << QStringLiteral("3943B fast capture path skipped post-stop status queries.");
    }

    result.success = writesOk && errorQueueClear && sampleCountAccepted && rawOk && stopOk && stopErrorQueueClear;
    if (!writesOk) {
        result.message = QStringLiteral("3943B trigger IQ report start failed: %1").arg(writeErrors.join(QStringLiteral("; ")));
    } else if (!errorQueueClear) {
        result.message = QStringLiteral("3943B trigger IQ report command written, but SYSTem:ERRor? is not clear");
    } else if (!sampleCountAccepted) {
        result.message = receiverResult.message;
    } else if (!rawOk) {
        result.message = QStringLiteral("3943B trigger IQ report saved non-IQ data: %1").arg(rawValidationError);
    } else if (!stopOk) {
        result.message = QStringLiteral("3943B trigger IQ report stop failed: %1").arg(stopErrors.join(QStringLiteral("; ")));
    } else if (!stopErrorQueueClear) {
        result.message = QStringLiteral("3943B trigger IQ report stop written, but SYSTem:ERRor? is not clear");
    } else if (result.success) {
        result.message = receiverResult.success
            ? QStringLiteral("3943B trigger IQ report completed and stopped")
            : QStringLiteral("3943B trigger IQ report completed with acceptable UDP sample loss");
    }

    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before close device"));
    api.close(device);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport before close resourceManager"));
    api.close(resourceManager);
    ceyearVisaDiagLog(QStringLiteral("triggerIqReport return success=%1 bytes=%2 message=%3")
                          .arg(result.success ? QStringLiteral("true") : QStringLiteral("false"))
                          .arg(result.bytesReceived)
                          .arg(result.message));
    return result;
}

CeyearCaptureResult CeyearVisaService::startCapture(const QString &resource, const CeyearCaptureConfig &config) const
{
    CeyearCaptureResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result.captureName = config.outputFilePath.isEmpty() ? QStringLiteral("3943B_CAPTURE") : config.outputFilePath;
    result.outputFilePath = config.outputFilePath;
    result.sampleCount = 0;

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig receiverConfig;
    receiverConfig.protocol = config.pcReceiveProtocol.compare(QStringLiteral("UDP"), Qt::CaseInsensitive) == 0
        ? CeyearIqReceiveProtocol::Udp
        : CeyearIqReceiveProtocol::Tcp;
    receiverConfig.bindAddress = QStringLiteral("0.0.0.0");
    receiverConfig.port = static_cast<quint16>(config.pcReceivePort);
    receiverConfig.outputFilePath = config.outputFilePath;
    receiverConfig.timeoutMs = config.pcReceiveTimeoutMs;
    receiverConfig.targetSampleCount = config.iqPointCount;

    QString receiverError;
    if (!receiver.start(receiverConfig, &receiverError)) {
        result.message = QStringLiteral("PC IQ receiver failed: %1").arg(receiverError);
        result.logs << result.message;
        return result;
    }
    ActiveIqReceiverRegistration receiverRegistration(&receiver);
    result.logs << QStringLiteral("PC IQ receiver listening on %1:%2").arg(config.pcReceiveIpAddress).arg(receiver.boundPort());

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result.message = loadError;
        receiver.stop();
        return result;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        receiver.stop();
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        receiver.stop();
        return result;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        receiver.stop();
        return result;
    }

    QStringList writeErrors;
    bool errorQueueClear = true;
    bool writesOk = writeCommandSet(api, device, preferredCeyearCaptureCommands(config), &writeErrors);
    int startQueryIndex = result.items.size();
    runCaptureQueries(api, device, &result);
    errorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, startQueryIndex));
    const bool preferredCmdErr = hasCmdErr(writeErrors) || ceyearStatusContainsCommandError(result);

    if (preferredCmdErr) {
        result.items.clear();
        writeErrors.clear();
        errorQueueClear = true;
        writesOk = writeCommandSet(api, device, fallbackCaptureCommands(config), &writeErrors);
        startQueryIndex = result.items.size();
        runCaptureQueries(api, device, &result);
        errorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, startQueryIndex));
    }

    const CeyearIqReceiverResult receiverResult = receiver.waitForFinished(config.iqPointCount > 0 ? -1 : config.pcReceiveTimeoutMs);
    result.bytesReceived = receiverResult.bytesReceived;
    result.sampleCount = static_cast<int>(receiverResult.sampleCount);
    result.outputFilePath = receiverResult.outputFilePath;
    result.logs << QStringLiteral("PC IQ receiver saved %1 bytes to %2").arg(receiverResult.bytesReceived).arg(receiverResult.outputFilePath);
    CeyearIfIqRawSummary rawSummary;
    QString rawValidationError;
    const bool rawOk = receiverResult.bytesReceived > 0
        && CeyearIfIqPacketParser::validateRawFile(receiverResult.outputFilePath, &rawSummary, &rawValidationError);
    if (rawSummary.sampleCount > 0) {
        result.sampleCount = rawSummary.sampleCount;
    }
    if (config.iqPointCount > 0) {
        result.logs << QStringLiteral("PC IQ receiver parsed %1 / target %2 samples.")
                       .arg(result.sampleCount)
                       .arg(config.iqPointCount);
        result.logs << iqShortfallText(result.sampleCount, config.iqPointCount);
    }
    result.logs << QStringLiteral("3943B IQRAW validation: packets=%1, IF=%2, non-IF=%3, firstTag=%4")
                       .arg(rawSummary.packetCount)
                       .arg(rawSummary.ifPacketCount)
                       .arg(rawSummary.nonIfPacketCount)
                       .arg(rawSummary.firstPacketHeader);
    if (!rawOk && !rawValidationError.isEmpty()) {
        result.logs << rawValidationError;
    }

    QStringList stopErrors;
    bool stopOk = writeCommandSet(api, device, preferredCeyearStopCommands(), &stopErrors);
    const bool sampleCountAccepted = iqSampleCountAcceptable(result.sampleCount, config.iqPointCount);
    result.logs << (receiverResult.success
        ? QStringLiteral("3943B stop command sent after target IQ sample count was received.")
        : sampleCountAccepted
        ? QStringLiteral("3943B stop command sent after accepted partial IQ capture.")
        : QStringLiteral("3943B stop command sent after PC IQ receiver ended without reaching target sample count."));
    int stopQueryIndex = result.items.size();
    runCaptureQueries(api, device, &result);
    bool stopErrorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, stopQueryIndex));
    const bool stopPreferredCmdErr = hasCmdErr(stopErrors) || ceyearStatusContainsCommandError(result);
    if (stopPreferredCmdErr) {
        result.items.clear();
        stopErrors.clear();
        stopOk = writeCommandSet(api, device, fallbackStopCommands(), &stopErrors);
        result.logs << QStringLiteral("3943B stop command fell back to compatibility mode.");
        stopQueryIndex = result.items.size();
        runCaptureQueries(api, device, &result);
        stopErrorQueueClear = querySystemErrorClear(statusItemsFrom(result.items, stopQueryIndex));
    }

    result.success = writesOk && errorQueueClear && sampleCountAccepted && rawOk && stopOk && stopErrorQueueClear;
    if (!writesOk) {
        result.message = preferredCmdErr
            ? QStringLiteral("3943B capture command fell back to compatibility mode")
            : QStringLiteral("3943B capture start failed: %1").arg(writeErrors.join(QStringLiteral("; ")));
    } else if (!errorQueueClear) {
        result.message = QStringLiteral("3943B capture command written, but SYSTem:ERRor? is not clear");
    } else if (!sampleCountAccepted) {
        result.message = receiverResult.message;
    } else if (!rawOk) {
        result.message = QStringLiteral("3943B capture saved non-IQ data: %1").arg(rawValidationError);
    } else if (!stopOk) {
        result.message = stopPreferredCmdErr
            ? QStringLiteral("3943B stop command fell back to compatibility mode")
            : QStringLiteral("3943B stop capture failed: %1").arg(stopErrors.join(QStringLiteral("; ")));
    } else if (!stopErrorQueueClear) {
        result.message = QStringLiteral("3943B stop command written, but SYSTem:ERRor? is not clear");
    } else if (result.success) {
        result.message = receiverResult.success
            ? QStringLiteral("3943B capture completed and stopped")
            : QStringLiteral("3943B capture completed with acceptable UDP sample loss");
    }

    api.close(device);
    api.close(resourceManager);
    return result;
}

CeyearCaptureResult CeyearVisaService::stopCapture(const QString &resource) const
{
    CeyearCaptureResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result.captureName = QStringLiteral("3943B_CAPTURE");

    VisaApi api;
    QString loadError;
    if (!api.load(&loadError)) {
        result.message = loadError;
        return result;
    }

    ViSession resourceManager = VI_NULL;
    ViSession device = VI_NULL;
    ViStatus status = api.openDefaultRM(&resourceManager);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpenDefaultRM failed: %1").arg(visaStatusText(status));
        return result;
    }

    const QByteArray resourceBytes = result.resource.toLocal8Bit();
    status = api.open(resourceManager,
                      reinterpret_cast<ViRsrc>(const_cast<char *>(resourceBytes.constData())),
                      VI_NULL,
                      VI_NULL,
                      &device);
    if (status < VI_SUCCESS) {
        result.message = QStringLiteral("viOpen %1 failed: %2").arg(result.resource, visaStatusText(status));
        api.close(resourceManager);
        return result;
    }

    QString configError;
    if (!configureSession(api, device, &configError)) {
        result.message = configError;
        api.close(device);
        api.close(resourceManager);
        return result;
    }

    QStringList writeErrors;
    bool writesOk = writeCommandSet(api, device, preferredCeyearStopCommands(), &writeErrors);
    runCaptureQueries(api, device, &result);
    bool errorQueueClear = querySystemErrorClear(result.items);
    const bool preferredCmdErr = hasCmdErr(writeErrors) || ceyearStatusContainsCommandError(result);
    if (preferredCmdErr) {
        result.items.clear();
        writeErrors.clear();
        writesOk = writeCommandSet(api, device, fallbackStopCommands(), &writeErrors);
        runCaptureQueries(api, device, &result);
        errorQueueClear = querySystemErrorClear(result.items);
    }

    result.success = writesOk && result.success && errorQueueClear;
    if (!writesOk) {
        result.message = preferredCmdErr
            ? QStringLiteral("3943B stop command fell back to compatibility mode")
            : QStringLiteral("3943B stop capture failed: %1").arg(writeErrors.join(QStringLiteral("; ")));
    } else if (!errorQueueClear) {
        result.message = QStringLiteral("3943B stop command written, but SYSTem:ERRor? is not clear");
    } else if (result.success) {
        result.message = QStringLiteral("3943B capture stopped");
    }

    api.close(device);
    api.close(resourceManager);
    return result;
}

void CeyearVisaService::requestStop() const
{
    requestActiveIqReceiverStop();
}

CeyearCaptureResult CeyearVisaService::startPscSweep(const QString &resource, const CeyearPscSweepConfig &config) const
{
    CeyearCaptureResult result;
    runPscSweepCommandSet(resource,
                          QStringLiteral("3943B_PSC_SWEEP"),
                          pscSweepStartQueries(config),
                          QStringLiteral("3943B PSC sweep started"),
                          QStringLiteral("3943B PSC sweep start failed"),
                          &result);
    return result;
}

CeyearCaptureResult CeyearVisaService::stopPscSweep(const QString &resource) const
{
    CeyearCaptureResult result;
    runPscSweepCommandSet(resource,
                          QStringLiteral("3943B_PSC_SWEEP"),
                          pscSweepStopQueries(),
                          QStringLiteral("3943B PSC sweep stopped"),
                          QStringLiteral("3943B PSC sweep stop failed"),
                          &result);
    return result;
}
