#include <QtTest/QtTest>

#include "core/Records.h"
#include "services/AppStateStore.h"
#include "services/CapProbeService.h"
#include "services/CeyearVisaQuery.h"
#include "services/PreprocessProcessRunner.h"
#include "services/UsrpDeviceService.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <vector>

class ServiceTests : public QObject
{
    Q_OBJECT

private slots:
    void sampleRecordRoundTripsThroughJson();
    void appStateStorePersistsRecordsAndVersions();
    void capProbeReadsCurrentTrialHeaderFormat();
    void preprocessResponseParserKeepsSegmentsAndLogs();
    void preprocessRunnerExecutesBridgeScript();
    void preprocessRunnerTimesOutHungProcess();
    void preprocessFailureResponseDoesNotBuildSamples();
    void demoPreprocessBuildsSamplesWithoutPython();
    void usrpCaptureWriterCreatesReadableCapFile();
    void usrpCaptureWriterAcceptsSc16Samples();
    void ceyearStatusQueriesAreReadOnlyAndCoverCoreState();
};

static void writeDoubleBE(QByteArray &buffer, int offset, double value)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << value;
    for (int i = 0; i < bytes.size(); ++i) {
        buffer[offset + i] = bytes.at(i);
    }
}

static void writeUInt32BE(QByteArray &buffer, int offset, quint32 value)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << value;
    for (int i = 0; i < bytes.size(); ++i) {
        buffer[offset + i] = bytes.at(i);
    }
}

void ServiceTests::sampleRecordRoundTripsThroughJson()
{
    SampleRecord record;
    record.sampleId = "pp_iq_seg_001";
    record.sourceType = "local_preprocess";
    record.rawFilePath = "D:/data/input.cap";
    record.sampleFilePath = "D:/data/output/seg_001.npy";
    record.labelType = "DJI";
    record.labelIndividual = "Drone-01";
    record.sampleRateHz = 12800000.0;
    record.centerFrequencyHz = 1090000000.0;
    record.dataFormat = "complex64_npy";
    record.sampleCount = 4096;
    record.deviceId = "IQ_2025";
    record.startSample = 100;
    record.endSample = 4196;
    record.status = "待复核";
    record.sourceName = "预处理输出";

    const auto restored = SampleRecord::fromJson(record.toJson());
    QCOMPARE(restored.sampleId, record.sampleId);
    QCOMPARE(restored.sampleRateHz, record.sampleRateHz);
    QCOMPARE(restored.status, QString("待复核"));
}

void ServiceTests::appStateStorePersistsRecordsAndVersions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AppStateStore store(dir.filePath("app_state.json"));
    SampleRecord sample;
    sample.sampleId = "sample-1";
    sample.sampleFilePath = "D:/sample.npy";
    sample.status = "待复核";
    DatasetVersionRecord version;
    version.versionId = "dataset-v1";
    version.taskType = "类型识别";
    version.sampleCount = 1;

    AppState state;
    state.samples.append(sample);
    state.datasetVersions.append(version);

    QVERIFY2(store.save(state), qPrintable(store.lastError()));
    const auto loaded = store.load();
    QVERIFY2(loaded.has_value(), qPrintable(store.lastError()));
    QCOMPARE(loaded->samples.size(), 1);
    QCOMPARE(loaded->datasetVersions.size(), 1);
    QCOMPARE(loaded->samples.first().sampleId, QString("sample-1"));
}

void ServiceTests::capProbeReadsCurrentTrialHeaderFormat()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("sample.cap");

    QByteArray file(0x200, '\0');
    file.replace(0, 7, "B.01.00");
    writeDoubleBE(file, 0x0010, 10000000.0);
    writeDoubleBE(file, 0x0018, 1090000000.0);
    writeUInt32BE(file, 0x0110, 62500);
    writeUInt32BE(file, 0x0114, 1024);

    QByteArray iq;
    QDataStream stream(&iq, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << qint16(1) << qint16(-1) << qint16(2) << qint16(-2) << qint16(3) << qint16(-3);
    file.append(iq);

    QFile output(path);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write(file), qint64(file.size()));
    output.close();

    CapProbeService service;
    const auto result = service.probe(path);
    QVERIFY2(result.has_value(), qPrintable(service.lastError()));
    QCOMPARE(result->headerLength, 0x200);
    QCOMPARE(result->bandwidthHz, 10000000.0);
    QCOMPARE(result->sampleRateHz, 12800000.0);
    QCOMPARE(result->centerFrequencyHz, 1090000000.0);
    QCOMPARE(result->previewPairs.size(), 3);
}

