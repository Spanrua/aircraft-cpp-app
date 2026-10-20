#include <QtTest/QtTest>

#include "app/ProjectPaths.h"
#include "core/Records.h"
#include "services/AppStateStore.h"
#include "services/CandidateReviewService.h"
#include "services/CapProbeService.h"
#include "services/CeyearIfIqPacketParser.h"
#include "services/CeyearIqReceiver.h"
#include "services/CeyearPscReceiver.h"
#include "services/CeyearPscSweep.h"
#include "services/CeyearVisaQuery.h"
#include "services/CeyearVisaService.h"
#include "services/PreprocessProcessRunner.h"
#include "services/SignalCleaningProcessRunner.h"
#include "services/UsrpDeviceService.h"

#include <QTcpSocket>
#include <QUdpSocket>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <vector>

class ServiceTests : public QObject
{
    Q_OBJECT

private slots:
    void sampleRecordRoundTripsThroughJson();
    void trainingRunRecordRoundTripsThroughJson();
    void appStateStorePersistsRecordsAndVersions();
    void appStateStoreDoesNotOverwriteInvalidState();
    void projectPathsUsesConfiguredCppAppRootForPortableRuntime();
    void candidateReviewGroupsAggregateAndIsolateSources();
    void candidateReviewGroupsSelectRepresentativeWindows();
    void capProbeReadsCurrentTrialHeaderFormat();
    void preprocessResponseParserKeepsSegmentsAndLogs();
    void preprocessRequestSerializesRealControls();
    void preprocessRunnerExecutesBridgeScript();
    void preprocessRunnerTimesOutHungProcess();
    void preprocessFailureResponseDoesNotBuildSamples();
    void preprocessSampleRecordsKeepNpzWindowMetadata();
    void signalCleaningStartupFailureFinishesOnceWithoutCrashing();
    void signalCleaningRejectsHeaderOnlyResultCsv();
    void usrpCaptureWriterCreatesReadableCapFile();
    void usrpCaptureWriterAcceptsSc16Samples();
    void ceyearStatusQueriesAreReadOnlyAndCoverCoreState();
    void ceyearControlCommandsCoverApprovedWrites();
    void ceyearCaptureCommandsStartInstrumentAcquisition();
    void ceyearIfIqProbeQueriesAreReadOnlyAndCoverManualState();
    void ceyearCaptureRiskClassificationBlocksKnownHazards();
    void ceyearManualIqReportCommandsSetPointCount();
    void ceyearManualIqReportRequiresExplicitAcknowledgement();
    void ceyearManualIqReportRequiresRebootObservedAcknowledgement();
    void ceyearTriggerIqReportCommandsMatchVendorManualSequence();
    void ceyearStopCommandsStopRecordingAndCloseTrigger();
    void ceyearCaptureDetectsScpiCommandErrorsForFallback();
    void ceyearStatusAggregationAllowsRepeatedSuccessfulQueryBatches();
    void ceyearIfIqParserExtractsLittleEndianSamplesFromPacketStream();
    void ceyearIfIqRawValidationRejectsPscAndEmptyCaptures();
    void ceyearIfIqRawPrefixValidationAvoidsFullFileScan();
    void ceyearIfIqParserConvertsRawPacketsToReadableCapFile();
    void ceyearDefaultResourceUsesLanSocket();
    void ceyearSmokeToolDryRunListsCommandsAndRequiresExecute();
    void ceyearPscSweepCommandsUseOnlyPscReportPath();
    void ceyearPscSweepDefaultTargetUsesLocal3943bNetwork();
    void ceyearPscSweepUsesSingleBandFrequencyCommands();
    void ceyearPscSweepStopCommandsAbortAndClearUdp();
    void ceyearPscParserExtractsFrequencyAndLevelBins();
    void ceyearPscReceiverEmitsFramesFromUdpDatagrams();
    void ceyearPscCandidateExtractorPromotesPersistentPeaks();
    void ceyearPscCandidateExtractorUsesConfiguredHitThreshold();
    void ceyearPscSweepCsvWriterPersistsFrameBins();
    void ceyearPscSweepCsvReaderRestoresSortedFrames();
    void ceyearPscSweepCsvReaderSkipsZeroFrequencySentinelBins();
    void ceyearPscSweepCsvReaderRejectsMissingColumnsAndInvalidRows();
    void ceyearPscSweepQueriesAreWritableAndServiceExposesInterface();
    void ceyearIqReceiverWritesTcpPayloadToPcFile();
    void ceyearIqReceiverKeepsTcpStreamOpenForCaptureWindow();
    void ceyearIqReceiverPreservesExistingFileWhenStartFails();
    void ceyearIqReceiverWritesUdpPayloadToPcFile();
    void ceyearIqReceiverStopsAtTargetSampleCount();
    void ceyearIqReceiverWaitsForDelayedTargetPackets();
    void ceyearIqReceiverStopsWhenTargetStreamGoesIdle();
    void ceyearIqReceiverCancelStopsTargetWait();
};

static void writeUInt16BE(QByteArray &buffer, int offset, quint16 value)
{
    buffer[offset] = static_cast<char>((value >> 8) & 0xFF);
    buffer[offset + 1] = static_cast<char>(value & 0xFF);
}

static void writeInt16BE(QByteArray &buffer, int offset, qint16 value)
{
    writeUInt16BE(buffer, offset, static_cast<quint16>(value));
}

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

static void writeUInt64BE(QByteArray &buffer, int offset, quint64 value)
{
    writeUInt32BE(buffer, offset, static_cast<quint32>((value >> 32) & 0xFFFFFFFF));
    writeUInt32BE(buffer, offset + 4, static_cast<quint32>(value & 0xFFFFFFFF));
}

static QByteArray ceyearIfPacket(const QVector<QPair<qint16, qint16>> &samples)
{
    QByteArray packet(86 + samples.size() * 4, '\0');
    packet[0] = '\x00';
    packet[1] = '\x0E';
    packet[2] = '\xB2';
    packet[3] = '\x00';
    writeUInt32BE(packet, 12, static_cast<quint32>(packet.size()));
    packet[16] = '\x03';
    packet[17] = '\x85';
    packet[23] = '\x3A';
    packet[30] = static_cast<char>((samples.size() >> 8) & 0xFF);
    packet[31] = static_cast<char>(samples.size() & 0xFF);
    packet[32] = '\x01';
    packet[33] = static_cast<char>(0xAB);
    packet[48] = static_cast<char>(0x80);
    packet[49] = '\x03';
    packet[54] = 'F';
    packet[55] = 'M';

    int offset = 86;
    for (const auto &sample : samples) {
        const quint16 iValue = static_cast<quint16>(sample.first);
        const quint16 qValue = static_cast<quint16>(sample.second);
        packet[offset++] = static_cast<char>(iValue & 0xFF);
        packet[offset++] = static_cast<char>((iValue >> 8) & 0xFF);
        packet[offset++] = static_cast<char>(qValue & 0xFF);
        packet[offset++] = static_cast<char>((qValue >> 8) & 0xFF);
    }
    return packet;
}

static QByteArray ceyearIfPacketWithOptionalHeaderLength(const QVector<QPair<qint16, qint16>> &samples,
                                                         int optionalHeaderLength)
{
    QByteArray packet(28 + optionalHeaderLength + samples.size() * 4, '\0');
    writeUInt32BE(packet, 0, 0x000EB200);
    writeUInt32BE(packet, 12, static_cast<quint32>(packet.size()));
    writeUInt16BE(packet, 16, 901);
    packet[23] = static_cast<char>(optionalHeaderLength);

    int offset = 28 + optionalHeaderLength;
    for (const auto &sample : samples) {
        const quint16 iValue = static_cast<quint16>(sample.first);
        const quint16 qValue = static_cast<quint16>(sample.second);
        packet[offset++] = static_cast<char>(iValue & 0xFF);
        packet[offset++] = static_cast<char>((iValue >> 8) & 0xFF);
        packet[offset++] = static_cast<char>(qValue & 0xFF);
        packet[offset++] = static_cast<char>((qValue >> 8) & 0xFF);
    }
    return packet;
}

static QByteArray ceyearPscPacket(const QVector<qint16> &levels, quint64 lowHz, quint64 highHz)
{
    constexpr int headerLength = 28;
    const int payloadLength = levels.size() * 2 + levels.size() * 4 + levels.size() * 4;
    QByteArray packet(headerLength + payloadLength, '\0');

    writeUInt32BE(packet, 0, 0x000EB200);
    writeUInt16BE(packet, 4, 0x0061);
    writeUInt16BE(packet, 6, 0x0002);
    writeUInt32BE(packet, 12, static_cast<quint32>(packet.size()));
    writeUInt16BE(packet, 16, 1201);
    writeUInt16BE(packet, 18, static_cast<quint16>(payloadLength));
    writeUInt16BE(packet, 20, static_cast<quint16>(levels.size()));
    packet[23] = '\0';
    writeUInt32BE(packet, 24, 0x80220001);

    int offset = headerLength;
    for (qint16 level : levels) {
        writeInt16BE(packet, offset, level);
        offset += 2;
    }

    const quint64 span = levels.size() > 1 ? (highHz - lowHz) : 0;
    for (int i = 0; i < levels.size(); ++i) {
        const quint64 frequencyHz = lowHz + (span * static_cast<quint64>(i)) / static_cast<quint64>(levels.size() - 1);
        writeUInt32BE(packet, offset, static_cast<quint32>(frequencyHz & 0xFFFFFFFF));
        offset += 4;
    }
    for (int i = 0; i < levels.size(); ++i) {
        const quint64 frequencyHz = lowHz + (span * static_cast<quint64>(i)) / static_cast<quint64>(levels.size() - 1);
        writeUInt32BE(packet, offset, static_cast<quint32>((frequencyHz >> 32) & 0xFFFFFFFF));
        offset += 4;
    }

    return packet;
}

void ServiceTests::sampleRecordRoundTripsThroughJson()
{
    SampleRecord record;
    record.sampleId = "pp_iq_seg_001";
    record.sourceType = "local_preprocess";
    record.rawFilePath = "D:/data/input.cap";
    record.sampleFilePath = "D:/data/output/samples/DJI/input.npz";
    record.spectrogramFilePath = "D:/data/output/spectrograms/DJI/pp_iq_seg_001.png";
    record.candidatePreviewFilePath = "D:/data/output/spectrograms/candidate_previews/input_chunk00000_candidates.png";
    record.labelType = "DJI";
    record.labelIndividual = "Drone-01";
    record.sampleRateHz = 12800000.0;
    record.centerFrequencyHz = 1090000000.0;
    record.bandwidthHz = 12000000.0;
    record.snrDb = 18.5;
    record.score = 0.91;
    record.occupancy = 0.42;
    record.dataFormat = "npz_iq_float32";
    record.sampleCount = 4096;
    record.sampleIndex = 2;
    record.windowIndex = 7;
    record.deviceId = "IQ_2025";
    record.startSample = 100;
    record.endSample = 4196;
    record.status = "待复核";
    record.sourceName = "预处理输出";

    const auto restored = SampleRecord::fromJson(record.toJson());
    QCOMPARE(restored.sampleId, record.sampleId);
    QCOMPARE(restored.sampleRateHz, record.sampleRateHz);
    QCOMPARE(restored.spectrogramFilePath, record.spectrogramFilePath);
    QCOMPARE(restored.candidatePreviewFilePath, record.candidatePreviewFilePath);
    QCOMPARE(restored.bandwidthHz, record.bandwidthHz);
    QCOMPARE(restored.sampleIndex, 2);
    QCOMPARE(restored.windowIndex, 7);
    QCOMPARE(restored.status, QString("待复核"));
}

