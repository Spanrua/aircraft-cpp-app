#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

struct CeyearPscSweepConfig;

struct CeyearControlConfig
{
    double centerFrequencyHz = 2400000000.0;
    double frequencySpanHz = 20000000.0;
    QString demodulation = QStringLiteral("IQ");
    QString detector = QStringLiteral("PEAK");
};

struct CeyearVisaQuery
{
    QByteArray label;
    QByteArray command;
    bool readOnly = true;
};

struct CeyearStatusItem
{
    QString label;
    QString command;
    QString response;
    bool success = false;
    QString error;
};

struct CeyearStatusResult
{
    QString resource;
    QVector<CeyearStatusItem> items;
    bool success = false;
    QString message;
};

struct CeyearCaptureConfig
{
    double centerFrequencyHz = 2400000000.0;
    double frequencySpanHz = 20000000.0;
    QString measureMode = QStringLiteral("FIX");
    double measurementTimeSeconds = 3.0;
    int iqPointCount = 0;
    bool recordEnabled = true;
    QString recordSource = QStringLiteral("IQAudio");
    QString reportPath = QStringLiteral("UDP");
    QString pcReceiveProtocol = QStringLiteral("UDP");
    QString demodulation = QStringLiteral("IQ");
    QString detector = QStringLiteral("PEAK");
    QString pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    int pcReceivePort = 5030;
    int pcReceiveTimeoutMs = 5000;
    bool sendInitiate = false;
    QString outputFilePath;
};

struct CeyearCaptureResult
{
    bool success = false;
    QString message;
    QString resource;
    QVector<CeyearStatusItem> items;
    QString captureName;
    int sampleCount = 0;
    qint64 bytesReceived = 0;
    QString outputFilePath;
    QStringList logs;
};

QString defaultCeyearVisaResource();
std::vector<CeyearVisaQuery> defaultCeyearStatusQueries();
std::vector<CeyearVisaQuery> defaultCeyearIfIqProbeQueries();
std::vector<CeyearVisaQuery> defaultCeyearControlCommands(const CeyearControlConfig &config);
std::vector<CeyearVisaQuery> defaultCeyearCaptureCommands(const CeyearCaptureConfig &config);
std::vector<CeyearVisaQuery> preferredCeyearCaptureCommands(const CeyearCaptureConfig &config);
std::vector<CeyearVisaQuery> preferredCeyearStopCommands();
std::vector<CeyearVisaQuery> defaultCeyearStopCommands();
std::vector<CeyearVisaQuery> defaultCeyearCaptureStatusQueries();
QStringList ceyearCaptureSmokeCommandLines(const CeyearCaptureConfig &config);
QStringList ceyearCaptureSmokeStopCommandLines();
QStringList ceyearLowRiskPathSetupCommandLines(const CeyearCaptureConfig &config);
QStringList ceyearRiskyStreamStartCommandLines(const CeyearCaptureConfig &config);
QStringList ceyearManualIqReportCommandLines(const CeyearCaptureConfig &config);
QStringList ceyearTriggerIqReportCommandLines(const CeyearCaptureConfig &config);
QStringList ceyearTriggerIqReportStopCommandLines();
std::vector<CeyearVisaQuery> pscSweepStartQueries(const CeyearPscSweepConfig &config);
std::vector<CeyearVisaQuery> pscSweepStopQueries();
bool ceyearCaptureSmokeRequiresRiskAcknowledgement(const CeyearCaptureConfig &config,
                                                   bool execute,
                                                   bool allowRiskyInitiate);
bool ceyearCaptureSmokeRequiresFreeRunAcknowledgement(const CeyearCaptureConfig &config,
                                                      bool execute,
                                                      bool allowRiskyFreeRun);
bool ceyearCaptureSmokeRequiresFreeRunAcknowledgement(const CeyearCaptureConfig &config,
                                                      bool execute,
                                                      bool allowRiskyFreeRun,
                                                      bool configurePathOnly);
bool ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(bool execute,
                                                             bool manualIqReport,
                                                             bool allowManualIqReport);
bool ceyearCaptureSmokeRequiresTriggerIqReportAcknowledgement(bool execute,
                                                              bool triggerIqReport,
                                                              bool allowTriggerIqReport);
bool ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(bool execute,
                                                                    bool usesIfRemoteShort,
                                                                    bool allowRebootObservedIfShort);
bool ceyearCaptureSmokeRequiresReceivedBytes(bool execute, bool configurePathOnly);