void ServiceTests::preprocessResponseParserKeepsSegmentsAndLogs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString responsePath = dir.filePath("response.json");

    QJsonObject segment;
    segment["segment_id"] = "SEG_001";
    segment["start_sample"] = 10;
    segment["end_sample"] = 30;
    segment["duration_ms"] = 0.25;
    segment["center_freq_hz"] = 1090000000.0;
    segment["bandwidth_hz"] = 12000000.0;
    segment["snr_db"] = 18.5;
    segment["score"] = 0.91;
    segment["output_file_path"] = dir.filePath("seg_001.npy");
    segment["status"] = "valid";

    QJsonObject root;
    root["success"] = true;
    root["message"] = "处理成功";
    root["detected_segment_count"] = 1;
    root["candidate_segment_count"] = 2;
    root["output_sample_count"] = 1;
    root["segments"] = QJsonArray{segment};
    root["logs"] = QJsonArray{"启动", "完成"};

    QFile file(responsePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();

    QString error;
    const auto result = PreprocessProcessRunner::readResponseFile(responsePath, &error);
    QVERIFY2(result.has_value(), qPrintable(error));
    QVERIFY(result->success);
    QCOMPARE(result->segments.size(), 1);
    QCOMPARE(result->logs.size(), 2);
    QCOMPARE(result->candidateSegmentCount, 2);
}

void ServiceTests::preprocessRunnerExecutesBridgeScript()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString inputCapPath = dir.filePath("input.cap");
    QFile capFile(inputCapPath);
    QVERIFY(capFile.open(QIODevice::WriteOnly));
    capFile.write("dummy");
    capFile.close();

    const QString outputSamplePath = QDir(dir.filePath("out")).filePath("seg_001.npy");
    const QString runnerScriptPath = dir.filePath("fake_runner.py");
    QFile runnerScript(runnerScriptPath);
    QVERIFY(runnerScript.open(QIODevice::WriteOnly | QIODevice::Truncate));
    runnerScript.write(R"(from __future__ import annotations
import json
import pathlib
import sys

request_path = pathlib.Path(sys.argv[1])
response_path = pathlib.Path(sys.argv[2])
request = json.loads(request_path.read_text(encoding="utf-8"))
pathlib.Path(request["sample_output_dir"]).mkdir(parents=True, exist_ok=True)
pathlib.Path(request["sample_output_dir"], "seg_001.npy").write_bytes(b"npy")
response = {
    "success": True,
    "message": "处理成功",
    "detected_segment_count": 1,
    "candidate_segment_count": 1,
    "output_sample_count": 1,
    "sample_output_dir": request["sample_output_dir"],
    "segments": [
        {
            "segment_id": "SEG_001",
            "start_sample": 10,
            "end_sample": 20,
            "duration_ms": 0.25,
            "center_freq_hz": 1090000000.0,
            "bandwidth_hz": 12000000.0,
            "snr_db": 18.5,
            "score": 0.91,
            "output_file_path": str(pathlib.Path(request["sample_output_dir"], "seg_001.npy")),
            "status": "valid",
        }
    ],
    "logs": ["启动", "完成"],
}
response_path.write_text(json.dumps(response, ensure_ascii=False), encoding="utf-8")
)");
    runnerScript.close();

    PreprocessRequest request;
    request.inputFilePath = inputCapPath;
    request.sampleOutputDir = dir.filePath("out");
    request.modelWeightsPath = dir.filePath("weights.pth");

    CapProbeResult capInfo;
    capInfo.sampleRateHz = 12800000.0;
    capInfo.centerFrequencyHz = 1090000000.0;

    QString error;
    const auto result = PreprocessProcessRunner::run(request, capInfo, &error, QStringLiteral("python"), runnerScriptPath);
    QVERIFY2(result.has_value(), qPrintable(error));
    QCOMPARE(result->message, QStringLiteral("处理成功"));
    QCOMPARE(result->segments.size(), 1);
    QCOMPARE(result->sampleRecords.size(), 1);
    QCOMPARE(QDir::cleanPath(result->sampleRecords.first().sampleFilePath), QDir::cleanPath(outputSamplePath));
}

void ServiceTests::preprocessRunnerTimesOutHungProcess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString inputCapPath = dir.filePath("input.cap");
    QFile capFile(inputCapPath);
    QVERIFY(capFile.open(QIODevice::WriteOnly));
    capFile.write("dummy");
    capFile.close();

    const QString runnerScriptPath = dir.filePath("hung_runner.py");
    QFile runnerScript(runnerScriptPath);
    QVERIFY(runnerScript.open(QIODevice::WriteOnly | QIODevice::Truncate));
    runnerScript.write(R"(import time
time.sleep(5)
)");
    runnerScript.close();

    PreprocessRequest request;
    request.inputFilePath = inputCapPath;
    request.sampleOutputDir = dir.filePath("out");
    request.modelWeightsPath = dir.filePath("weights.pth");
    request.timeoutMs = 100;

    CapProbeResult capInfo;
    QString error;
    const auto result = PreprocessProcessRunner::run(request, capInfo, &error, QStringLiteral("python"), runnerScriptPath);
    QVERIFY(!result.has_value());
    QVERIFY2(error.contains(QStringLiteral("超时")), qPrintable(error));
}