void ServiceTests::trainingRunRecordRoundTripsThroughJson()
{
    TrainingRunRecord record;
    record.runId = QStringLiteral("train-1");
    record.datasetVersionId = QStringLiteral("dataset-v1");
    record.outputDirectory = QStringLiteral("D:/data/exports/training_runs/train-1");
    record.state = QStringLiteral("已完成");
    record.epoch = 10;
    record.epochs = 10;
    record.bestValAccuracy = 91.5;
    record.finalValAccuracy = 90.2;
    record.bestModelPath = QStringLiteral("D:/data/exports/training_runs/train-1/best_model.pth");
    record.cacheDirectory = QStringLiteral("D:/data/cache/training/dataset-v1/legacy");
    record.cacheSummaryPath = QStringLiteral("D:/data/cache/training/dataset-v1/legacy/cache_summary.json");
    record.sampleLength = 4096;
    record.modelMode = QStringLiteral("closed_set");

    const TrainingRunRecord restored = TrainingRunRecord::fromJson(record.toJson());
    QCOMPARE(restored.runId, record.runId);
    QCOMPARE(restored.datasetVersionId, record.datasetVersionId);
    QCOMPARE(restored.state, record.state);
    QCOMPARE(restored.epoch, record.epoch);
    QCOMPARE(restored.bestValAccuracy, record.bestValAccuracy);
    QCOMPARE(restored.bestModelPath, record.bestModelPath);
    QCOMPARE(restored.cacheDirectory, record.cacheDirectory);
    QCOMPARE(restored.cacheSummaryPath, record.cacheSummaryPath);
    QCOMPARE(restored.sampleLength, 4096);
    QCOMPARE(restored.modelMode, QStringLiteral("closed_set"));
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
    version.outputDirectory = "D:/data/exports/dataset_versions/dataset-v1";
    version.manifestPath = "D:/data/exports/dataset_versions/dataset-v1/manifest.jsonl";
    version.selectedLabels = QStringList() << QStringLiteral("DJI") << QStringLiteral("Autel");
    version.includePartial = true;

    AppState state;
    state.samples.append(sample);
    state.datasetVersions.append(version);

    QVERIFY2(store.save(state), qPrintable(store.lastError()));
    const auto loaded = store.load();
    QVERIFY2(loaded.has_value(), qPrintable(store.lastError()));
    QCOMPARE(loaded->samples.size(), 1);
    QCOMPARE(loaded->datasetVersions.size(), 1);
    QCOMPARE(loaded->samples.first().sampleId, QString("sample-1"));
    QCOMPARE(loaded->datasetVersions.first().selectedLabels, QStringList({"DJI", "Autel"}));
    QCOMPARE(loaded->datasetVersions.first().manifestPath, version.manifestPath);
    QVERIFY(loaded->datasetVersions.first().includePartial);
}

void ServiceTests::appStateStoreDoesNotOverwriteInvalidState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("app_state.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{ invalid json");
    file.close();

    AppStateStore store(path);
    QVERIFY(!store.load().has_value());

    AppState replacement;
    QVERIFY(!store.save(replacement));
    QVERIFY(store.lastError().contains(QStringLiteral("拒绝覆盖")));

    QFile preserved(path);
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArray("{ invalid json"));
}

void ServiceTests::projectPathsUsesConfiguredCppAppRootForPortableRuntime()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());

    const QByteArray previousRoot = qgetenv("AIRCRAFT_CPP_APP_ROOT");
    QVERIFY(qputenv("AIRCRAFT_CPP_APP_ROOT", runtimeDir.path().toUtf8()));

    const QString expectedRoot = QFileInfo(runtimeDir.path()).absoluteFilePath();
    QCOMPARE(ProjectPaths::cppAppRoot(), expectedRoot);
    QCOMPARE(ProjectPaths::dataDirectory(), QDir(expectedRoot).filePath(QStringLiteral("data")));

    if (previousRoot.isEmpty()) {
        qunsetenv("AIRCRAFT_CPP_APP_ROOT");
    } else {
        qputenv("AIRCRAFT_CPP_APP_ROOT", previousRoot);
    }
}

void ServiceTests::candidateReviewGroupsAggregateAndIsolateSources()
{
    QVector<SampleRecord> samples;
    SampleRecord first;
    first.rawFilePath = QStringLiteral("D:/capture/a.cap");
    first.batchId = QStringLiteral("batch-1");
    first.candidateId = QStringLiteral("candidate-1");
    first.sampleId = QStringLiteral("sample-1");
    first.sampleFilePath = QStringLiteral("D:/capture/a-1.npz");
    samples.append(first);

    SampleRecord second = first;
    second.sampleId = QStringLiteral("sample-2");
    second.sampleFilePath = QStringLiteral("D:/capture/a-2.npz");
    samples.append(second);

    SampleRecord differentBatch = first;
    differentBatch.batchId = QStringLiteral("batch-2");
    differentBatch.sampleId = QStringLiteral("sample-3");
    differentBatch.sampleFilePath = QStringLiteral("D:/capture/a-3.npz");
    samples.append(differentBatch);

    const QVector<CandidateReviewGroup> groups = CandidateReviewService::buildGroups(samples);
    QCOMPARE(groups.size(), 2);
    QCOMPARE(CandidateReviewService::reviewKey(samples.at(0)),
             CandidateReviewService::reviewKey(samples.at(1)));
    QVERIFY(CandidateReviewService::reviewKey(samples.at(0))
                != CandidateReviewService::reviewKey(samples.at(2)));
    for (const CandidateReviewGroup &group : groups) {
        if (group.sampleIndices.size() == 2) {
            QCOMPARE(group.review.windowCount, 2);
        }
    }
}

void ServiceTests::candidateReviewGroupsSelectRepresentativeWindows()
{
    QVector<SampleRecord> samples;
    for (int index = 0; index < 3; ++index) {
        SampleRecord sample;
        sample.rawFilePath = QStringLiteral("D:/capture/b.cap");
        sample.batchId = QStringLiteral("batch-1");
        sample.candidateId = QStringLiteral("candidate-2");
        sample.sampleId = QStringLiteral("sample-%1").arg(index);
        sample.sampleFilePath = QStringLiteral("D:/capture/b-%1.npz").arg(index);
        sample.score = index == 1 ? 0.95 : (index == 0 ? 0.4 : 0.7);
        samples.append(sample);
    }
    const QVector<CandidateReviewGroup> groups = CandidateReviewService::buildGroups(samples);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().representativeSampleIndices.size(), 3);
    QCOMPARE(groups.first().representativeSampleIndices.first(), 1);
    QCOMPARE(groups.first().review.representativeSampleId, QStringLiteral("sample-1"));
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
    segment["occupancy"] = 0.37;
    segment["detection_mode_detail"] = "cv_band:pulse";
    segment["output_file_path"] = dir.filePath("input.npz");
    segment["status"] = "valid";

    QJsonObject sample;
    sample["sample_id"] = "input_seg00001_w000";
    sample["label"] = "DJI";
    sample["source_file"] = dir.filePath("input.cap");
    sample["sample_file_path"] = dir.filePath("input.npz");
    sample["spectrogram_path"] = dir.filePath("input_seg00001_w000.png");
    sample["sample_index"] = 3;
    sample["window_index"] = 0;
    sample["source_start_sample"] = 10;
    sample["source_end_sample"] = 30;
    sample["valid_samples"] = 4096;
    sample["sample_rate_hz"] = 12800000.0;
    sample["center_frequency_hz"] = 1090000000.0;
    sample["bandwidth_hz"] = 12000000.0;
    sample["snr_db"] = 18.5;
    sample["score"] = 0.91;
    sample["occupancy"] = 0.37;
    sample["data_format"] = "npz_iq_float32";

    QJsonObject root;
    root["success"] = true;
    root["message"] = "处理成功";
    root["detected_segment_count"] = 1;
    root["candidate_segment_count"] = 2;
    root["output_sample_count"] = 1;
    root["source_format"] = "cap";
    root["sample_rate_hz"] = 12800000.0;
    root["center_frequency_hz"] = 1090000000.0;
    root["bandwidth_hz"] = 12000000.0;
    root["segments"] = QJsonArray{segment};
    root["samples"] = QJsonArray{sample};
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
    QCOMPARE(result->segments.first().detectionModeDetail, QStringLiteral("cv_band:pulse"));
    QCOMPARE(result->segments.first().occupancy, 0.37);
    QCOMPARE(result->sampleRecords.size(), 1);
    QCOMPARE(result->sampleRecords.first().sampleIndex, 3);
    QCOMPARE(result->sampleRecords.first().dataFormat, QStringLiteral("npz_iq_float32"));
}

void ServiceTests::preprocessRequestSerializesRealControls()
{
    PreprocessRequest request;
    request.inputFilePath = QStringLiteral("D:/data/input.iqraw");
    request.windowSamples = 8192;
    request.chunkSamples = 3000000;
    request.payloadEndian = QStringLiteral("big");
    request.detectionMode = QStringLiteral("cv_band");
    request.frequencySpanMhz = 14.0;
    request.energyOffsetDb = 12.5;
    request.targetBandwidthMhz = 20.0;
    request.minBandwidthMhz = 16.0;
    request.minWidebandFill = 0.44;
    request.minWidebandSnrDb = 4.0;
    request.morphFrequencyCloseMhz = 1.5;
    request.minBandScore = 0.03;
    request.cvColumnQuantile = 41.0;
    request.cvMinWindowOccupancy = 0.31;
    request.cvMorphFrequencyMhz = 0.4;
    request.cvMorphTimeMs = 0.06;
    request.minDurationMs = 0.2;
    request.mergeGapMs = 0.3;
    request.nperseg = 4096;
    request.noverlap = 2048;
    request.exportSpectrogramPng = true;
    request.spectrogramNperseg = 1024;
    request.spectrogramNoverlap = 768;
    request.computeBackend = QStringLiteral("cuda");
    request.gpuDevice = 0;
    request.allowCpuFallback = false;
    request.sampleOutputDir = QStringLiteral("D:/data/output");

    const QJsonObject json = PreprocessProcessRunner::requestToJson(request);
    QCOMPARE(json.value(QStringLiteral("input_file_path")).toString(), request.inputFilePath);
    QCOMPARE(json.value(QStringLiteral("window_samples")).toInt(), request.windowSamples);
    QCOMPARE(json.value(QStringLiteral("chunk_samples")).toInt(), request.chunkSamples);
    QCOMPARE(json.value(QStringLiteral("payload_endian")).toString(), request.payloadEndian);
    QCOMPARE(json.value(QStringLiteral("detection_mode")).toString(), request.detectionMode);
    QCOMPARE(json.value(QStringLiteral("frequency_span_mhz")).toDouble(), request.frequencySpanMhz);
    QCOMPARE(json.value(QStringLiteral("energy_offset_db")).toDouble(), request.energyOffsetDb);
    QCOMPARE(json.value(QStringLiteral("target_bandwidth_mhz")).toDouble(), request.targetBandwidthMhz);
    QCOMPARE(json.value(QStringLiteral("min_bandwidth_mhz")).toDouble(), request.minBandwidthMhz);
    QCOMPARE(json.value(QStringLiteral("min_wideband_fill")).toDouble(), request.minWidebandFill);
    QCOMPARE(json.value(QStringLiteral("min_wideband_snr_db")).toDouble(), request.minWidebandSnrDb);
    QCOMPARE(json.value(QStringLiteral("morph_frequency_close_mhz")).toDouble(), request.morphFrequencyCloseMhz);
    QCOMPARE(json.value(QStringLiteral("min_band_score")).toDouble(), request.minBandScore);
    QCOMPARE(json.value(QStringLiteral("cv_column_quantile")).toDouble(), request.cvColumnQuantile);
    QCOMPARE(json.value(QStringLiteral("cv_min_window_occupancy")).toDouble(), request.cvMinWindowOccupancy);
    QCOMPARE(json.value(QStringLiteral("cv_morph_frequency_mhz")).toDouble(), request.cvMorphFrequencyMhz);
    QCOMPARE(json.value(QStringLiteral("cv_morph_time_ms")).toDouble(), request.cvMorphTimeMs);
    QCOMPARE(json.value(QStringLiteral("min_duration_ms")).toDouble(), request.minDurationMs);
    QCOMPARE(json.value(QStringLiteral("merge_gap_ms")).toDouble(), request.mergeGapMs);
    QCOMPARE(json.value(QStringLiteral("nperseg")).toInt(), request.nperseg);
    QCOMPARE(json.value(QStringLiteral("noverlap")).toInt(), request.noverlap);
    QCOMPARE(json.value(QStringLiteral("export_spectrogram_png")).toBool(), request.exportSpectrogramPng);
    QCOMPARE(json.value(QStringLiteral("spectrogram_nperseg")).toInt(), request.spectrogramNperseg);
    QCOMPARE(json.value(QStringLiteral("spectrogram_noverlap")).toInt(), request.spectrogramNoverlap);
    QCOMPARE(json.value(QStringLiteral("compute_backend")).toString(), request.computeBackend);
    QCOMPARE(json.value(QStringLiteral("gpu_device")).toInt(), request.gpuDevice);
    QCOMPARE(json.value(QStringLiteral("allow_cpu_fallback")).toBool(), request.allowCpuFallback);
    QCOMPARE(json.value(QStringLiteral("sample_output_dir")).toString(), request.sampleOutputDir);
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

    const QString outputSamplePath = QDir(dir.filePath("out")).filePath("samples/DJI/input.npz");
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
sample_path = pathlib.Path(request["sample_output_dir"], "samples", "DJI", "input.npz")
sample_path.parent.mkdir(parents=True, exist_ok=True)
sample_path.write_bytes(b"npz")
response = {
    "success": True,
    "message": "处理成功",
    "detected_segment_count": 1,
    "candidate_segment_count": 1,
    "output_sample_count": 1,
    "sample_output_dir": request["sample_output_dir"],
    "source_format": "cap",
    "sample_rate_hz": 12800000.0,
    "center_frequency_hz": 1090000000.0,
    "bandwidth_hz": 12000000.0,
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
            "occupancy": 0.37,
            "detection_mode_detail": "cv_band:pulse",
            "output_file_path": str(sample_path),
            "status": "valid",
        }
    ],
    "samples": [
        {
            "sample_id": "input_seg00001_w000",
            "label": "DJI",
            "source_file": request["input_file_path"],
            "sample_file_path": str(sample_path),
            "sample_index": 0,
            "window_index": 0,
            "source_start_sample": 10,
            "source_end_sample": 20,
            "valid_samples": 4096,
            "sample_rate_hz": 12800000.0,
            "center_frequency_hz": 1090000000.0,
            "bandwidth_hz": 12000000.0,
            "snr_db": 18.5,
            "score": 0.91,
            "occupancy": 0.37,
            "data_format": "npz_iq_float32",
            "status": "待复核",
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
    QCOMPARE(result->sampleRecords.first().sampleIndex, 0);
    QCOMPARE(result->sampleRecords.first().dataFormat, QStringLiteral("npz_iq_float32"));
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

void ServiceTests::preprocessSampleRecordsKeepNpzWindowMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString outputPath = dir.filePath("input.npz");
    QFile sample(outputPath);
    QVERIFY(sample.open(QIODevice::WriteOnly | QIODevice::Truncate));
    sample.write("npz");
    sample.close();

    SampleRecord sampleRecord;
    sampleRecord.sampleId = QStringLiteral("input_seg00001_w000");
    sampleRecord.rawFilePath = dir.filePath("input.iqraw");
    sampleRecord.sampleFilePath = outputPath;
    sampleRecord.spectrogramFilePath = dir.filePath("input_seg00001_w000.png");
    sampleRecord.candidatePreviewFilePath = dir.filePath("input_chunk00000_candidates.png");
    sampleRecord.labelType = QStringLiteral("DJI");
    sampleRecord.sampleRateHz = 12800000.0;
    sampleRecord.centerFrequencyHz = 1090000000.0;
    sampleRecord.bandwidthHz = 12000000.0;
    sampleRecord.snrDb = 18.5;
    sampleRecord.score = 0.91;
    sampleRecord.occupancy = 0.37;
    sampleRecord.dataFormat = QStringLiteral("npz_iq_float32");
    sampleRecord.sampleCount = 4096;
    sampleRecord.sampleIndex = 4;
    sampleRecord.windowIndex = 2;
    sampleRecord.status = QStringLiteral("待复核");

    PreprocessRunResult result;
    result.success = true;
    result.sampleRecords.append(sampleRecord);

    CapProbeResult capInfo;
    const auto records = PreprocessProcessRunner::buildSampleRecords(dir.filePath("input.iqraw"), capInfo, result);

    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().sampleCount, 4096);
    QCOMPARE(records.first().sampleIndex, 4);
    QCOMPARE(records.first().windowIndex, 2);
    QCOMPARE(records.first().bandwidthHz, 12000000.0);
    QCOMPARE(records.first().score, 0.91);
    QCOMPARE(records.first().spectrogramFilePath, sampleRecord.spectrogramFilePath);
    QCOMPARE(records.first().candidatePreviewFilePath, sampleRecord.candidatePreviewFilePath);
}

void ServiceTests::signalCleaningStartupFailureFinishesOnceWithoutCrashing()
{
    QTemporaryDir directory(QDir(ProjectPaths::dataDirectory())
                                .filePath(QStringLiteral("signal-cleaning-start-XXXXXX")));
    QVERIFY(directory.isValid());

    const QString modelPath = directory.filePath(QStringLiteral("model.pt"));
    QFile modelFile(modelPath);
    QVERIFY(modelFile.open(QIODevice::WriteOnly));
    modelFile.close();

    const QString backendPath = directory.filePath(QStringLiteral("backend.py"));
    QFile backendFile(backendPath);
    QVERIFY(backendFile.open(QIODevice::WriteOnly | QIODevice::Text));
    backendFile.write("raise RuntimeError('must not execute')\n");
    backendFile.close();

    const QString inputRoot = directory.filePath(QStringLiteral("input"));
    QVERIFY(QDir().mkpath(inputRoot));

    SignalCleaningRequest request;
    request.modelPath = modelPath;
    request.backendScriptPath = backendPath;
    request.inputRoot = inputRoot;
    request.outputDirectory = directory.filePath(QStringLiteral("output"));

    SignalCleaningProcessRunner runner;
    QSignalSpy failedSpy(&runner, &SignalCleaningProcessRunner::failed);
    QSignalSpy finishedSpy(&runner, &SignalCleaningProcessRunner::finished);

    QVERIFY(!runner.start(request, directory.filePath(QStringLiteral("missing-python.exe"))));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(finishedSpy.count(), 1);
    QVERIFY(!runner.isRunning());
}