void ServiceTests::preprocessFailureResponseDoesNotBuildSamples()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString outputPath = dir.filePath("seg_001.npy");
    QFile sample(outputPath);
    QVERIFY(sample.open(QIODevice::WriteOnly));
    sample.write("npy");
    sample.close();

    PreprocessSegment segment;
    segment.segmentId = "SEG_001";
    segment.outputFilePath = outputPath;
    segment.status = "valid";

    PreprocessRunResult result;
    result.success = false;
    result.segments.append(segment);

    CapProbeResult capInfo;
    const auto records = PreprocessProcessRunner::buildSampleRecords(dir.filePath("input.cap"), capInfo, result);
    QVERIFY(records.isEmpty());
}

void ServiceTests::demoPreprocessBuildsSamplesWithoutPython()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString inputCapPath = dir.filePath("demo.cap");
    QFile capFile(inputCapPath);
    QVERIFY(capFile.open(QIODevice::WriteOnly));
    capFile.write("demo");
    capFile.close();

    PreprocessRequest request;
    request.inputFilePath = inputCapPath;
    request.sampleOutputDir = dir.filePath("samples");

    CapProbeResult capInfo;
    capInfo.sampleRateHz = 12800000.0;
    capInfo.centerFrequencyHz = 1090000000.0;
    capInfo.bandwidthHz = 10000000.0;

    const auto result = PreprocessProcessRunner::runDemo(request, capInfo);

    QVERIFY(result.success);
    QCOMPARE(result.segments.size(), 4);
    QCOMPARE(result.sampleRecords.size(), 4);
    QVERIFY(QFileInfo::exists(result.sampleRecords.first().sampleFilePath));
    QCOMPARE(result.sampleRecords.first().sourceType, QStringLiteral("demo_preprocess"));
}

void ServiceTests::usrpCaptureWriterCreatesReadableCapFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    UsrpCaptureConfig config;
    config.centerFrequencyHz = 1090000000.0;
    config.sampleRateHz = 12800000.0;
    config.bandwidthHz = 10000000.0;
    config.gainDb = 30.0;
    config.antenna = QStringLiteral("RX2");
    config.durationSeconds = 0.1;
    config.outputFilePath = dir.filePath("usrp.cap");

    QVector<QPair<qint16, qint16>> samples;
    samples.append({1, -1});
    samples.append({2, -2});
    samples.append({3, -3});

    QString error;
    QVERIFY2(UsrpDeviceService::writeCapFile(config, samples, &error), qPrintable(error));

    CapProbeService probe;
    const auto result = probe.probe(config.outputFilePath);
    QVERIFY2(result.has_value(), qPrintable(probe.lastError()));
    QCOMPARE(result->headerLength, 0x200);
    QCOMPARE(result->bandwidthHz, 10000000.0);
    QCOMPARE(result->sampleRateHz, 12800000.0);
    QCOMPARE(result->centerFrequencyHz, 1090000000.0);
    QCOMPARE(result->previewPairs.size(), 3);
}

void ServiceTests::usrpCaptureWriterAcceptsSc16Samples()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    UsrpCaptureConfig config;
    config.centerFrequencyHz = 2400000000.0;
    config.sampleRateHz = 12800000.0;
    config.bandwidthHz = 10000000.0;
    config.outputFilePath = dir.filePath("usrp_sc16.cap");

    const std::vector<UsrpSc16Sample> samples{
        {qint16(11), qint16(-11)},
        {qint16(22), qint16(-22)},
        {qint16(33), qint16(-33)},
    };

    QString error;
    QVERIFY2(UsrpDeviceService::writeCapFileFromSc16(config, samples, &error), qPrintable(error));

    CapProbeService probe;
    const auto result = probe.probe(config.outputFilePath);
    QVERIFY2(result.has_value(), qPrintable(probe.lastError()));
    QCOMPARE(result->centerFrequencyHz, 2400000000.0);
    QCOMPARE(result->previewPairs.size(), 3);
    QCOMPARE(result->previewPairs.at(0).first, qint16(11));
    QCOMPARE(result->previewPairs.at(0).second, qint16(-11));
}

void ServiceTests::ceyearStatusQueriesAreReadOnlyAndCoverCoreState()
{
    const auto queries = defaultCeyearStatusQueries();
    QVERIFY(queries.size() >= 8);

    const QStringList commands = [&]() {
        QStringList values;
        for (const auto &query : queries) {
            QVERIFY2(query.readOnly, qPrintable(query.label));
            QVERIFY2(query.command.endsWith(QLatin1Char('?')), qPrintable(query.command));
            values.append(query.command);
        }
        return values;
    }();

    QVERIFY(commands.contains(QStringLiteral("*IDN?")));
    QVERIFY(commands.contains(QStringLiteral("SYSTem:VERSion?")));
    QVERIFY(commands.contains(QStringLiteral("MEASure:MODE?")));
    QVERIFY(commands.contains(QStringLiteral("FREQuency?")));
    QVERIFY(commands.contains(QStringLiteral("FREQuency:SPAN?")));
    QVERIFY(commands.contains(QStringLiteral("DEModulation?")));
    QVERIFY(commands.contains(QStringLiteral("DETector?")));
    QVERIFY(commands.contains(QStringLiteral("SYSTem:ERRor[:NEXT]?")));
}

QTEST_MAIN(ServiceTests)
#include "service_tests.moc"