void ServiceTests::signalCleaningRejectsHeaderOnlyResultCsv()
{
    QTemporaryDir directory(QDir(ProjectPaths::dataDirectory())
                                .filePath(QStringLiteral("signal-cleaning-invalid-result-XXXXXX")));
    QVERIFY(directory.isValid());

    const QString pythonPath = QStandardPaths::findExecutable(QStringLiteral("python"));
    QVERIFY2(!pythonPath.isEmpty(), "Python is required for the signal-cleaning result validation test.");

    const QString modelPath = directory.filePath(QStringLiteral("model.pt"));
    QFile modelFile(modelPath);
    QVERIFY(modelFile.open(QIODevice::WriteOnly));
    modelFile.close();

    const QString backendPath = directory.filePath(QStringLiteral("header_only_backend.py"));
    QFile backendFile(backendPath);
    QVERIFY(backendFile.open(QIODevice::WriteOnly | QIODevice::Text));
    backendFile.write(R"(import json
import sys
from pathlib import Path

output = Path(sys.argv[sys.argv.index("--output-csv") + 1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text("path,index,sample_id,group,prob_signal,prediction\\n", encoding="utf-8")
output.with_suffix(".json").write_text(json.dumps({"records": 0, "threshold": 0.5}), encoding="utf-8")
)");
    backendFile.close();

    const QString inputRoot = directory.filePath(QStringLiteral("input"));
    QVERIFY(QDir().mkpath(inputRoot));

    SignalCleaningRequest request;
    request.modelPath = modelPath;
    request.backendScriptPath = backendPath;
    request.inputRoot = inputRoot;
    request.outputDirectory = directory.filePath(QStringLiteral("output"));

    SignalCleaningProcessRunner runner;
    QSignalSpy finishedSpy(&runner, &SignalCleaningProcessRunner::finished);
    QVERIFY(runner.start(request, pythonPath));
    QTRY_COMPARE(finishedSpy.count(), 1);

    const SignalCleaningResult result = qvariant_cast<SignalCleaningResult>(finishedSpy.takeFirst().at(0));
    QVERIFY(!result.success);
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

    for (const auto &query : queries) {
        QVERIFY2(query.readOnly, query.label.constData());
        const auto length = std::strlen(query.command.constData());
        QVERIFY2(length > 0, query.label.constData());
        QVERIFY2(query.command.constData()[length - 1] == '?', query.command.constData());
        QVERIFY2(std::strchr(query.command.constData(), '[') == nullptr, query.command.constData());
        QVERIFY2(std::strchr(query.command.constData(), ']') == nullptr, query.command.constData());
    }

    const auto hasCommand = [&](const char *command) {
        return std::any_of(queries.cbegin(), queries.cend(), [&](const CeyearVisaQuery &query) {
            return std::strcmp(query.command.constData(), command) == 0;
        });
    };

    QVERIFY(hasCommand("*IDN?"));
    QVERIFY(hasCommand("SYSTem:VERSion?"));
    QVERIFY(hasCommand("MEASure:MODE?"));
    QVERIFY(hasCommand("FREQuency?"));
    QVERIFY(hasCommand("FREQuency:SPAN?"));
    QVERIFY(hasCommand("DEModulation?"));
    QVERIFY(hasCommand("DETector?"));
    QVERIFY(hasCommand("SYSTem:ERRor?"));
}

void ServiceTests::ceyearControlCommandsCoverApprovedWrites()
{
    CeyearControlConfig config;
    config.centerFrequencyHz = 109000000.0;
    config.frequencySpanHz = 20000000.0;
    config.demodulation = QStringLiteral("FM");
    config.detector = QStringLiteral("PEAK");

    const auto commands = defaultCeyearControlCommands(config);
    QCOMPARE(commands.size(), std::size_t{4});

    for (const auto &command : commands) {
        QVERIFY2(!command.readOnly, command.label.constData());
        const auto length = std::strlen(command.command.constData());
        QVERIFY2(length > 0, command.label.constData());
        QVERIFY2(command.command.constData()[length - 1] != '?', command.command.constData());
        QVERIFY2(std::strchr(command.command.constData(), '[') == nullptr, command.command.constData());
        QVERIFY2(std::strchr(command.command.constData(), ']') == nullptr, command.command.constData());
    }

    const auto hasCommandPrefix = [&](const char *prefix) {
        return std::any_of(commands.cbegin(), commands.cend(), [&](const CeyearVisaQuery &command) {
            return std::strncmp(command.command.constData(), prefix, std::strlen(prefix)) == 0;
        });
    };

    QVERIFY(hasCommandPrefix("FREQuency "));
    QVERIFY(hasCommandPrefix("FREQuency:SPAN "));
    QVERIFY(hasCommandPrefix("DEModulation "));
    QVERIFY(hasCommandPrefix("DETector:FUNCtion "));
}

void ServiceTests::ceyearCaptureCommandsStartInstrumentAcquisition()
{
    CeyearCaptureConfig config;
    config.centerFrequencyHz = 109000000.0;
    config.frequencySpanHz = 20000000.0;
    config.iqPointCount = 2048;
    config.recordEnabled = true;
    config.recordSource = QStringLiteral("IQ");
    config.reportPath = QStringLiteral("TCP");
    config.pcReceiveProtocol = QStringLiteral("TCP");
    config.pcReceiveIpAddress = QStringLiteral("192.168.1.20");
    config.pcReceivePort = 5030;

    const auto preferredCommands = preferredCeyearCaptureCommands(config);
    QVERIFY(preferredCommands.size() >= std::size_t{13});

    const auto hasExactCommand = [&](const std::vector<CeyearVisaQuery> &commands, const char *command) {
        return std::any_of(commands.cbegin(), commands.cend(), [&](const CeyearVisaQuery &item) {
            return std::strcmp(item.command.constData(), command) == 0;
        });
    };
    const auto hasCommandPrefix = [&](const std::vector<CeyearVisaQuery> &commands, const char *prefix) {
        return std::any_of(commands.cbegin(), commands.cend(), [&](const CeyearVisaQuery &item) {
            return std::strncmp(item.command.constData(), prefix, std::strlen(prefix)) == 0;
        });
    };
    const auto commandIndex = [&](const std::vector<CeyearVisaQuery> &commands, const char *prefix) {
        const auto it = std::find_if(commands.cbegin(), commands.cend(), [&](const CeyearVisaQuery &item) {
            return std::strncmp(item.command.constData(), prefix, std::strlen(prefix)) == 0;
        });
        return it == commands.cend() ? -1 : static_cast<int>(std::distance(commands.cbegin(), it));
    };

    QVERIFY(hasCommandPrefix(preferredCommands, "FREQuency "));
    QVERIFY(!hasCommandPrefix(preferredCommands, "FREQuency:SPAN "));
    QVERIFY(hasExactCommand(preferredCommands, "INPut:ATTenuation:STATe AUTO"));
    QVERIFY(hasExactCommand(preferredCommands, "BANDwidth 20MHz"));
    QVERIFY(hasExactCommand(preferredCommands, "TRACe:RECord:SOURce IQ"));
    QVERIFY(hasExactCommand(preferredCommands, "TRACe:IQ:DATA:NUMBer 2048"));
    QVERIFY(hasExactCommand(preferredCommands, "FREQuency:MODE FFM"));
    QVERIFY(hasExactCommand(preferredCommands, "TRACe:TCP:FLAG \"192.168.1.20\",5030,\"opt\""));
    QVERIFY(hasExactCommand(preferredCommands, "TRACe:TCP:TAG \"192.168.1.20\",5030,IF"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:SELectivity REPort"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:STATe OFF"));
    QVERIFY(hasExactCommand(preferredCommands, "TRACe:IQ:DATA:REPort MAN"));
    QVERIFY(hasExactCommand(preferredCommands, "TRIGger:IQ:REPort:STATe ON"));
    QVERIFY(hasExactCommand(preferredCommands, "SYSTem:IF:REMote:MODE SHORT"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:IQ:DATA:REPort FREE"));
    QVERIFY(!hasExactCommand(preferredCommands, "INITiate"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:STOP"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:STORage FILE"));
    QVERIFY(hasCommandPrefix(preferredCommands, "TRACe:PATH "));
    QVERIFY(!hasExactCommand(preferredCommands, "INITiate:IMMediate"));
    QVERIFY(!hasCommandPrefix(preferredCommands, "TRACe:RECord:STARt"));
    QVERIFY(!hasCommandPrefix(preferredCommands, "TRIGger:SEQuence:ENABle ON"));
    QVERIFY(commandIndex(preferredCommands, "TRACe:IQ:DATA:REPort ") < commandIndex(preferredCommands, "TRACe:RECord:SOURce "));
    QVERIFY(commandIndex(preferredCommands, "SYSTem:IF:REMote:MODE ") < commandIndex(preferredCommands, "TRACe:IQ:DATA:NUMBer "));
    QVERIFY(commandIndex(preferredCommands, "TRACe:IQ:DATA:NUMBer ") < commandIndex(preferredCommands, "TRACe:PATH "));
    QVERIFY(commandIndex(preferredCommands, "TRACe:PATH ") < commandIndex(preferredCommands, "TRACe:TCP:FLAG "));
    QVERIFY(commandIndex(preferredCommands, "TRACe:TCP:FLAG ") < commandIndex(preferredCommands, "TRACe:TCP:TAG "));
    QVERIFY(commandIndex(preferredCommands, "TRACe:IQ:DATA:REPort ") < commandIndex(preferredCommands, "TRIGger:IQ:REPort:STATe "));

    CeyearCaptureConfig initiatedConfig = config;
    initiatedConfig.sendInitiate = true;
    const auto initiatedCommands = preferredCeyearCaptureCommands(initiatedConfig);
    QVERIFY(!hasExactCommand(initiatedCommands, "INITiate"));

    const auto fallbackCommands = defaultCeyearCaptureCommands(config);
    QVERIFY(hasExactCommand(fallbackCommands, "FREQuency:MODE FFM"));
    QVERIFY(hasExactCommand(fallbackCommands, "INPut:ATTenuation:STATe AUTO"));
    QVERIFY(hasExactCommand(fallbackCommands, "BANDwidth 20MHz"));
    QVERIFY(hasExactCommand(fallbackCommands, "TRACe:RECord:SOURce IQ"));
    QVERIFY(hasExactCommand(fallbackCommands, "TRACe:TCP:FLAG \"192.168.1.20\",5030,\"opt\""));
    QVERIFY(hasExactCommand(fallbackCommands, "TRACe:TCP:TAG \"192.168.1.20\",5030,IF"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:SELectivity REPort"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:STATe OFF"));
    QVERIFY(hasExactCommand(fallbackCommands, "TRACe:IQ:DATA:REPort MAN"));
    QVERIFY(hasExactCommand(fallbackCommands, "TRIGger:IQ:REPort:STATe ON"));
    QVERIFY(hasExactCommand(fallbackCommands, "SYSTem:IF:REMote:MODE SHORT"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:IQ:DATA:REPort FREE"));
    QVERIFY(!hasExactCommand(fallbackCommands, "INITiate"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:STOP"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:STORage FILE"));
    QVERIFY(hasCommandPrefix(fallbackCommands, "TRACe:PATH "));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:DATA:RECord:STARt"));
    QVERIFY(!hasCommandPrefix(fallbackCommands, "TRACe:RECord:STARt"));
    QVERIFY(!hasCommandPrefix(fallbackCommands, "TRIGger:SEQuence:ENABle ON"));
    QVERIFY(commandIndex(fallbackCommands, "TRACe:IQ:DATA:REPort ") < commandIndex(fallbackCommands, "TRACe:RECord:SOURce "));
    QVERIFY(commandIndex(fallbackCommands, "SYSTem:IF:REMote:MODE ") < commandIndex(fallbackCommands, "TRACe:IQ:DATA:NUMBer "));
    QVERIFY(commandIndex(fallbackCommands, "TRACe:IQ:DATA:NUMBer ") < commandIndex(fallbackCommands, "TRACe:PATH "));
    QVERIFY(commandIndex(fallbackCommands, "TRACe:PATH ") < commandIndex(fallbackCommands, "TRACe:TCP:FLAG "));
    QVERIFY(commandIndex(fallbackCommands, "TRACe:TCP:FLAG ") < commandIndex(fallbackCommands, "TRACe:TCP:TAG "));
    QVERIFY(commandIndex(fallbackCommands, "TRACe:IQ:DATA:REPort ") < commandIndex(fallbackCommands, "TRIGger:IQ:REPort:STATe "));

    for (const auto &command : preferredCommands) {
        QVERIFY2(!command.readOnly, command.label.constData());
        QVERIFY2(std::strchr(command.command.constData(), '[') == nullptr, command.command.constData());
        QVERIFY2(std::strchr(command.command.constData(), ']') == nullptr, command.command.constData());
    }

    CeyearCaptureConfig defaultConfig;
    QCOMPARE(defaultConfig.measureMode, QStringLiteral("FIX"));

    const auto statusQueries = defaultCeyearCaptureStatusQueries();
    const auto hasStatusCommand = [&](const char *command) {
        return std::any_of(statusQueries.cbegin(), statusQueries.cend(), [&](const CeyearVisaQuery &item) {
            return std::strcmp(item.command.constData(), command) == 0;
        });
    };
    QVERIFY(!hasStatusCommand("MEASure:MODE?"));
    QVERIFY(hasStatusCommand("FREQuency:MODE?"));
    QVERIFY(hasStatusCommand("SYSTem:IF:REMote:MODE?"));
    QVERIFY(hasStatusCommand("TRACe:PATH?"));
    QVERIFY(hasStatusCommand("TRACe:RECord:STATe?"));
    QVERIFY(hasStatusCommand("TRACe:RECord:SELectivity?"));
    QVERIFY(hasStatusCommand("TRACe:IQ:DATA:REPort?"));
    QVERIFY(hasStatusCommand("TRACe:TCP?"));
    QVERIFY(hasStatusCommand("TRACe:UDP?"));
    QVERIFY(!hasStatusCommand("TRACe:DATA:PATH?"));
    QVERIFY(!hasStatusCommand("TRACe:DATA:RECord:STATe?"));
}

void ServiceTests::ceyearIfIqProbeQueriesAreReadOnlyAndCoverManualState()
{
    const auto queries = defaultCeyearIfIqProbeQueries();
    QVERIFY(queries.size() >= std::size_t{12});

    const auto hasCommand = [&](const char *command) {
        return std::any_of(queries.cbegin(), queries.cend(), [&](const CeyearVisaQuery &query) {
            return std::strcmp(query.command.constData(), command) == 0;
        });
    };

    for (const auto &query : queries) {
        QVERIFY2(query.readOnly, query.label.constData());
        QVERIFY2(query.command.endsWith('?'), query.command.constData());
    }

    QVERIFY(hasCommand("*IDN?"));
    QVERIFY(hasCommand("SYSTem:ERRor?"));
    QVERIFY(hasCommand("FREQuency:MODE?"));
    QVERIFY(hasCommand("FREQuency?"));
    QVERIFY(hasCommand("FREQuency:SPAN?"));
    QVERIFY(hasCommand("MEASure:MODE?"));
    QVERIFY(hasCommand("SYSTem:IF:REMote:MODE?"));
    QVERIFY(!hasCommand("TRACe:IQ:DATA:NUMBer?"));
    QVERIFY(hasCommand("TRACe:IQ:DATA:REPort?"));
    QVERIFY(hasCommand("TRACe:RECord:SELectivity?"));
    QVERIFY(hasCommand("TRACe:PATH?"));
    QVERIFY(hasCommand("TRACe:UDP?"));
    QVERIFY(hasCommand("TRACe:TCP?"));

    const auto captureQueries = defaultCeyearCaptureStatusQueries();
    auto hasCaptureCommand = [&](const QByteArray &command) {
        return std::any_of(captureQueries.cbegin(), captureQueries.cend(), [&](const CeyearVisaQuery &query) {
            return query.command == command;
        });
    };

    QVERIFY(!hasCaptureCommand("TRACe:RECord:STARt?"));
    QVERIFY(!hasCaptureCommand("TRACe:RECord:STOP?"));
    QVERIFY(hasCaptureCommand("TRACe:RECord:STATe?"));
    QVERIFY(hasCaptureCommand("TRACe:RECord:SOURce?"));
    QVERIFY(hasCaptureCommand("TRACe:IQ:DATA:REPort?"));
    QVERIFY(hasCaptureCommand("SYSTem:ERRor?"));
}

void ServiceTests::ceyearCaptureRiskClassificationBlocksKnownHazards()
{
    CeyearCaptureConfig config;
    config.pcReceiveProtocol = QStringLiteral("UDP");
    config.reportPath = QStringLiteral("UDP");
    config.pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    config.pcReceivePort = 5030;

    const QStringList lowRisk = ceyearLowRiskPathSetupCommandLines(config);
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:UDP:DEL ALL")));
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:TCP:DEL ALL")));
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:UDP:TAG \"192.168.50.20\",5030,IF")));
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:UDP:FLAG \"192.168.50.20\",5030,\"opt\"")));
    QVERIFY(lowRisk.contains(QStringLiteral("TRACe:PATH UDP")));
    QVERIFY(!lowRisk.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!lowRisk.contains(QStringLiteral("INITiate")));
    QVERIFY(!lowRisk.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));

    const QStringList risky = ceyearRiskyStreamStartCommandLines(config);
    QVERIFY(risky.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
    QVERIFY(risky.contains(QStringLiteral("TRACe:RECord:SELectivity REPort")));
    QVERIFY(risky.contains(QStringLiteral("TRACe:RECord:STATe OFF")));
    QVERIFY(risky.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(risky.contains(QStringLiteral("TRIGger:IQ:REPort:STATe ON")));
    QVERIFY(!risky.contains(QStringLiteral("INITiate")));

    config.sendInitiate = true;
    const QStringList riskyWithInitiate = ceyearRiskyStreamStartCommandLines(config);
    QVERIFY(riskyWithInitiate.contains(QStringLiteral("INITiate")));
}

void ServiceTests::ceyearManualIqReportCommandsSetPointCount()
{
    CeyearCaptureConfig config;
    config.pcReceiveProtocol = QStringLiteral("UDP");
    config.reportPath = QStringLiteral("UDP");
    config.pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    config.pcReceivePort = 5030;
    config.recordSource = QStringLiteral("IQAudio");
    config.iqPointCount = 4096;

    const QStringList lines = ceyearManualIqReportCommandLines(config);
    QVERIFY(lines.contains(QStringLiteral("TRACe:UDP:DEL ALL")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:TCP:DEL ALL")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:UDP:TAG \"192.168.50.20\",5030,IF")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:UDP:FLAG \"192.168.50.20\",5030,\"opt\"")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:PATH UDP")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:RECord:SELectivity REPort")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:RECord:STATe OFF")));
    QVERIFY(lines.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:IQ:DATA:NUMBer 4096")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:IQ:DATA:REPort MAN")));
    QVERIFY(lines.contains(QStringLiteral("TRIGger:IQ:REPort:STATe ON")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!lines.contains(QStringLiteral("INITiate")));

    const auto startsWith = [&](const QString &prefix) {
        return std::any_of(lines.cbegin(), lines.cend(), [&](const QString &line) {
            return line.startsWith(prefix);
        });
    };
    QVERIFY(!startsWith(QStringLiteral("TRIGger:SEQuence:ENABle")));
    QVERIFY(!startsWith(QStringLiteral("TRACe:RECord:STARt")));
}

void ServiceTests::ceyearManualIqReportRequiresExplicitAcknowledgement()
{
    QVERIFY(!ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(false, false, false));
    QVERIFY(!ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(true, false, false));
    QVERIFY(ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(true, true, false));
    QVERIFY(!ceyearCaptureSmokeRequiresManualIqReportAcknowledgement(true, true, true));
}

void ServiceTests::ceyearManualIqReportRequiresRebootObservedAcknowledgement()
{
    QVERIFY(!ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(false, false, false));
    QVERIFY(!ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(true, false, false));
    QVERIFY(ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(true, true, false));
    QVERIFY(!ceyearCaptureSmokeRequiresRebootObservedIfShortAcknowledgement(true, true, true));
}

void ServiceTests::ceyearTriggerIqReportCommandsMatchVendorManualSequence()
{
    CeyearCaptureConfig config;
    config.pcReceiveProtocol = QStringLiteral("UDP");
    config.reportPath = QStringLiteral("UDP");
    config.pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    config.pcReceivePort = 5030;
    config.recordSource = QStringLiteral("IQAudio");
    config.measureMode = QStringLiteral("FIX");
    config.iqPointCount = 1000000;

    const QStringList lines = ceyearTriggerIqReportCommandLines(config);
    const QStringList stopLines = ceyearTriggerIqReportStopCommandLines();

    QCOMPARE(lines.size(), 14);
    QCOMPARE(lines.at(0), QStringLiteral("TRACe:UDP:DEL ALL"));
    QCOMPARE(lines.at(1), QStringLiteral("TRACe:TCP:DEL ALL"));
    QVERIFY(lines.contains(QStringLiteral("TRACe:UDP:DEL ALL")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:TCP:DEL ALL")));
    QVERIFY(lines.contains(QStringLiteral("FREQuency:MODE FFM")));
    QVERIFY(lines.contains(QStringLiteral("INPut:ATTenuation:STATe AUTO")));
    QVERIFY(lines.contains(QStringLiteral("FREQuency 2400MHz")));
    QVERIFY(lines.contains(QStringLiteral("BANDwidth 20MHz")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:RECord:SOURce IQAudio")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:IQ:DATA:NUMBer 1000000")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:UDP:TAG \"192.168.50.20\",5030,IF")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:UDP:FLAG \"192.168.50.20\",5030,\"opt\"")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:PATH UDP")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:RECord:STATe OFF")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:RECord:SELectivity REPort")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:IQ:DATA:NUMBer 256")));
    QVERIFY(lines.contains(QStringLiteral("TRACe:IQ:DATA:REPort MAN")));
    QVERIFY(lines.contains(QStringLiteral("TRIGger:IQ:REPort:STATe ON")));

    QVERIFY(lines.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
    QVERIFY(!lines.contains(QStringLiteral("SYSTem:IF:REMote:MODE LONG")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!lines.contains(QStringLiteral("INITiate")));
    QVERIFY(!lines.contains(QStringLiteral("TRACe:RECord:STARt")));

    QCOMPARE(stopLines, (QStringList{
                            QStringLiteral("TRACe:UDP:DEL ALL"),
                            QStringLiteral("TRACe:TCP:DEL ALL"),
                        }));
}

void ServiceTests::ceyearStopCommandsStopRecordingAndCloseTrigger()
{
    const auto preferredCommands = preferredCeyearStopCommands();
    const auto fallbackCommands = defaultCeyearStopCommands();

    const auto hasExactCommand = [](const std::vector<CeyearVisaQuery> &commands, const char *command) {
        return std::any_of(commands.cbegin(), commands.cend(), [&](const CeyearVisaQuery &item) {
            return std::strcmp(item.command.constData(), command) == 0;
        });
    };

    QVERIFY(hasExactCommand(preferredCommands, "TRACe:UDP:DEL ALL"));
    QVERIFY(hasExactCommand(preferredCommands, "TRACe:TCP:DEL ALL"));
    QVERIFY(!hasExactCommand(preferredCommands, "SYSTem:IF:REMote:MODE OFF"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:SELectivity OFF"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:STOP"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRACe:RECord:STATe OFF"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRIGger:IQ:REPort:STATe OFF"));
    QVERIFY(!hasExactCommand(preferredCommands, "TRIGger:SEQuence:ENABle OFF"));
    QVERIFY(hasExactCommand(fallbackCommands, "TRACe:UDP:DEL ALL"));
    QVERIFY(hasExactCommand(fallbackCommands, "TRACe:TCP:DEL ALL"));
    QVERIFY(!hasExactCommand(fallbackCommands, "SYSTem:IF:REMote:MODE OFF"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:SELectivity OFF"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:STOP"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRACe:RECord:STATe OFF"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRIGger:IQ:REPort:STATe OFF"));
    QVERIFY(!hasExactCommand(fallbackCommands, "TRIGger:SEQuence:ENABle OFF"));
}

void ServiceTests::ceyearCaptureDetectsScpiCommandErrorsForFallback()
{
    CeyearCaptureResult result;

    CeyearStatusItem benign;
    benign.label = QStringLiteral("Record source");
    benign.command = QStringLiteral("TRACe:RECord:SOURce?");
    benign.response = QStringLiteral("IQ");
    benign.success = true;
    result.items.append(benign);

    CeyearStatusItem scpiError;
    scpiError.label = QStringLiteral("System error");
    scpiError.command = QStringLiteral("SYSTem:ERRor?");
    scpiError.response = QStringLiteral(" 402 CMD ERR");
    scpiError.success = true;
    result.items.append(scpiError);

    QVERIFY(ceyearStatusContainsCommandError(result));
}

void ServiceTests::ceyearStatusAggregationAllowsRepeatedSuccessfulQueryBatches()
{
    QVector<CeyearStatusItem> items;

    CeyearStatusItem first;
    first.command = QStringLiteral("TRACe:RECord:STATe?");
    first.response = QStringLiteral("1");
    first.success = true;
    items.append(first);

    CeyearStatusItem second;
    second.command = QStringLiteral("SYSTem:ERRor?");
    second.response = QStringLiteral("00");
    second.success = true;
    items.append(second);

    QVERIFY(ceyearStatusItemsAllSucceeded(items));

    CeyearStatusItem third;
    third.command = QStringLiteral("TRACe:RECord:STATe?");
    third.response = QStringLiteral("0");
    third.success = true;
    items.append(third);

    CeyearStatusItem fourth;
    fourth.command = QStringLiteral("SYSTem:ERRor?");
    fourth.response = QStringLiteral("00");
    fourth.success = true;
    items.append(fourth);

    QVERIFY(ceyearStatusItemsAllSucceeded(items));
}

void ServiceTests::ceyearIfIqParserExtractsLittleEndianSamplesFromPacketStream()
{
    QByteArray raw;
    raw.append(ceyearIfPacket({{qint16(1), qint16(-1)}, {qint16(256), qint16(-256)}}));
    raw.append(ceyearIfPacket({{qint16(1024), qint16(-1024)}}));

    QString error;
    const auto samples = CeyearIfIqPacketParser::extractSamples(raw, &error);

    QVERIFY2(!samples.isEmpty(), qPrintable(error));
    QCOMPARE(samples.size(), 3);
    QCOMPARE(samples.at(0).first, qint16(1));
    QCOMPARE(samples.at(0).second, qint16(-1));
    QCOMPARE(samples.at(1).first, qint16(256));
    QCOMPARE(samples.at(1).second, qint16(-256));
    QCOMPARE(samples.at(2).first, qint16(1024));
    QCOMPARE(samples.at(2).second, qint16(-1024));

    raw = ceyearIfPacketWithOptionalHeaderLength({{qint16(7), qint16(-7)}}, 12);
    error.clear();
    const auto shortHeaderSamples = CeyearIfIqPacketParser::extractSamples(raw, &error);
    QVERIFY2(!shortHeaderSamples.isEmpty(), qPrintable(error));
    QCOMPARE(shortHeaderSamples.size(), 1);
    QCOMPARE(shortHeaderSamples.first().first, qint16(7));
    QCOMPARE(shortHeaderSamples.first().second, qint16(-7));

    raw = ceyearIfPacket({{qint16(9), qint16(-9)}});
    QByteArray truncated = ceyearIfPacket({{qint16(10), qint16(-10)}});
    truncated.truncate(32);
    raw.append(truncated);
    error.clear();
    const auto trailingPartialSamples = CeyearIfIqPacketParser::extractSamples(raw, &error);
    QVERIFY2(!trailingPartialSamples.isEmpty(), qPrintable(error));
    QCOMPARE(trailingPartialSamples.size(), 1);
    QCOMPARE(trailingPartialSamples.first().first, qint16(9));
    QCOMPARE(trailingPartialSamples.first().second, qint16(-9));
}

void ServiceTests::ceyearIfIqRawValidationRejectsPscAndEmptyCaptures()
{
    CeyearIfIqRawSummary summary;
    QString error;

    QVERIFY(CeyearIfIqPacketParser::validateRawBytes(
        ceyearIfPacket({{qint16(1), qint16(-1)}, {qint16(2), qint16(-2)}}), &summary, &error));
    QCOMPARE(summary.packetCount, 1);
    QCOMPARE(summary.ifPacketCount, 1);
    QCOMPARE(summary.nonIfPacketCount, 0);
    QCOMPARE(summary.firstPacketHeader, 901);
    QCOMPARE(summary.sampleCount, 2);

    QByteArray rawWithTrailingPartial = ceyearIfPacket({{qint16(3), qint16(-3)}});
    QByteArray partialPacket = ceyearIfPacket({{qint16(4), qint16(-4)}});
    partialPacket.truncate(32);
    rawWithTrailingPartial.append(partialPacket);
    summary = CeyearIfIqRawSummary{};
    error.clear();
    QVERIFY(CeyearIfIqPacketParser::validateRawBytes(rawWithTrailingPartial, &summary, &error));
    QCOMPARE(summary.packetCount, 1);
    QCOMPARE(summary.ifPacketCount, 1);
    QCOMPARE(summary.sampleCount, 1);
    QCOMPARE(summary.trailingBytes, qint64(32));

    summary = CeyearIfIqRawSummary{};
    error.clear();
    QVERIFY(!CeyearIfIqPacketParser::validateRawBytes(
        ceyearPscPacket({qint16(-700), qint16(-350)}, 900000000ULL, 901000000ULL), &summary, &error));
    QCOMPARE(summary.packetCount, 1);
    QCOMPARE(summary.ifPacketCount, 0);
    QCOMPARE(summary.nonIfPacketCount, 1);
    QCOMPARE(summary.firstPacketHeader, 1201);
    QVERIFY(error.contains(QStringLiteral("PSCAN")));
    QVERIFY(error.contains(QStringLiteral("1201")));

    summary = CeyearIfIqRawSummary{};
    error.clear();
    QVERIFY(!CeyearIfIqPacketParser::validateRawBytes(QByteArray(), &summary, &error));
    QCOMPARE(summary.bytesRead, qint64(0));
    QVERIFY(error.contains(QStringLiteral("empty")));
}

void ServiceTests::ceyearIfIqRawPrefixValidationAvoidsFullFileScan()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString rawPath = dir.filePath(QStringLiteral("prefix.iqraw"));

    QFile raw(rawPath);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    const QByteArray ifPacket = ceyearIfPacket({{qint16(1), qint16(-1)}, {qint16(2), qint16(-2)}});
    raw.write(ifPacket);
    raw.write(QByteArray(4 * 1024 * 1024, '\0'));
    raw.close();

    CeyearIfIqRawSummary summary;
    QString error;
    QVERIFY2(CeyearIfIqPacketParser::validateRawFilePrefix(rawPath, &summary, &error, ifPacket.size() + 64),
             qPrintable(error));
    QCOMPARE(summary.bytesRead, qint64(ifPacket.size() + 64));
    QCOMPARE(summary.packetCount, 1);
    QCOMPARE(summary.ifPacketCount, 1);
    QCOMPARE(summary.firstPacketHeader, 901);
    QCOMPARE(summary.sampleCount, 2);

    const QString pscPath = dir.filePath(QStringLiteral("psc.iqraw"));
    QFile psc(pscPath);
    QVERIFY(psc.open(QIODevice::WriteOnly));
    psc.write(ceyearPscPacket({qint16(-700), qint16(-350)}, 900000000ULL, 901000000ULL));
    psc.close();

    summary = CeyearIfIqRawSummary{};
    error.clear();
    QVERIFY(!CeyearIfIqPacketParser::validateRawFilePrefix(pscPath, &summary, &error));
    QCOMPARE(summary.firstPacketHeader, 1201);
    QVERIFY(error.contains(QStringLiteral("PSCAN")));
}

void ServiceTests::ceyearIfIqParserConvertsRawPacketsToReadableCapFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString rawPath = dir.filePath("3943b.iqraw");
    QFile rawFile(rawPath);
    QVERIFY(rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    rawFile.write(ceyearIfPacket({{qint16(11), qint16(-11)}, {qint16(22), qint16(-22)}}));
    rawFile.close();

    UsrpCaptureConfig config;
    config.centerFrequencyHz = 1000000000.0;
    config.bandwidthHz = 10000000.0;
    config.outputFilePath = dir.filePath("3943b.cap");

    QString error;
    QVERIFY2(CeyearIfIqPacketParser::convertRawFileToCap(rawPath, config, &error), qPrintable(error));

    CapProbeService probe;
    const auto result = probe.probe(config.outputFilePath);
    QVERIFY2(result.has_value(), qPrintable(probe.lastError()));
    QCOMPARE(result->frameSampleCount, quint32(2));
    QCOMPARE(result->centerFrequencyHz, 1000000000.0);
    QCOMPARE(result->bandwidthHz, 10000000.0);
    QCOMPARE(result->previewPairs.size(), 2);
    QCOMPARE(result->previewPairs.at(0).first, qint16(11));
    QCOMPARE(result->previewPairs.at(0).second, qint16(-11));
}

void ServiceTests::ceyearDefaultResourceUsesLanSocket()
{
    QCOMPARE(defaultCeyearVisaResource(), QStringLiteral("TCPIP::192.168.50.54::5025::SOCKET"));
}

void ServiceTests::ceyearSmokeToolDryRunListsCommandsAndRequiresExecute()
{
    CeyearCaptureConfig config;
    config.pcReceiveProtocol = QStringLiteral("UDP");
    config.reportPath = QStringLiteral("UDP");
    config.pcReceiveIpAddress = QStringLiteral("192.168.50.20");
    config.pcReceivePort = 5030;

    const QStringList captureLines = ceyearCaptureSmokeCommandLines(config);
    const QStringList stopLines = ceyearCaptureSmokeStopCommandLines();

    QVERIFY(captureLines.contains(QStringLiteral("TRACe:UDP:TAG \"192.168.50.20\",5030,IF")));
    QVERIFY(captureLines.contains(QStringLiteral("TRACe:UDP:FLAG \"192.168.50.20\",5030,\"opt\"")));
    QVERIFY(captureLines.contains(QStringLiteral("TRACe:UDP:DEL ALL")));
    QVERIFY(captureLines.contains(QStringLiteral("TRACe:TCP:DEL ALL")));
    QVERIFY(captureLines.contains(QStringLiteral("TRACe:PATH UDP")));
    QVERIFY(!captureLines.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!captureLines.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
    QVERIFY(!captureLines.contains(QStringLiteral("INITiate")));
    QVERIFY(!captureLines.contains(QStringLiteral("TRACe:RECord:STORage FILE")));
    QVERIFY(!captureLines.contains(QStringLiteral("TRACe:RECord:SELectivity REPort")));
    QVERIFY(!captureLines.contains(QStringLiteral("TRACe:RECord:STATe OFF")));
    QVERIFY(!captureLines.contains(QStringLiteral("TRACe:RECord:SOURce IQ")));
    QVERIFY(!captureLines.contains(QStringLiteral("TRACe:RECord:STARt")));

    config.sendInitiate = true;
    const QStringList initiatedCaptureLines = ceyearCaptureSmokeCommandLines(config);
    QVERIFY(!initiatedCaptureLines.contains(QStringLiteral("TRACe:IQ:DATA:REPort FREE")));
    QVERIFY(!initiatedCaptureLines.contains(QStringLiteral("SYSTem:IF:REMote:MODE SHORT")));
    QVERIFY(!initiatedCaptureLines.contains(QStringLiteral("INITiate")));
    QVERIFY(!ceyearCaptureSmokeRequiresRiskAcknowledgement(config, false, false));
    QVERIFY(ceyearCaptureSmokeRequiresRiskAcknowledgement(config, true, false));
    QVERIFY(!ceyearCaptureSmokeRequiresRiskAcknowledgement(config, true, true));
    QVERIFY(!ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config, false, false));
    QVERIFY(ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config, true, false));
    QVERIFY(!ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config, true, true));
    QVERIFY(!ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config, true, false, true));
    QVERIFY(ceyearCaptureSmokeRequiresFreeRunAcknowledgement(config, true, false, false));
    QVERIFY(!ceyearCaptureSmokeRequiresReceivedBytes(false, false));
    QVERIFY(ceyearCaptureSmokeRequiresReceivedBytes(true, false));
    QVERIFY(!ceyearCaptureSmokeRequiresReceivedBytes(true, true));

    QVERIFY(stopLines.contains(QStringLiteral("TRACe:UDP:DEL ALL")));
    QVERIFY(stopLines.contains(QStringLiteral("TRACe:TCP:DEL ALL")));
    QVERIFY(!stopLines.contains(QStringLiteral("SYSTem:IF:REMote:MODE OFF")));
    QVERIFY(!stopLines.contains(QStringLiteral("TRACe:RECord:STOP")));

    const QFileInfo testSource(QString::fromLocal8Bit(__FILE__));
    QFile toolSource(testSource.absoluteDir().filePath(QStringLiteral("../src/tools/ceyear_capture_smoke.cpp")));
    QVERIFY(toolSource.open(QIODevice::ReadOnly));
    const QString toolText = QString::fromUtf8(toolSource.readAll());
    QVERIFY(!toolText.contains(QStringLiteral("--cap-output <path>")));
    QVERIFY(!toolText.contains(QStringLiteral("CAP_SUCCESS: ")));
    QVERIFY(!toolText.contains(QStringLiteral("--seconds <seconds>")));
    QVERIFY(toolText.contains(QStringLiteral("--points <count>")));
    QVERIFY(toolText.contains(QStringLiteral("--freq-hz <hz>")));
    QVERIFY(toolText.contains(QStringLiteral("--span-hz <hz>")));
    QVERIFY(toolText.contains(QStringLiteral("IQ_POINTS: ")));
}

void ServiceTests::ceyearPscSweepCommandsUseOnlyPscReportPath()
{
    CeyearPscSweepConfig config;
    config.pcReceiveIpAddress = QStringLiteral("192.168.1.168");
    config.pcReceivePort = 19712;
    config.segments = {
        {900000000.0, 1100000000.0, 25000.0},
        {1300000000.0, 1400000000.0, 12500.0},
    };

    const QStringList lines = ceyearPscSweepCommandLines(config);

    QVERIFY(lines.contains(QStringLiteral("FREQ:MODE PSC")));
    QVERIFY(!lines.contains(QStringLiteral("SWE:MODE:SPE SENS")));
    QVERIFY(!lines.contains(QStringLiteral("SWE:MODE:SPE SENE")));
    QVERIFY(lines.contains(QStringLiteral("TRAC:UDP:TAG:ON \"192.168.1.168\",19712,PSC")));
    QVERIFY(lines.contains(QStringLiteral("TRAC:UDP:FLAG:ON \"192.168.1.168\",19712,\"VOLT:AC\",\"FREQ:RX\",\"FREQ:HIGH:RX\",\"OPT\"")));
    QVERIFY(lines.contains(QStringLiteral("INIT")));
    for (const QString &line : lines) {
        QVERIFY(!line.contains(QStringLiteral("IFPAN"), Qt::CaseInsensitive));
        QVERIFY(!line.contains(QStringLiteral("TRAC:IQ"), Qt::CaseInsensitive));
        QVERIFY(!line.contains(QStringLiteral("SYST:IF:REM"), Qt::CaseInsensitive));
    }
}

void ServiceTests::ceyearPscSweepDefaultTargetUsesLocal3943bNetwork()
{
    CeyearPscSweepConfig config;

    QCOMPARE(config.pcReceiveIpAddress, QStringLiteral("192.168.50.20"));
    QCOMPARE(config.pcReceivePort, 19712);

    const QStringList lines = ceyearPscSweepCommandLines(config);
    QVERIFY(lines.contains(QStringLiteral("TRAC:UDP:TAG:ON \"192.168.50.20\",19712,PSC")));
    QVERIFY(lines.contains(QStringLiteral("TRAC:UDP:FLAG:ON \"192.168.50.20\",19712,\"VOLT:AC\",\"FREQ:RX\",\"FREQ:HIGH:RX\",\"OPT\"")));
}

void ServiceTests::ceyearPscSweepUsesSingleBandFrequencyCommands()
{
    CeyearPscSweepConfig config;
    config.pcReceiveIpAddress = QStringLiteral("192.168.1.168");
    config.pcReceivePort = 19712;
    config.segments = {
        {900000000.0, 1100000000.0, 25000.0},
        {1300000000.0, 1400000000.0, 12500.0},
    };

    const QStringList lines = ceyearPscSweepCommandLines(config);

    QVERIFY(lines.contains(QStringLiteral("FREQ:PSC:STAR 900000000 Hz")));
    QVERIFY(lines.contains(QStringLiteral("FREQ:PSC:STOP 1100000000 Hz")));
    QVERIFY(lines.contains(QStringLiteral("PSC:STEP 25000 Hz")));
    QVERIFY(!lines.join(QStringLiteral("\n")).contains(QStringLiteral("PSC:SEGM:CONF")));
    QVERIFY(!lines.join(QStringLiteral("\n")).contains(QStringLiteral("1300000000")));
}

void ServiceTests::ceyearPscSweepStopCommandsAbortAndClearUdp()
{
    QCOMPARE(ceyearPscSweepStopCommandLines(),
             QStringList({QStringLiteral("ABORt"),
                          QStringLiteral("*CLS"),
                          QStringLiteral("TRAC:UDP:DEL ALL"),
                          QStringLiteral("TRAC:TCP:DEL ALL"),
                          QStringLiteral("FREQ:MODE FFM")}));
}

void ServiceTests::ceyearPscParserExtractsFrequencyAndLevelBins()
{
    const QByteArray packet = ceyearPscPacket({qint16(-700), qint16(-350), qint16(-620)},
                                             900000000ULL,
                                             902000000ULL);

    const auto frames = CeyearPscPacketParser::parse(packet);

    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().bins.size(), 3);
    QCOMPARE(frames.first().bins.at(0).frequencyHz, 900000000.0);
    QCOMPARE(frames.first().bins.at(1).frequencyHz, 901000000.0);
    QCOMPARE(frames.first().bins.at(1).levelDbuv, -35.0);
    QCOMPARE(frames.first().bins.at(2).levelDbuv, -62.0);
}

void ServiceTests::ceyearPscReceiverEmitsFramesFromUdpDatagrams()
{
    CeyearPscReceiver receiver;
    CeyearPscReceiverConfig config;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;

    QVector<CeyearPscFrame> receivedFrames;
    connect(&receiver, &CeyearPscReceiver::framesReceived, this, [&](const QVector<CeyearPscFrame> &frames) {
        receivedFrames += frames;
    });

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    const QByteArray packet = ceyearPscPacket({qint16(-700), qint16(-350), qint16(-620)},
                                             900000000ULL,
                                             902000000ULL);
    QUdpSocket sender;
    QCOMPARE(sender.writeDatagram(packet, QHostAddress::LocalHost, receiver.boundPort()), qint64(packet.size()));

    QVERIFY(QTest::qWaitFor([&]() {
        return !receivedFrames.isEmpty();
    }, 1000));

    QCOMPARE(receiver.datagramCount(), qint64(1));
    QCOMPARE(receiver.bytesReceived(), qint64(packet.size()));
    QCOMPARE(receivedFrames.first().bins.size(), 3);
    QCOMPARE(receivedFrames.first().bins.at(1).frequencyHz, 901000000.0);
    QCOMPARE(receivedFrames.first().bins.at(1).levelDbuv, -35.0);
}

void ServiceTests::ceyearPscCandidateExtractorPromotesPersistentPeaks()
{
    CeyearPscCandidateExtractor extractor;

    QVERIFY(extractor.update({{{900000000.0, -82.0}, {901000000.0, -45.0}, {902000000.0, -30.0}}}).isEmpty());
    const auto secondCandidates = extractor.update({{{900000000.0, -83.0}, {901000000.0, -42.0}, {902000000.0, -78.0}}});
    QCOMPARE(secondCandidates.size(), 1);
    QCOMPARE(secondCandidates.first().frequencyHz, 901000000.0);
    QCOMPARE(secondCandidates.first().hitCount, 2);

    const auto thirdCandidates = extractor.update({{{900000000.0, -80.0}, {901000000.0, -38.0}, {902000000.0, -81.0}}});
    QCOMPARE(thirdCandidates.size(), 1);
    QCOMPARE(thirdCandidates.first().frequencyHz, 901000000.0);
    QCOMPARE(thirdCandidates.first().peakLevelDbuv, -38.0);
    QCOMPARE(thirdCandidates.first().hitCount, 3);
}

void ServiceTests::ceyearPscCandidateExtractorUsesConfiguredHitThreshold()
{
    CeyearPscCandidateExtractor extractor(-35.0);

    QVERIFY(extractor.update({{{901000000.0, -40.0}, {902000000.0, -30.0}}}).isEmpty());
    const auto secondCandidates = extractor.update({{{901000000.0, -39.0}, {902000000.0, -31.0}}});

    QCOMPARE(secondCandidates.size(), 1);
    QCOMPARE(secondCandidates.first().frequencyHz, 902000000.0);
    QCOMPARE(secondCandidates.first().peakLevelDbuv, -30.0);
    QCOMPARE(secondCandidates.first().hitCount, 2);
}

void ServiceTests::ceyearPscSweepCsvWriterPersistsFrameBins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVector<CeyearPscFrame> frames;
    frames.append({{{900000000.0, -72.0}, {901000000.0, -35.0}}});
    frames.append({{{902000000.0, -42.5}}});

    QString error;
    const QString filePath = dir.filePath(QStringLiteral("sweep.csv"));
    QVERIFY2(saveCeyearPscFramesCsv(filePath, frames, &error), qPrintable(error));

    QFile file(filePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(file.readAll());
    QVERIFY(text.startsWith(QStringLiteral("frame_index,bin_index,frequency_hz,level_dbuv")));
    QVERIFY(text.contains(QStringLiteral("0,1,901000000,-35.0")));
    QVERIFY(text.contains(QStringLiteral("1,0,902000000,-42.5")));

    error.clear();
    QVERIFY(!saveCeyearPscFramesCsv(dir.filePath(QStringLiteral("empty.csv")), {}, &error));
    QVERIFY(error.contains(QStringLiteral("No PSC sweep frames")));
}

void ServiceTests::ceyearPscSweepCsvReaderRestoresSortedFrames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString filePath = dir.filePath(QStringLiteral("sweep.csv"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("level_dbuv,source,frame_index,bin_index,frequency_hz\n"
               "-35.0,demo,1,1,901000000\n"
               "-72.0,demo,0,1,900500000\n"
               "-71.0,demo,0,0,900000000\n"
               "-34.0,demo,1,0,900000000\n");
    file.close();

    QVector<CeyearPscFrame> frames;
    QString error;
    QVERIFY2(loadCeyearPscFramesCsv(filePath, &frames, &error), qPrintable(error));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).bins.size(), 2);
    QCOMPARE(frames.at(0).bins.at(0).frequencyHz, 900000000.0);
    QCOMPARE(frames.at(0).bins.at(1).frequencyHz, 900500000.0);
    QCOMPARE(frames.at(1).bins.at(0).levelDbuv, -34.0);
}

void ServiceTests::ceyearPscSweepCsvReaderSkipsZeroFrequencySentinelBins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString filePath = dir.filePath(QStringLiteral("sentinel.csv"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("frame_index,bin_index,frequency_hz,level_dbuv\n"
               "0,0,5725000000,-1.6\n"
               "0,1,0,200.0\n"
               "0,2,5725010000,2.4\n"
               "1,0,5725000000,-1.7\n");
    file.close();

    QVector<CeyearPscFrame> frames;
    QString error;
    QVERIFY2(loadCeyearPscFramesCsv(filePath, &frames, &error), qPrintable(error));
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).bins.size(), 2);
    QCOMPARE(frames.at(0).bins.at(0).frequencyHz, 5725000000.0);
    QCOMPARE(frames.at(0).bins.at(1).frequencyHz, 5725010000.0);
    QCOMPARE(frames.at(1).bins.size(), 1);
}

void ServiceTests::ceyearPscSweepCsvReaderRejectsMissingColumnsAndInvalidRows()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString missingColumnPath = dir.filePath(QStringLiteral("missing.csv"));
    QFile missingColumnFile(missingColumnPath);
    QVERIFY(missingColumnFile.open(QIODevice::WriteOnly | QIODevice::Text));
    missingColumnFile.write("frame_index,bin_index,frequency_hz\n0,0,900000000\n");
    missingColumnFile.close();

    QVector<CeyearPscFrame> frames;
    QString error;
    QVERIFY(!loadCeyearPscFramesCsv(missingColumnPath, &frames, &error));
    QVERIFY(frames.isEmpty());
    QVERIFY(!error.isEmpty());

    const QString invalidRowPath = dir.filePath(QStringLiteral("invalid.csv"));
    QFile invalidRowFile(invalidRowPath);
    QVERIFY(invalidRowFile.open(QIODevice::WriteOnly | QIODevice::Text));
    invalidRowFile.write("frame_index,bin_index,frequency_hz,level_dbuv\n"
                         "0,0,not-a-number,-35.0\n");
    invalidRowFile.close();

    error.clear();
    QVERIFY(!loadCeyearPscFramesCsv(invalidRowPath, &frames, &error));
    QVERIFY(frames.isEmpty());
    QVERIFY(!error.isEmpty());
}

void ServiceTests::ceyearPscSweepQueriesAreWritableAndServiceExposesInterface()
{
    CeyearPscSweepConfig config;
    config.pcReceiveIpAddress = QStringLiteral("192.168.1.168");
    config.pcReceivePort = 19712;
    config.segments = {
        {900000000.0, 1100000000.0, 25000.0},
    };

    const auto startQueries = pscSweepStartQueries(config);
    QVERIFY(!startQueries.empty());
    QVERIFY((std::all_of(startQueries.cbegin(), startQueries.cend(), [](const CeyearVisaQuery &query) {
        return !query.readOnly;
    })));
    QVERIFY((std::any_of(startQueries.cbegin(), startQueries.cend(), [](const CeyearVisaQuery &query) {
        return query.command == QByteArray("FREQ:MODE PSC");
    })));

    const auto stopQueries = pscSweepStopQueries();
    QVERIFY(!stopQueries.empty());
    QVERIFY((std::all_of(stopQueries.cbegin(), stopQueries.cend(), [](const CeyearVisaQuery &query) {
        return !query.readOnly;
    })));

    CeyearVisaService service;
    ICeyearVisaService *iface = &service;
    const auto startMethod = &ICeyearVisaService::startPscSweep;
    const auto stopMethod = &ICeyearVisaService::stopPscSweep;
    Q_UNUSED(iface)
    Q_UNUSED(startMethod)
    Q_UNUSED(stopMethod)
}

void ServiceTests::ceyearIqReceiverWritesTcpPayloadToPcFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Tcp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("tcp.iqraw");
    config.timeoutMs = 500;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    const QByteArray payload("CEYEAR_TCP_IQ_PAYLOAD");
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, receiver.boundPort());
    QVERIFY(client.waitForConnected(500));
    QCOMPARE(client.write(payload), qint64(payload.size()));
    QVERIFY(client.waitForBytesWritten(500));
    client.disconnectFromHost();

    CeyearIqReceiverResult result = receiver.waitForFinished(1500);
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.bytesReceived, qint64(payload.size()));

    QFile file(config.outputFilePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payload);
}

void ServiceTests::ceyearIqReceiverKeepsTcpStreamOpenForCaptureWindow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Tcp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("tcp_stream.iqraw");
    config.timeoutMs = 300;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    const QByteArray firstChunk("CEYEAR_TCP_IQ_PART_1");
    const QByteArray secondChunk("CEYEAR_TCP_IQ_PART_2");
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, receiver.boundPort());
    QVERIFY(client.waitForConnected(500));
    QCOMPARE(client.write(firstChunk), qint64(firstChunk.size()));
    QVERIFY(client.waitForBytesWritten(500));
    QTimer::singleShot(80, [&client, secondChunk]() {
        QCOMPARE(client.write(secondChunk), qint64(secondChunk.size()));
        QVERIFY(client.waitForBytesWritten(500));
        client.disconnectFromHost();
    });

    CeyearIqReceiverResult result = receiver.waitForFinished(1000);
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.bytesReceived, qint64(firstChunk.size() + secondChunk.size()));

    QFile file(config.outputFilePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), firstChunk + secondChunk);
}

void ServiceTests::ceyearIqReceiverWritesUdpPayloadToPcFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Udp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("udp.iqraw");
    config.timeoutMs = 500;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    const QByteArray payload("CEYEAR_UDP_IQ_PAYLOAD");
    QUdpSocket sender;
    QCOMPARE(sender.writeDatagram(payload, QHostAddress::LocalHost, receiver.boundPort()), qint64(payload.size()));

    CeyearIqReceiverResult result = receiver.waitForFinished(1500);
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.bytesReceived, qint64(payload.size()));

    QFile file(config.outputFilePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payload);
}

void ServiceTests::ceyearIqReceiverStopsAtTargetSampleCount()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Udp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("target.iqraw");
    config.targetSampleCount = 2;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    const QByteArray packet = ceyearIfPacket({{qint16(1), qint16(-1)}, {qint16(2), qint16(-2)}});
    QUdpSocket sender;
    QCOMPARE(sender.writeDatagram(packet, QHostAddress::LocalHost, receiver.boundPort()), qint64(packet.size()));

    CeyearIqReceiverResult result = receiver.waitForFinished(1500);
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.bytesReceived, qint64(packet.size()));
    QCOMPARE(result.sampleCount, qint64(2));

    QFile file(config.outputFilePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), packet);
}

void ServiceTests::ceyearIqReceiverWaitsForDelayedTargetPackets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Udp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("delayed_target.iqraw");
    config.targetSampleCount = 3;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    QUdpSocket sender;
    const QByteArray firstPacket = ceyearIfPacket({{qint16(1), qint16(-1)}, {qint16(2), qint16(-2)}});
    QCOMPARE(sender.writeDatagram(firstPacket, QHostAddress::LocalHost, receiver.boundPort()),
             qint64(firstPacket.size()));

    QTimer::singleShot(500, &sender, [&sender, &receiver]() {
        const QByteArray delayedPacket = ceyearIfPacket({{qint16(3), qint16(-3)}});
        sender.writeDatagram(delayedPacket, QHostAddress::LocalHost, receiver.boundPort());
    });

    const CeyearIqReceiverResult result = receiver.waitForFinished(-1);
    QVERIFY2(result.success, qPrintable(result.message));
    QCOMPARE(result.sampleCount, qint64(3));
    QCOMPARE(result.bytesReceived, qint64(firstPacket.size() + ceyearIfPacket({{qint16(3), qint16(-3)}}).size()));
}

void ServiceTests::ceyearIqReceiverPreservesExistingFileWhenStartFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString outputPath = dir.filePath(QStringLiteral("existing.iqraw"));
    QFile existing(outputPath);
    QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
    existing.write("existing-data");
    existing.close();

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Udp;
    config.bindAddress = QStringLiteral("not-an-ip-address");
    config.port = 5030;
    config.outputFilePath = outputPath;

    QString error;
    QVERIFY(!receiver.start(config, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("existing-data"));
}

void ServiceTests::ceyearIqReceiverStopsWhenTargetStreamGoesIdle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Udp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("partial_target.iqraw");
    config.targetSampleCount = 100000000;
    config.dataIdleFinishMs = 100;
    config.firstByteWaitMs = 1000;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    const QByteArray packet = ceyearIfPacket({{qint16(1), qint16(-1)}, {qint16(2), qint16(-2)}});
    QUdpSocket sender;
    QCOMPARE(sender.writeDatagram(packet, QHostAddress::LocalHost, receiver.boundPort()), qint64(packet.size()));

    QElapsedTimer elapsed;
    elapsed.start();
    CeyearIqReceiverResult result = receiver.waitForFinished(-1);
    QVERIFY2(elapsed.elapsed() < 1000, qPrintable(QStringLiteral("receiver did not stop on idle stream, elapsed=%1").arg(elapsed.elapsed())));
    QVERIFY(!result.success);
    QVERIFY2(result.message.contains(QStringLiteral("idle")), qPrintable(result.message));
    QCOMPARE(result.bytesReceived, qint64(packet.size()));
    QCOMPARE(result.sampleCount, qint64(2));
}

void ServiceTests::ceyearIqReceiverCancelStopsTargetWait()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    CeyearIqReceiver receiver;
    CeyearIqReceiverConfig config;
    config.protocol = CeyearIqReceiveProtocol::Udp;
    config.bindAddress = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.outputFilePath = dir.filePath("cancel.iqraw");
    config.targetSampleCount = 2;

    QString error;
    QVERIFY2(receiver.start(config, &error), qPrintable(error));
    QVERIFY(receiver.boundPort() > 0);

    QTimer::singleShot(50, &receiver, &CeyearIqReceiver::cancel);
    CeyearIqReceiverResult result = receiver.waitForFinished(1000);
    QVERIFY(!result.success);
    QCOMPARE(result.bytesReceived, qint64(0));
    QCOMPARE(result.sampleCount, qint64(0));
}

QTEST_MAIN(ServiceTests)
#include "service_tests.moc"
