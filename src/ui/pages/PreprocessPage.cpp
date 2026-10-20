#include "ui/pages/PreprocessPage.h"

#include "app/ProjectPaths.h"
#include "services/AppStateStore.h"
#include "services/CapProbeService.h"
#include "services/PreprocessProcessRunner.h"
#include "ui/widgets/MetricCard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QPixmap>

#include <algorithm>

namespace
{
constexpr int kEditorHeight = 38;
constexpr int kPathWidth = 520;

QFrame *sectionFrame(const QString &name, const QString &title, QWidget *parent, QVBoxLayout **layoutOut)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(name);
    frame->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *titleLabel = new QLabel(title, frame);
    titleLabel->setObjectName(name + QStringLiteral("Title"));
    titleLabel->setProperty("sectionTitle", true);
    layout->addWidget(titleLabel);
    if (layoutOut) {
        *layoutOut = layout;
    }
    return frame;
}

QLineEdit *edit(const QString &name, const QString &value, QWidget *parent, int width = 180)
{
    auto *widget = new QLineEdit(value, parent);
    widget->setObjectName(name);
    widget->setMinimumHeight(kEditorHeight);
    widget->setMinimumWidth(width);
    return widget;
}

QWidget *field(const QString &caption, QWidget *editor, QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *label = new QLabel(caption, container);
    label->setProperty("muted", true);
    layout->addWidget(label);
    layout->addWidget(editor);
    return container;
}

QWidget *fieldWithHint(const QString &caption, const QString &hint, QWidget *editor, QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *label = new QLabel(caption, container);
    label->setProperty("muted", true);
    layout->addWidget(label);
    layout->addWidget(editor);
    auto *hintLabel = new QLabel(hint, container);
    hintLabel->setWordWrap(true);
    hintLabel->setProperty("muted", true);
    hintLabel->setProperty("fieldHint", true);
    layout->addWidget(hintLabel);
    return container;
}

QTableWidgetItem *readonlyItem(const QString &text, const QString &path = QString())
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (!path.isEmpty()) {
        item->setData(Qt::UserRole, path);
    }
    return item;
}

QString mhz(double hz)
{
    return hz > 0.0 ? QStringLiteral("%1 MHz").arg(hz / 1000000.0, 0, 'f', 3) : QStringLiteral("-");
}

QString fileSizeText(qint64 bytes)
{
    if (bytes >= 1024ll * 1024ll * 1024ll) {
        return QStringLiteral("%1 GB").arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
    }
    if (bytes >= 1024ll * 1024ll) {
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 2);
    }
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
}

bool readInt(const QLineEdit *widget, const QString &label, int min, int max, int *value, QString *error)
{
    bool ok = false;
    const int parsed = widget ? widget->text().trimmed().toInt(&ok) : 0;
    if (!ok || parsed < min || parsed > max) {
        if (error) {
            *error = QStringLiteral("%1必须是 %2 到 %3 之间的整数。").arg(label).arg(min).arg(max);
        }
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

bool readDouble(const QLineEdit *widget, const QString &label, double min, double max, double *value, QString *error)
{
    bool ok = false;
    const double parsed = widget ? widget->text().trimmed().toDouble(&ok) : 0.0;
    if (!ok || parsed < min || parsed > max) {
        if (error) {
            *error = QStringLiteral("%1必须是 %2 到 %3 之间的数值。").arg(label).arg(min).arg(max);
        }
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

QString latestIqrawPath()
{
    const QDir rawDir(QDir(ProjectPaths::dataDirectory()).filePath(QStringLiteral("raw")));
    const auto files = rawDir.entryInfoList({QStringLiteral("*.iqraw"), QStringLiteral("*.IQRAW")},
                                            QDir::Files | QDir::Readable,
                                            QDir::Time);
    for (const QFileInfo &file : files) {
        if (file.size() > 0) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

QString stateForResult(const PreprocessRunResult &result)
{
    if (result.cancelled) {
        return QStringLiteral("部分完成");
    }
    if (result.timedOut) {
        return QStringLiteral("部分完成");
    }
    if (result.partial) {
        return QStringLiteral("部分完成");
    }
    return result.success ? QStringLiteral("已完成") : QStringLiteral("失败");
}
} // namespace

PreprocessPage::PreprocessPage(QWidget *parent)
    : WorkflowPage(QStringLiteral("信号预处理"),
                   QStringLiteral("确认采集文件，选择处理方案，生成供数据集页人工复核的候选片段和样本。"),
                   parent,
                   true)
{
    buildUi();
}

PreprocessPage::~PreprocessPage()
{
    if (m_processRunner && m_processRunner->isRunning()) {
        m_processRunner->cancel();
    }
}

void PreprocessPage::setInputFilePath(const QString &filePath)
{
    const QString path = filePath.trimmed();
    if (path.isEmpty() || !QFileInfo::exists(path) || !m_capPathEdit) {
        return;
    }
    const bool inputChanged = QFileInfo(m_capPathEdit->text()).absoluteFilePath()
        != QFileInfo(path).absoluteFilePath();
    m_capPathEdit->setText(path);
    m_inputConfirmed = false;
    if (inputChanged) {
        m_latestResult = PreprocessRunResult();
        if (m_segmentTable) {
            m_segmentTable->setRowCount(0);
        }
        if (m_sampleTable) {
            m_sampleTable->setRowCount(0);
        }
        if (m_nextDatasetButton) {
            m_nextDatasetButton->setEnabled(false);
        }
        if (m_segmentMetric) {
            m_segmentMetric->setValue(QStringLiteral("-"));
        }
        if (m_sampleMetric) {
            m_sampleMetric->setValue(QStringLiteral("-"));
        }
    }
    setValidationMessage(QStringLiteral("采集文件已带入，请点击“读取并确认”后再开始处理。"), QStringLiteral("neutral"));
    m_inputFileSummaryLabel->setText(QStringLiteral("尚未确认输入文件：%1").arg(QFileInfo(path).fileName()));
    if (m_capMetric) {
        m_capMetric->setValue(QStringLiteral("待确认"));
    }
    emit workflowStateChanged();
}

bool PreprocessPage::isInputConfirmed() const
{
    return m_inputConfirmed;
}

bool PreprocessPage::isRunning() const
{
    return m_processRunner && m_processRunner->isRunning();
}

bool PreprocessPage::hasResults() const
{
    return !m_latestResult.segments.isEmpty() || !m_latestResult.sampleRecords.isEmpty();
}

QString PreprocessPage::latestOutputDirectory() const
{
    return m_latestResult.batchDirectory;
}

void PreprocessPage::buildUi()
{
    auto *inputLayout = static_cast<QVBoxLayout *>(nullptr);
    auto *inputSection = sectionFrame(QStringLiteral("PreprocessInputColumn"), QStringLiteral("1. 确认输入文件"), this, &inputLayout);

    const QString recent = latestIqrawPath();
    m_capPathEdit = edit(QStringLiteral("PreprocessCapPathEdit"), recent, inputSection, kPathWidth);
    m_capPathEdit->setPlaceholderText(QStringLiteral("选择 .iqraw 或 .cap 文件"));
    auto *browseButton = new QPushButton(QStringLiteral("选择文件"), inputSection);
    browseButton->setObjectName(QStringLiteral("PreprocessBrowseCapButton"));
    m_previewButton = new QPushButton(QStringLiteral("读取并确认"), inputSection);
    m_previewButton->setObjectName(QStringLiteral("PreprocessPreviewButton"));
    m_previewButton->setProperty("secondary", true);
    auto *pathField = new QWidget(inputSection);
    auto *pathFieldLayout = new QVBoxLayout(pathField);
    pathFieldLayout->setContentsMargins(0, 0, 0, 0);
    pathFieldLayout->setSpacing(4);
    auto *pathCaption = new QLabel(QStringLiteral("采集文件"), pathField);
    pathCaption->setProperty("muted", true);
    pathFieldLayout->addWidget(pathCaption);
    auto *pathRow = new QHBoxLayout();
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(m_capPathEdit, 1);
    pathRow->addWidget(browseButton);
    pathRow->addWidget(m_previewButton);
    pathFieldLayout->addLayout(pathRow);
    inputLayout->addWidget(pathField);

    m_validationMessage = new QLabel(recent.isEmpty() ? QStringLiteral("请选择一个 IQRAW 或 CAP 文件。")
                                                       : QStringLiteral("已找到最近的 IQRAW 文件，请点击“读取并确认”。"),
                                     inputSection);
    m_validationMessage->setObjectName(QStringLiteral("PreprocessValidationMessage"));
    m_validationMessage->setWordWrap(true);
    inputLayout->addWidget(m_validationMessage);

    m_inputFileSummaryLabel = new QLabel(QStringLiteral("尚未确认输入文件。"), inputSection);
    m_inputFileSummaryLabel->setObjectName(QStringLiteral("PreprocessInputFileSummary"));
    m_inputFileSummaryLabel->setWordWrap(true);
    m_inputFileSummaryLabel->setProperty("muted", true);
    inputLayout->addWidget(m_inputFileSummaryLabel);

    auto *summaryPanel = new QFrame(inputSection);
    summaryPanel->setObjectName(QStringLiteral("PreprocessSummaryPanel"));
    auto *summaryGrid = new QGridLayout(summaryPanel);
    summaryGrid->setContentsMargins(12, 10, 12, 10);
    summaryGrid->setHorizontalSpacing(22);
    m_versionLabel = new QLabel("-", summaryPanel);
    m_headerLengthLabel = new QLabel("-", summaryPanel);
    m_bandwidthLabel = new QLabel("-", summaryPanel);
    m_sampleRateLabel = new QLabel("-", summaryPanel);
    m_centerFrequencyLabel = new QLabel("-", summaryPanel);
    m_sampleCountLabel = new QLabel("-", summaryPanel);
    const QList<QPair<QString, QLabel *>> metadata = {
        {QStringLiteral("格式"), m_versionLabel},
        {QStringLiteral("头/包长度"), m_headerLengthLabel},
        {QStringLiteral("带宽"), m_bandwidthLabel},
        {QStringLiteral("采样率"), m_sampleRateLabel},
        {QStringLiteral("中心频率"), m_centerFrequencyLabel},
        {QStringLiteral("样本数"), m_sampleCountLabel},
    };
    for (int index = 0; index < metadata.size(); ++index) {
        auto *block = new QWidget(summaryPanel);
        auto *blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->addWidget(new QLabel(metadata.at(index).first, block));
        blockLayout->addWidget(metadata.at(index).second);
        summaryGrid->addWidget(block, index / 3, index % 3);
    }
    inputLayout->addWidget(summaryPanel);

    auto *outputPanel = new QFrame(inputSection);
    outputPanel->setObjectName(QStringLiteral("PreprocessPathParametersPanel"));
    auto *outputLayout = new QHBoxLayout(outputPanel);
    outputLayout->setContentsMargins(12, 8, 12, 8);
    m_outputDirEdit = edit(QStringLiteral("PreprocessOutputDirEdit"),
                           ProjectPaths::preprocessRunsDirectory(),
                           outputPanel,
                           kPathWidth);
    auto *outputButton = new QPushButton(QStringLiteral("选择根目录"), outputPanel);
    outputButton->setObjectName(QStringLiteral("PreprocessBrowseOutputDirButton"));
    outputButton->setProperty("secondary", true);
    outputLayout->addWidget(field(QStringLiteral("批次输出根目录"), m_outputDirEdit, outputPanel), 1);
    outputLayout->addWidget(outputButton);
    inputLayout->addWidget(outputPanel);

    auto *terminologyPanel = new QFrame(inputSection);
    terminologyPanel->setObjectName(QStringLiteral("PreprocessTerminologyPanel"));
    auto *terminologyLayout = new QVBoxLayout(terminologyPanel);
    terminologyLayout->setContentsMargins(12, 10, 12, 10);
    auto *terminologyLabel = new QLabel(
        QStringLiteral("术语速查：I/Q 是同相/正交复数采样；IQRAW 是原始 I/Q 文件；CAP 是 USRP 原始采集文件；NPZ 是预处理后的样本包。"
                       "UHD 是 USRP 驱动库；VISA 是 3943B 仪器通信接口；STFT 是短时傅里叶变换；nperseg 是每个 STFT 分析窗的采样点数。"),
        terminologyPanel);
    terminologyLabel->setObjectName(QStringLiteral("PreprocessTerminologyLabel"));
    terminologyLabel->setWordWrap(true);
    terminologyLabel->setProperty("muted", true);
    terminologyLayout->addWidget(terminologyLabel);
    inputLayout->addWidget(terminologyPanel);

    auto *parameterLayout = static_cast<QVBoxLayout *>(nullptr);
    auto *parameterSection = sectionFrame(QStringLiteral("PreprocessParametersColumn"), QStringLiteral("2. 选择处理方案"), this, &parameterLayout);
    auto *schemeGrid = new QGridLayout();
    schemeGrid->setHorizontalSpacing(14);
    schemeGrid->setVerticalSpacing(10);
    m_detectionModeCombo = new QComboBox(parameterSection);
    m_detectionModeCombo->setObjectName(QStringLiteral("PreprocessDetectionModeCombo"));
    m_detectionModeCombo->setMinimumHeight(kEditorHeight);
    m_detectionModeCombo->addItem(QStringLiteral("推荐识别：适合常规 3943B 信号"), QStringLiteral("cv_band"));
    m_detectionModeCombo->addItem(QStringLiteral("快速能量筛选：优先速度"), QStringLiteral("energy"));
    m_detectionModeCombo->addItem(QStringLiteral("结构分割：适合脉冲形状"), QStringLiteral("shape"));
    m_detectionModeCombo->addItem(QStringLiteral("固定频带窗口：按窗口导出"), QStringLiteral("band_window"));
    m_payloadEndianCombo = new QComboBox(parameterSection);
    m_payloadEndianCombo->setObjectName(QStringLiteral("PreprocessPayloadEndianCombo"));
    m_payloadEndianCombo->setMinimumHeight(kEditorHeight);
    m_payloadEndianCombo->addItem(QStringLiteral("小端 IQ"), QStringLiteral("little"));
    m_payloadEndianCombo->addItem(QStringLiteral("大端 IQ"), QStringLiteral("big"));
    m_exportSpectrogramCheck = new QCheckBox(QStringLiteral("生成时频图，处理时间更长"), parameterSection);
    m_exportSpectrogramCheck->setObjectName(QStringLiteral("PreprocessExportSpectrogramCheck"));
    m_exportSpectrogramCheck->setChecked(false);
    schemeGrid->addWidget(fieldWithHint(QStringLiteral("处理方案"),
                                         QStringLiteral("推荐识别适合常规 3943B 信号；能量、结构和固定频带模式用于明确的专项场景。"),
                                         m_detectionModeCombo,
                                         parameterSection),
                          0,
                          0,
                          1,
                          2);
    schemeGrid->addWidget(fieldWithHint(QStringLiteral("IQRAW 载荷字节序"),
                                         QStringLiteral("示例：小端 IQ；只有设备文档明确要求时才选择大端。"),
                                         m_payloadEndianCombo,
                                         parameterSection),
                          1,
                          0);
    schemeGrid->addWidget(m_exportSpectrogramCheck, 1, 1, Qt::AlignLeft | Qt::AlignVCenter);
    parameterLayout->addLayout(schemeGrid);

    m_parameterModeLabel = new QLabel(QStringLiteral("当前模式：推荐模式（使用已验证的常规参数；适合首次处理）"), parameterSection);
    m_parameterModeLabel->setObjectName(QStringLiteral("PreprocessParameterModeLabel"));
    m_parameterModeLabel->setProperty("summary", true);
    m_parameterModeLabel->setWordWrap(true);
    parameterLayout->addWidget(m_parameterModeLabel);

    m_advancedParamsToggle = new QPushButton(QStringLiteral("切换到专家模式"), parameterSection);
    m_advancedParamsToggle->setObjectName(QStringLiteral("PreprocessAdvancedParametersToggle"));
    m_advancedParamsToggle->setCheckable(true);
    m_advancedParamsToggle->setProperty("secondary", true);
    parameterLayout->addWidget(m_advancedParamsToggle, 0, Qt::AlignLeft);

    m_windowSamplesEdit = edit(QStringLiteral("PreprocessWindowSamplesEdit"), QStringLiteral("4096"), parameterSection);
    m_chunkSamplesEdit = edit(QStringLiteral("PreprocessChunkSamplesEdit"), QStringLiteral("2000000"), parameterSection);
    m_frequencySpanEdit = edit(QStringLiteral("PreprocessFrequencySpanEdit"), QStringLiteral("12.0"), parameterSection);
    m_energyOffsetEdit = edit(QStringLiteral("PreprocessEnergyOffsetEdit"), QStringLiteral("8.0"), parameterSection);
    m_targetBandwidthEdit = edit(QStringLiteral("PreprocessTargetBandwidthEdit"), QStringLiteral("20.0"), parameterSection);
    m_minBandwidthEdit = edit(QStringLiteral("PreprocessMinBandwidthEdit"), QStringLiteral("16.0"), parameterSection);
    m_minWidebandFillEdit = edit(QStringLiteral("PreprocessMinWidebandFillEdit"), QStringLiteral("0.42"), parameterSection);
    m_minWidebandSnrEdit = edit(QStringLiteral("PreprocessMinWidebandSnrEdit"), QStringLiteral("3.0"), parameterSection);
    m_morphFrequencyCloseEdit = edit(QStringLiteral("PreprocessMorphFrequencyCloseEdit"), QStringLiteral("1.2"), parameterSection);
    m_shapeSplitQuantileEdit = edit(QStringLiteral("PreprocessShapeSplitQuantileEdit"), QStringLiteral("55.0"), parameterSection);
    m_shapeSplitMergeGapEdit = edit(QStringLiteral("PreprocessShapeSplitMergeGapEdit"), QStringLiteral("0.05"), parameterSection);
    m_maxSegmentDurationEdit = edit(QStringLiteral("PreprocessMaxSegmentDurationEdit"), QStringLiteral("2.0"), parameterSection);
    m_bandWindowEdit = edit(QStringLiteral("PreprocessBandWindowEdit"), QStringLiteral("2.0"), parameterSection);
    m_bandWindowOverlapEdit = edit(QStringLiteral("PreprocessBandWindowOverlapEdit"), QStringLiteral("0.0"), parameterSection);
    m_minBandScoreEdit = edit(QStringLiteral("PreprocessMinBandScoreEdit"), QStringLiteral("0.02"), parameterSection);
    m_cvColumnQuantileEdit = edit(QStringLiteral("PreprocessCvColumnQuantileEdit"), QStringLiteral("40.0"), parameterSection);
    m_cvOccupancyEdit = edit(QStringLiteral("PreprocessCvOccupancyEdit"), QStringLiteral("0.25"), parameterSection);
    m_cvMorphFrequencyEdit = edit(QStringLiteral("PreprocessCvMorphFrequencyEdit"), QStringLiteral("0.3"), parameterSection);
    m_cvMorphTimeEdit = edit(QStringLiteral("PreprocessCvMorphTimeEdit"), QStringLiteral("0.05"), parameterSection);
    m_minDurationEdit = edit(QStringLiteral("PreprocessMinDurationEdit"), QStringLiteral("0.05"), parameterSection);
    m_mergeGapEdit = edit(QStringLiteral("PreprocessMergeGapEdit"), QStringLiteral("0.20"), parameterSection);
    m_npersegEdit = edit(QStringLiteral("PreprocessNpersegEdit"), QStringLiteral("2048"), parameterSection);
    m_noverlapEdit = edit(QStringLiteral("PreprocessNoverlapEdit"), QStringLiteral("1536"), parameterSection);
    m_spectrogramNpersegEdit = edit(QStringLiteral("PreprocessSpectrogramNpersegEdit"), QStringLiteral("512"), parameterSection);
    m_spectrogramNoverlapEdit = edit(QStringLiteral("PreprocessSpectrogramNoverlapEdit"), QStringLiteral("384"), parameterSection);

    m_advancedParamsPanel = new QFrame(parameterSection);
    m_advancedParamsPanel->setObjectName(QStringLiteral("PreprocessAdvancedParametersPanel"));
    auto *advancedLayout = new QVBoxLayout(m_advancedParamsPanel);
    advancedLayout->setContentsMargins(12, 12, 12, 12);
    advancedLayout->setSpacing(8);
    m_advancedParametersHelp = new QLabel(
        QStringLiteral("专家参数说明：窗口采样点示例 4096（1024 到 262144），决定时间/频率分辨率；"
                       "STFT nperseg 示例 2048（16 到 262144），noverlap 必须小于 nperseg；"
                       "PNG nperseg 示例 512，仅在生成时频图时生效。数值越大通常频率分辨率更细，但处理时间和内存占用也会上升。"),
        m_advancedParamsPanel);
    m_advancedParametersHelp->setObjectName(QStringLiteral("PreprocessAdvancedParametersHelp"));
    m_advancedParametersHelp->setWordWrap(true);
    m_advancedParametersHelp->setProperty("muted", true);
    advancedLayout->addWidget(m_advancedParametersHelp);

    auto *advancedGrid = new QGridLayout();
    advancedGrid->setContentsMargins(0, 0, 0, 0);
    advancedGrid->setHorizontalSpacing(12);
    advancedGrid->setVerticalSpacing(8);
    advancedLayout->addLayout(advancedGrid);
    const QVector<QPair<QString, QLineEdit *>> advancedFields = {
        {QStringLiteral("窗口采样点"), m_windowSamplesEdit}, {QStringLiteral("分块采样点"), m_chunkSamplesEdit},
        {QStringLiteral("频率跨度 MHz"), m_frequencySpanEdit}, {QStringLiteral("能量偏移 dB"), m_energyOffsetEdit},
        {QStringLiteral("目标带宽 MHz"), m_targetBandwidthEdit}, {QStringLiteral("最小带宽 MHz"), m_minBandwidthEdit},
        {QStringLiteral("最小宽带填充"), m_minWidebandFillEdit}, {QStringLiteral("最小宽带 SNR dB"), m_minWidebandSnrEdit},
        {QStringLiteral("频向闭运算 MHz"), m_morphFrequencyCloseEdit}, {QStringLiteral("形状分割分位数"), m_shapeSplitQuantileEdit},
        {QStringLiteral("形状合并间隔 ms"), m_shapeSplitMergeGapEdit}, {QStringLiteral("最大片段时长 ms"), m_maxSegmentDurationEdit},
        {QStringLiteral("频带窗口 ms"), m_bandWindowEdit}, {QStringLiteral("窗口重叠 ms"), m_bandWindowOverlapEdit},
        {QStringLiteral("最小频带评分"), m_minBandScoreEdit}, {QStringLiteral("CV 列分位数"), m_cvColumnQuantileEdit},
        {QStringLiteral("CV 最小占空"), m_cvOccupancyEdit}, {QStringLiteral("CV 频向形态 MHz"), m_cvMorphFrequencyEdit},
        {QStringLiteral("CV 时向形态 ms"), m_cvMorphTimeEdit}, {QStringLiteral("最小时长 ms"), m_minDurationEdit},
        {QStringLiteral("片段合并间隔 ms"), m_mergeGapEdit}, {QStringLiteral("STFT nperseg"), m_npersegEdit},
        {QStringLiteral("STFT noverlap"), m_noverlapEdit}, {QStringLiteral("PNG nperseg"), m_spectrogramNpersegEdit},
        {QStringLiteral("PNG noverlap"), m_spectrogramNoverlapEdit},
    };
    for (int index = 0; index < advancedFields.size(); ++index) {
        advancedGrid->addWidget(field(advancedFields.at(index).first, advancedFields.at(index).second, m_advancedParamsPanel),
                                 index / 4,
                                 index % 4);
    }
    m_advancedParamsPanel->setVisible(false);
    parameterLayout->addWidget(m_advancedParamsPanel);

    auto *profileRow = new QHBoxLayout();
    m_resetProfileButton = new QPushButton(QStringLiteral("恢复默认"), parameterSection);
    m_resetProfileButton->setObjectName(QStringLiteral("PreprocessResetProfileButton"));
    m_saveProfileButton = new QPushButton(QStringLiteral("保存方案"), parameterSection);
    m_saveProfileButton->setObjectName(QStringLiteral("PreprocessSaveProfileButton"));
    m_loadProfileButton = new QPushButton(QStringLiteral("加载方案"), parameterSection);
    m_loadProfileButton->setObjectName(QStringLiteral("PreprocessLoadProfileButton"));
    m_resetProfileButton->setProperty("secondary", true);
    m_saveProfileButton->setProperty("secondary", true);
    m_loadProfileButton->setProperty("secondary", true);
    profileRow->addWidget(m_resetProfileButton);
    profileRow->addWidget(m_saveProfileButton);
    profileRow->addWidget(m_loadProfileButton);
    profileRow->addStretch(1);
    parameterLayout->addLayout(profileRow);

    auto *runLayout = static_cast<QVBoxLayout *>(nullptr);
    auto *runSection = sectionFrame(QStringLiteral("PreprocessRunColumn"), QStringLiteral("3. 运行任务"), this, &runLayout);
    auto *metrics = new QHBoxLayout();
    m_capMetric = new MetricCard(QStringLiteral("输入状态"), runSection);
    m_capMetric->setValue(QStringLiteral("未确认"));
    m_segmentMetric = new MetricCard(QStringLiteral("候选片段"), runSection);
    m_segmentMetric->setValue(QStringLiteral("0"));
    m_sampleMetric = new MetricCard(QStringLiteral("输出样本"), runSection);
    m_sampleMetric->setValue(QStringLiteral("0"));
    metrics->addWidget(m_capMetric);
    metrics->addWidget(m_segmentMetric);
    metrics->addWidget(m_sampleMetric);
    runLayout->addLayout(metrics);

    m_runStateLabel = new QLabel(QStringLiteral("待执行"), runSection);
    m_runStateLabel->setObjectName(QStringLiteral("PreprocessRunStateLabel"));
    runLayout->addWidget(m_runStateLabel);
    m_progressBar = new QProgressBar(runSection);
    m_progressBar->setObjectName(QStringLiteral("PreprocessProgressBar"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    runLayout->addWidget(m_progressBar);
    m_progressDetailLabel = new QLabel(QStringLiteral("等待开始"), runSection);
    m_progressDetailLabel->setObjectName(QStringLiteral("PreprocessProgressDetail"));
    m_progressDetailLabel->setWordWrap(true);
    m_progressDetailLabel->setProperty("muted", true);
    runLayout->addWidget(m_progressDetailLabel);

    m_outputSummaryLabel = new QLabel(QStringLiteral("任务结果将写入独立批次目录。"), runSection);
    m_outputSummaryLabel->setObjectName(QStringLiteral("PreprocessOutputSummaryLabel"));
    m_outputSummaryLabel->setWordWrap(true);
    runLayout->addWidget(m_outputSummaryLabel);
    m_computeBackendLabel = new QLabel(QStringLiteral("计算后端：CUDA GPU（任务启动后确认）"), runSection);
    m_computeBackendLabel->setObjectName(QStringLiteral("PreprocessComputeBackendLabel"));
    m_computeBackendLabel->setWordWrap(true);
    m_computeBackendLabel->setProperty("muted", true);
    runLayout->addWidget(m_computeBackendLabel);
    auto *runButtons = new QHBoxLayout();
    m_runButton = new QPushButton(QStringLiteral("开始预处理"), runSection);
    m_runButton->setObjectName(QStringLiteral("StartPreprocessButton"));
    m_runButton->setProperty("primary", true);
    m_runButton->setMinimumHeight(42);
    m_runButton->setEnabled(false);
    m_cancelButton = new QPushButton(QStringLiteral("取消并保留结果"), runSection);
    m_cancelButton->setObjectName(QStringLiteral("CancelPreprocessButton"));
    m_cancelButton->setProperty("secondary", true);
    m_cancelButton->setEnabled(false);
    m_nextDatasetButton = new QPushButton(QStringLiteral("去数据集人工复核"), runSection);
    m_nextDatasetButton->setObjectName(QStringLiteral("GoToDatasetButton"));
    m_nextDatasetButton->setProperty("secondary", true);
    m_nextDatasetButton->setEnabled(false);
    runButtons->addWidget(m_runButton);
    runButtons->addWidget(m_cancelButton);
    runButtons->addWidget(m_nextDatasetButton);
    runButtons->addStretch(1);
    runLayout->addLayout(runButtons);

    auto *detailsToggle = new QPushButton(QStringLiteral("显示运行详情"), runSection);
    detailsToggle->setObjectName(QStringLiteral("PreprocessLogToggle"));
    detailsToggle->setCheckable(true);
    detailsToggle->setProperty("secondary", true);
    runLayout->addWidget(detailsToggle, 0, Qt::AlignLeft);
    m_logOutput = new QPlainTextEdit(runSection);
    m_logOutput->setObjectName(QStringLiteral("PreprocessLog"));
    m_logOutput->setReadOnly(true);
    m_logOutput->setMinimumHeight(120);
    m_logOutput->setVisible(false);
    runLayout->addWidget(m_logOutput);

    auto *resultsPanel = new QGroupBox(QStringLiteral("4. 结果预览与质量检查"), this);
    resultsPanel->setObjectName(QStringLiteral("PreprocessResultsPanel"));
    auto *resultsLayout = new QVBoxLayout(resultsPanel);
    resultsLayout->setContentsMargins(16, 16, 16, 16);
    auto *overview = new QHBoxLayout();
    auto *avgBandwidth = new MetricCard(QStringLiteral("平均带宽"), resultsPanel);
    avgBandwidth->setObjectName(QStringLiteral("PreprocessAverageBandwidthMetric"));
    auto *avgSnr = new MetricCard(QStringLiteral("平均 SNR"), resultsPanel);
    avgSnr->setObjectName(QStringLiteral("PreprocessAverageSnrMetric"));
    m_qualitySummaryLabel = new QLabel(QStringLiteral("暂无结果。"), resultsPanel);
    m_qualitySummaryLabel->setObjectName(QStringLiteral("PreprocessQualitySummary"));
    m_qualitySummaryLabel->setWordWrap(true);
    overview->addWidget(avgBandwidth);
    overview->addWidget(avgSnr);
    overview->addWidget(m_qualitySummaryLabel, 2);
    resultsLayout->addLayout(overview);

    m_emptyState = new QLabel(QStringLiteral("完成预处理后，这里会显示候选片段和输出样本。"), resultsPanel);
    m_emptyState->setObjectName(QStringLiteral("EmptyState"));
    m_emptyState->setWordWrap(true);
    resultsLayout->addWidget(m_emptyState);

    m_resultsTabs = new QTabWidget(resultsPanel);
    m_resultsTabs->setObjectName(QStringLiteral("PreprocessResultsTabs"));
    m_segmentTable = new QTableWidget(0, 8, m_resultsTabs);
    m_segmentTable->setObjectName(QStringLiteral("PreprocessSegmentTable"));
    m_segmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_segmentTable->setAlternatingRowColors(true);
    m_segmentTable->setHorizontalHeaderLabels({QStringLiteral("候选片段"), QStringLiteral("起始点"), QStringLiteral("结束点"),
                                                QStringLiteral("时长 ms"), QStringLiteral("带宽"), QStringLiteral("SNR dB"),
                                                QStringLiteral("评分"), QStringLiteral("质量")});
    m_segmentTable->horizontalHeader()->setStretchLastSection(true);
    m_segmentTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_segmentTable, QStringLiteral("候选片段"));

    m_sampleTable = new QTableWidget(0, 7, m_resultsTabs);
    m_sampleTable->setObjectName(QStringLiteral("PreprocessSampleTable"));
    m_sampleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sampleTable->setAlternatingRowColors(true);
    m_sampleTable->setHorizontalHeaderLabels({QStringLiteral("样本 ID"), QStringLiteral("候选片段"), QStringLiteral("中心频率"),
                                                QStringLiteral("带宽"), QStringLiteral("评分"), QStringLiteral("候选局部图"),
                                                QStringLiteral("当前人工复核状态")});
    m_sampleTable->horizontalHeader()->setStretchLastSection(true);
    m_sampleTable->verticalHeader()->setVisible(false);
    m_resultsTabs->addTab(m_sampleTable, QStringLiteral("输出样本"));
    resultsLayout->addWidget(m_resultsTabs, 1);

    auto *detailPanel = new QFrame(resultsPanel);
    detailPanel->setObjectName(QStringLiteral("PreprocessResultDetailPanel"));
    auto *detailLayout = new QHBoxLayout(detailPanel);
    detailLayout->setContentsMargins(10, 10, 10, 10);
    auto *detailText = new QVBoxLayout();
    m_detailTitleLabel = new QLabel(QStringLiteral("选择一条结果查看详情"), detailPanel);
    m_detailTitleLabel->setObjectName(QStringLiteral("PreprocessDetailTitle"));
    m_detailMetricsLabel = new QLabel(QStringLiteral("-"), detailPanel);
    m_detailMetricsLabel->setObjectName(QStringLiteral("PreprocessDetailMetrics"));
    m_detailMetricsLabel->setWordWrap(true);
    m_detailPathsLabel = new QLabel(QStringLiteral("-"), detailPanel);
    m_detailPathsLabel->setObjectName(QStringLiteral("PreprocessDetailPaths"));
    m_detailPathsLabel->setWordWrap(true);
    detailText->addWidget(m_detailTitleLabel);
    detailText->addWidget(m_detailMetricsLabel);
    detailText->addWidget(m_detailPathsLabel);
    detailText->addStretch(1);
    detailLayout->addLayout(detailText, 2);
    m_spectrogramPreviewLabel = new QLabel(QStringLiteral("暂无时频图"), detailPanel);
    m_spectrogramPreviewLabel->setObjectName(QStringLiteral("PreprocessSpectrogramPreview"));
    m_spectrogramPreviewLabel->setAlignment(Qt::AlignCenter);
    m_spectrogramPreviewLabel->setMinimumSize(240, 150);
    detailLayout->addWidget(m_spectrogramPreviewLabel, 1);
    resultsLayout->addWidget(detailPanel);

    contentLayout()->addWidget(inputSection);
    contentLayout()->addWidget(parameterSection);
    contentLayout()->addWidget(runSection);
    contentLayout()->addWidget(resultsPanel, 1);

    m_processRunner = new PreprocessProcessRunner(this);
    connect(m_previewButton, &QPushButton::clicked, this, &PreprocessPage::previewCap);
    connect(m_capPathEdit, &QLineEdit::textChanged, this, [this]() {
        m_inputConfirmed = false;
        if (m_runButton && !isRunning()) {
            m_runButton->setEnabled(false);
        }
    });
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 IQRAW/CAP 文件"),
                                                          ProjectPaths::dataDirectory(),
                                                          QStringLiteral("IQ 数据 (*.iqraw *.cap);;所有文件 (*)"));
        if (!path.isEmpty()) {
            m_capPathEdit->setText(path);
            previewCap();
        }
    });
    connect(outputButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择预处理批次根目录"),
                                                                 m_outputDirEdit->text());
        if (!path.isEmpty()) {
            m_outputDirEdit->setText(path);
        }
    });
    connect(m_runButton, &QPushButton::clicked, this, &PreprocessPage::runPreprocess);
    connect(m_cancelButton, &QPushButton::clicked, this, &PreprocessPage::cancelPreprocess);
    connect(m_nextDatasetButton, &QPushButton::clicked, this, &PreprocessPage::requestOpenDataset);
    connect(m_advancedParamsToggle, &QPushButton::toggled, this, [this](bool checked) {
        m_advancedParamsPanel->setVisible(checked);
        m_advancedParamsToggle->setText(checked ? QStringLiteral("切换到推荐模式") : QStringLiteral("切换到专家模式"));
        m_parameterModeLabel->setText(checked
                                           ? QStringLiteral("当前模式：专家模式（可调整检测阈值和 STFT 参数；修改前请确认取值范围）")
                                           : QStringLiteral("当前模式：推荐模式（使用已验证的常规参数；适合首次处理）"));
    });
    connect(detailsToggle, &QPushButton::toggled, this, [this, detailsToggle](bool checked) {
        m_logOutput->setVisible(checked);
        detailsToggle->setText(checked ? QStringLiteral("隐藏运行详情") : QStringLiteral("显示运行详情"));
    });
    connect(m_resetProfileButton, &QPushButton::clicked, this, &PreprocessPage::resetProfile);
    connect(m_saveProfileButton, &QPushButton::clicked, this, &PreprocessPage::saveProfile);
    connect(m_loadProfileButton, &QPushButton::clicked, this, &PreprocessPage::loadProfile);
    connect(m_segmentTable, &QTableWidget::itemSelectionChanged, this, &PreprocessPage::selectResultRow);
    connect(m_sampleTable, &QTableWidget::itemSelectionChanged, this, &PreprocessPage::selectResultRow);
    connect(m_processRunner, &PreprocessProcessRunner::progressChanged, this, &PreprocessPage::updateProgress);
    connect(m_processRunner, &PreprocessProcessRunner::partialResultAvailable, this, &PreprocessPage::showPartialResult);
    connect(m_processRunner, &PreprocessProcessRunner::finished, this, [this](const PreprocessRunResult &result) {
        setBusy(false);
        showRunResult(result);
        if (!result.sampleRecords.isEmpty()) {
            saveSampleRecords(result.sampleRecords);
        }
        savePreprocessBatch(result);
        if (result.success || result.partial || result.cancelled || result.timedOut) {
            m_nextDatasetButton->setEnabled(!result.sampleRecords.isEmpty());
        }
        emit workflowStateChanged();
    });
    connect(m_processRunner, &PreprocessProcessRunner::failed, this, [this](const QString &message) {
        setBusy(false);
        setRunState(QStringLiteral("失败"), QStringLiteral("error"));
        setValidationMessage(message, QStringLiteral("error"));
        appendLog(QStringLiteral("预处理失败：%1").arg(message));
        emit workflowStateChanged();
    });
    connect(m_processRunner, &PreprocessProcessRunner::cancelled, this, [this]() {
        appendLog(QStringLiteral("任务已取消，正在整理并保留已生成文件。"));
        setValidationMessage(QStringLiteral("已收到取消请求，正在整理已生成结果。"), QStringLiteral("neutral"));
        emit workflowStateChanged();
    });

}

void PreprocessPage::previewCap()
{
    const QString path = m_capPathEdit ? m_capPathEdit->text().trimmed() : QString();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        setValidationMessage(QStringLiteral("请选择存在的 IQRAW 或 CAP 文件。"), QStringLiteral("error"));
        return;
    }
    CapProbeService service;
    const auto result = service.probe(path);
    if (!result.has_value()) {
        setValidationMessage(service.lastError(), QStringLiteral("error"));
        if (m_capMetric) {
            m_capMetric->setValue(QStringLiteral("需检查"));
        }
        return;
    }
    updateCapSummary(*result);
    m_inputConfirmed = true;
    const QFileInfo info(path);
    m_inputFileSummaryLabel->setText(QStringLiteral("已确认：%1\n格式 %2，文件大小 %3，预览 IQ 点数 %4。")
                                         .arg(info.fileName(), info.suffix().toUpper(), fileSizeText(info.size()))
                                         .arg(result->statistics.sampleCount));
    setValidationMessage(QStringLiteral("输入文件已确认，可以开始预处理。"), QStringLiteral("good"));
    if (m_capMetric) {
    m_capMetric->setValue(QStringLiteral("已确认"));
    if (m_runButton && !isRunning()) {
        m_runButton->setEnabled(true);
    }
    }
    appendLog(QStringLiteral("输入预览完成：%1").arg(path));
    emit workflowStateChanged();
}

void PreprocessPage::runPreprocess()
{
    if (!m_processRunner || m_processRunner->isRunning()) {
        return;
    }
    if (!m_inputConfirmed) {
        setValidationMessage(QStringLiteral("请先读取并确认输入文件。"), QStringLiteral("error"));
        return;
    }
    PreprocessRequest request;
    QString error;
    const QString inputPath = m_capPathEdit->text().trimmed();
    if (!buildRequest(inputPath, &request, &error)) {
        setValidationMessage(error, QStringLiteral("error"));
        appendLog(error);
        return;
    }

    const QString taskId = QStringLiteral("pp_%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString batchName = QStringLiteral("%1_%2").arg(QFileInfo(inputPath).completeBaseName(), taskId);
    const QString batchDir = QDir(m_outputDirEdit->text().trimmed()).filePath(batchName);
    request.taskId = taskId;
    request.batchId = batchName;
    request.sampleOutputDir = batchDir;
    request.progressPath = QDir(batchDir).filePath(QStringLiteral("progress.jsonl"));
    request.cancelPath = QDir(batchDir).filePath(QStringLiteral("cancel.flag"));
    m_outputSummaryLabel->setText(QStringLiteral("本次批次：%1").arg(QDir::toNativeSeparators(batchDir)));

    CapProbeService probe;
    const auto capInfo = probe.probe(inputPath);
    if (!capInfo.has_value()) {
        setValidationMessage(probe.lastError(), QStringLiteral("error"));
        return;
    }
    setBusy(true);
    setValidationMessage(QStringLiteral("任务已启动，正在读取文件。"), QStringLiteral("good"));
    setRunState(QStringLiteral("准备中"), QStringLiteral("running"));
    m_progressBar->setValue(0);
    m_progressDetailLabel->setText(QStringLiteral("正在启动 Python 预处理进程。"));
    m_logOutput->clear();
    appendLog(QStringLiteral("任务：%1").arg(taskId));
    appendLog(QStringLiteral("输入：%1").arg(inputPath));
    appendLog(QStringLiteral("输出：%1").arg(batchDir));
    if (m_computeBackendLabel) {
        m_computeBackendLabel->setText(QStringLiteral("计算后端：正在检查 CUDA GPU"));
    }
    m_cancelRequested = false;
    if (m_cancelButton) {
        m_cancelButton->setText(QStringLiteral("取消并保留结果"));
    }
    if (!m_processRunner->start(request, *capInfo)) {
        setBusy(false);
        setValidationMessage(QStringLiteral("无法启动预处理任务。"), QStringLiteral("error"));
    }
    emit workflowStateChanged();
}

void PreprocessPage::cancelPreprocess()
{
    if (!m_processRunner || !m_processRunner->isRunning() || m_cancelRequested) {
        return;
    }

    m_cancelRequested = true;
    m_processRunner->cancel();
    if (m_cancelButton) {
        m_cancelButton->setText(QStringLiteral("正在保留结果"));
        m_cancelButton->setEnabled(false);
    }
    setRunState(QStringLiteral("正在取消并保留已生成结果"), QStringLiteral("running"));
    setValidationMessage(QStringLiteral("已请求取消。后台会整理当前已生成样本，完成后可去数据集复核。"), QStringLiteral("neutral"));
    if (m_progressDetailLabel) {
        m_progressDetailLabel->setText(QStringLiteral("正在写入取消标记，并等待预处理进程保存部分结果。"));
    }
    appendLog(QStringLiteral("已请求取消预处理：后台将保留已生成文件和样本记录。"));
}

void PreprocessPage::updateProgress(const PreprocessProgress &progress)
{
    if (!progress.phase.isEmpty()) {
        setRunState(m_cancelRequested ? QStringLiteral("正在取消并保留已生成结果") : progress.phase,
                    QStringLiteral("running"));
    }

    if (m_progressBar) {
        m_progressBar->setValue(progress.percent);
    }
    if (m_segmentMetric) {
        m_segmentMetric->setValue(QString::number(progress.candidateCount));
    }
    if (m_sampleMetric) {
        m_sampleMetric->setValue(QString::number(progress.sampleCount));
    }
    const QString bytes = progress.totalBytes > 0
        ? QStringLiteral("%1 / %2").arg(fileSizeText(progress.bytesRead), fileSizeText(progress.totalBytes))
        : QStringLiteral("-");
    const QString eta = progress.etaMs > 0
        ? QStringLiteral("，预计剩余 %1 秒").arg(progress.etaMs / 1000)
        : QString();
    m_progressDetailLabel->setText(QStringLiteral("%1\n已读取 %2，分块 %3，候选 %4，样本 %5%6")
                                       .arg(progress.message, bytes)
                                       .arg(progress.chunksProcessed)
                                       .arg(progress.candidateCount)
                                       .arg(progress.sampleCount)
                                       .arg(eta));
}

void PreprocessPage::showPartialResult(const PreprocessRunResult &result)
{
    showRunResult(result);
    if (m_processRunner && m_processRunner->isRunning()) {
        if (m_cancelRequested) {
            setRunState(QStringLiteral("正在取消并保留已生成结果"), QStringLiteral("running"));
            m_qualitySummaryLabel->setText(QStringLiteral("已生成部分结果，取消完成后可去数据集页人工复核。"));
        } else {
            setRunState(QStringLiteral("处理中，已生成部分结果"), QStringLiteral("running"));
            m_qualitySummaryLabel->setText(QStringLiteral("已生成部分结果，任务仍在继续；当前仅供预览和质量检查。"));
        }
    }

}

void PreprocessPage::showRunResult(const PreprocessRunResult &result)
{
    m_latestResult = result;
    m_segmentTable->setRowCount(0);
    for (const PreprocessSegment &segment : result.segments) {
        const int row = m_segmentTable->rowCount();
        m_segmentTable->insertRow(row);
        m_segmentTable->setItem(row, 0, readonlyItem(segment.segmentId));
        m_segmentTable->setItem(row, 1, readonlyItem(QString::number(segment.startSample)));
        m_segmentTable->setItem(row, 2, readonlyItem(QString::number(segment.endSample)));
        m_segmentTable->setItem(row, 3, readonlyItem(QString::number(segment.durationMs, 'f', 3)));
        m_segmentTable->setItem(row, 4, readonlyItem(mhz(segment.bandwidthHz)));
        m_segmentTable->setItem(row, 5, readonlyItem(QString::number(segment.snrDb, 'f', 2)));
        m_segmentTable->setItem(row, 6, readonlyItem(QString::number(segment.score, 'f', 3)));
        m_segmentTable->setItem(row, 7, readonlyItem(segment.status == QStringLiteral("valid") ? QStringLiteral("已生成") : QStringLiteral("需检查")));
    }
    m_sampleTable->setRowCount(0);
    for (const SampleRecord &record : result.sampleRecords) {
        const int row = m_sampleTable->rowCount();
        m_sampleTable->insertRow(row);
        m_sampleTable->setItem(row, 0, readonlyItem(record.sampleId, record.sampleFilePath));
        m_sampleTable->setItem(row, 1, readonlyItem(record.candidateId));
        m_sampleTable->setItem(row, 2, readonlyItem(mhz(record.centerFrequencyHz)));
        m_sampleTable->setItem(row, 3, readonlyItem(mhz(record.bandwidthHz)));
        m_sampleTable->setItem(row, 4, readonlyItem(QString::number(record.score, 'f', 3)));
        const QString previewPath = record.candidatePreviewFilePath.isEmpty()
            ? record.spectrogramFilePath
            : record.candidatePreviewFilePath;
        m_sampleTable->setItem(row, 5, readonlyItem(previewPath.isEmpty() ? QStringLiteral("未生成") : QStringLiteral("已生成"),
                                                     previewPath));
        m_sampleTable->setItem(row, 6, readonlyItem(record.qualityStatus.isEmpty() ? QStringLiteral("待复核") : record.qualityStatus));
    }
    const bool hasResult = !result.segments.isEmpty() || !result.sampleRecords.isEmpty();
    m_emptyState->setVisible(!hasResult);
    m_emptyState->setText(hasResult ? QString() : QStringLiteral("任务完成，但没有检测到可用候选片段。请检查字节序、采样数据和处理方案。"));
    m_segmentMetric->setValue(QString::number(result.candidateSegmentCount > 0 ? result.candidateSegmentCount : result.segments.size()));
    m_sampleMetric->setValue(QString::number(result.sampleRecords.size()));
    if (result.averageBandwidthHz > 0.0) {
        if (auto *metric = findChild<MetricCard *>(QStringLiteral("PreprocessAverageBandwidthMetric"))) {
            metric->setValue(mhz(result.averageBandwidthHz));
        }
    }
    if (auto *metric = findChild<MetricCard *>(QStringLiteral("PreprocessAverageSnrMetric"))) {
        metric->setValue(result.averageSnrDb > 0.0 ? QStringLiteral("%1 dB").arg(result.averageSnrDb, 0, 'f', 2) : QStringLiteral("-"));
    }
    m_qualitySummaryLabel->setText(result.qualitySummary.isEmpty()
                                       ? (result.partial ? QStringLiteral("部分结果，建议在数据集页人工复核后再训练。") : QStringLiteral("结果已生成，可进入数据集页人工复核。"))
                                       : result.qualitySummary);
    m_resultsTabs->setCurrentWidget(result.sampleRecords.isEmpty() ? m_segmentTable : m_sampleTable);
    if (result.cancelled) {
        setRunState(QStringLiteral("已取消，部分结果已保留"), QStringLiteral("done"));
    } else if (result.timedOut) {
        setRunState(QStringLiteral("已超时，部分结果已保留"), QStringLiteral("error"));
    } else if (result.success) {
        setRunState(result.partial ? QStringLiteral("完成但有提示") : QStringLiteral("已完成"),
                    result.partial ? QStringLiteral("running") : QStringLiteral("done"));
    } else {
        setRunState(QStringLiteral("失败"), QStringLiteral("error"));
    }
    if (!result.message.isEmpty()) {
        appendLog(result.message);
    }
    if (m_computeBackendLabel) {
        if (result.computeBackend == QStringLiteral("cuda")) {
            const QString device = result.gpuDeviceName.isEmpty()
                ? QStringLiteral("CUDA GPU")
                : QStringLiteral("CUDA GPU：%1").arg(result.gpuDeviceName);
            m_computeBackendLabel->setText(QStringLiteral("计算后端：%1").arg(device));
        } else if (result.gpuFallback) {
            m_computeBackendLabel->setText(QStringLiteral("计算后端：CPU（CUDA 不可用，已回退）"));
        } else {
            m_computeBackendLabel->setText(QStringLiteral("计算后端：CPU"));
        }
    }
    if (!result.logs.isEmpty()) {
        for (const QString &line : result.logs) {
            appendLog(line);
        }
    }
}

void PreprocessPage::selectResultRow()
{
    if (m_resultsTabs && m_resultsTabs->currentWidget() == m_sampleTable) {
        const int row = m_sampleTable->currentRow();
        if (row >= 0 && row < m_latestResult.sampleRecords.size()) {
            const SampleRecord &record = m_latestResult.sampleRecords.at(row);
            m_detailTitleLabel->setText(record.sampleId);
            m_detailMetricsLabel->setText(QStringLiteral("候选：%1\n中心频率：%2\n带宽：%3\nSNR：%4 dB\n评分：%5")
                                              .arg(record.candidateId, mhz(record.centerFrequencyHz), mhz(record.bandwidthHz))
                                              .arg(record.snrDb, 0, 'f', 2).arg(record.score, 0, 'f', 3));
            const QString previewPath = record.candidatePreviewFilePath.isEmpty()
                ? record.spectrogramFilePath
                : record.candidatePreviewFilePath;
            m_detailPathsLabel->setText(QStringLiteral("原始文件：%1\nNPZ：%2\n候选局部图：%3\n样本时频图：%4")
                                            .arg(QDir::toNativeSeparators(record.rawFilePath),
                                                 QDir::toNativeSeparators(record.sampleFilePath),
                                                 record.candidatePreviewFilePath.isEmpty()
                                                     ? QStringLiteral("未生成")
                                                     : QDir::toNativeSeparators(record.candidatePreviewFilePath),
                                                 record.spectrogramFilePath.isEmpty()
                                                     ? QStringLiteral("未生成")
                                                     : QDir::toNativeSeparators(record.spectrogramFilePath)));
            if (!previewPath.isEmpty() && QFileInfo::exists(previewPath)) {
                QPixmap pixmap(previewPath);
                m_spectrogramPreviewLabel->setPixmap(pixmap.scaled(m_spectrogramPreviewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            } else {
                m_spectrogramPreviewLabel->setText(QStringLiteral("未生成候选局部图"));
                m_spectrogramPreviewLabel->setPixmap(QPixmap());
            }
            return;
        }
    }
    const int row = m_segmentTable->currentRow();
    if (row >= 0 && row < m_latestResult.segments.size()) {
        const auto &segment = m_latestResult.segments.at(row);
        m_detailTitleLabel->setText(segment.segmentId);
        m_detailMetricsLabel->setText(QStringLiteral("时间范围：%1 - %2\n时长：%3 ms\n带宽：%4\nSNR：%5 dB\n评分：%6")
                                          .arg(segment.startSample).arg(segment.endSample)
                                          .arg(segment.durationMs, 0, 'f', 3).arg(mhz(segment.bandwidthHz))
                                          .arg(segment.snrDb, 0, 'f', 2).arg(segment.score, 0, 'f', 3));
        m_detailPathsLabel->setText(QStringLiteral("候选片段输出：%1\n候选局部图：%2")
                                        .arg(QDir::toNativeSeparators(segment.outputFilePath),
                                             segment.candidatePreviewFilePath.isEmpty()
                                                 ? QStringLiteral("未生成")
                                                 : QDir::toNativeSeparators(segment.candidatePreviewFilePath)));
        if (!segment.candidatePreviewFilePath.isEmpty()
            && QFileInfo::exists(segment.candidatePreviewFilePath)) {
            QPixmap pixmap(segment.candidatePreviewFilePath);
            m_spectrogramPreviewLabel->setPixmap(pixmap.scaled(m_spectrogramPreviewLabel->size(),
                                                                Qt::KeepAspectRatio,
                                                                Qt::SmoothTransformation));
        } else {
            m_spectrogramPreviewLabel->setText(QStringLiteral("当前候选没有生成局部图"));
            m_spectrogramPreviewLabel->setPixmap(QPixmap());
        }
    }
}

void PreprocessPage::saveProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this,
        QStringLiteral("保存处理方案"),
        QStringLiteral("方案名称"),
        QLineEdit::Normal,
        QStringLiteral("推荐识别"),
        &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const QString path = QDir(ProjectPaths::preprocessProfilesDirectory()).filePath(name + QStringLiteral(".json"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject json;
    json.insert(QStringLiteral("name"), name);
    json.insert(QStringLiteral("detectionMode"), m_detectionModeCombo->currentData().toString());
    json.insert(QStringLiteral("payloadEndian"), m_payloadEndianCombo->currentData().toString());
    json.insert(QStringLiteral("exportSpectrogramPng"), m_exportSpectrogramCheck->isChecked());
    json.insert(QStringLiteral("createdAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    json.insert(QStringLiteral("pipelineVersion"), QStringLiteral("preprocess_v1"));
    const auto edits = findChildren<QLineEdit *>();
    for (QLineEdit *widget : edits) {
        if (widget->objectName().startsWith(QStringLiteral("Preprocess")) && widget != m_capPathEdit && widget != m_outputDirEdit) {
            json.insert(widget->objectName(), widget->text());
        }
    }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
        file.commit();
        appendLog(QStringLiteral("已保存本地处理方案：%1").arg(path));
    }
}

void PreprocessPage::loadProfile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("加载处理方案"),
        ProjectPaths::preprocessProfilesDirectory(),
        QStringLiteral("处理方案 (*.json);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        appendLog(QStringLiteral("尚未找到本地处理方案。"));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject json = document.object();
    const int modeIndex = m_detectionModeCombo->findData(json.value(QStringLiteral("detectionMode")).toString());
    if (modeIndex >= 0) {
        m_detectionModeCombo->setCurrentIndex(modeIndex);
    }
    const int endianIndex = m_payloadEndianCombo->findData(json.value(QStringLiteral("payloadEndian")).toString());
    if (endianIndex >= 0) {
        m_payloadEndianCombo->setCurrentIndex(endianIndex);
    }
    m_exportSpectrogramCheck->setChecked(json.value(QStringLiteral("exportSpectrogramPng")).toBool(false));
    for (QLineEdit *widget : findChildren<QLineEdit *>()) {
        if (json.contains(widget->objectName())) {
            widget->setText(json.value(widget->objectName()).toString());
        }
    }
    appendLog(QStringLiteral("已加载本地处理方案。"));
}

void PreprocessPage::resetProfile()
{
    m_detectionModeCombo->setCurrentIndex(0);
    m_payloadEndianCombo->setCurrentIndex(0);
    m_exportSpectrogramCheck->setChecked(false);
    m_windowSamplesEdit->setText(QStringLiteral("4096"));
    m_chunkSamplesEdit->setText(QStringLiteral("2000000"));
    m_frequencySpanEdit->setText(QStringLiteral("12.0"));
    m_energyOffsetEdit->setText(QStringLiteral("8.0"));
    m_targetBandwidthEdit->setText(QStringLiteral("20.0"));
    m_minBandwidthEdit->setText(QStringLiteral("16.0"));
    m_minWidebandFillEdit->setText(QStringLiteral("0.42"));
    m_minWidebandSnrEdit->setText(QStringLiteral("3.0"));
    m_morphFrequencyCloseEdit->setText(QStringLiteral("1.2"));
    m_shapeSplitQuantileEdit->setText(QStringLiteral("55.0"));
    m_shapeSplitMergeGapEdit->setText(QStringLiteral("0.05"));
    m_maxSegmentDurationEdit->setText(QStringLiteral("2.0"));
    m_bandWindowEdit->setText(QStringLiteral("2.0"));
    m_bandWindowOverlapEdit->setText(QStringLiteral("0.0"));
    m_minBandScoreEdit->setText(QStringLiteral("0.02"));
    m_cvColumnQuantileEdit->setText(QStringLiteral("40.0"));
    m_cvOccupancyEdit->setText(QStringLiteral("0.25"));
    m_cvMorphFrequencyEdit->setText(QStringLiteral("0.3"));
    m_cvMorphTimeEdit->setText(QStringLiteral("0.05"));
    m_minDurationEdit->setText(QStringLiteral("0.05"));
    m_mergeGapEdit->setText(QStringLiteral("0.20"));
    m_npersegEdit->setText(QStringLiteral("2048"));
    m_noverlapEdit->setText(QStringLiteral("1536"));
    m_spectrogramNpersegEdit->setText(QStringLiteral("512"));
    m_spectrogramNoverlapEdit->setText(QStringLiteral("384"));
    appendLog(QStringLiteral("已恢复推荐方案默认参数。"));
}

void PreprocessPage::saveSampleRecords(const QVector<SampleRecord> &records)
{
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    for (const SampleRecord &record : records) {
        bool replaced = false;
        for (SampleRecord &existing : state.samples) {
            const bool sameRecord = !record.batchId.isEmpty() && existing.batchId == record.batchId
                ? existing.sampleId == record.sampleId
                : existing.sampleId == record.sampleId && existing.sampleFilePath == record.sampleFilePath;
            if (sameRecord) {
                existing = record;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            state.samples.append(record);
        }
    }
    if (!store.save(state)) {
        appendLog(QStringLiteral("样本状态保存失败：%1").arg(store.lastError()));
    }
}

void PreprocessPage::savePreprocessBatch(const PreprocessRunResult &result)
{
    AppStateStore store;
    AppState state = store.load().value_or(AppState{});
    PreprocessBatchRecord batch;
    batch.batchId = QFileInfo(result.batchDirectory).fileName();
    batch.taskId = result.taskId;
    batch.inputFilePath = m_capPathEdit->text().trimmed();
    batch.outputDirectory = result.batchDirectory;
    batch.summaryPath = result.summaryPath;
    batch.profileName = m_detectionModeCombo->currentData().toString();
    batch.state = stateForResult(result);
    batch.createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    batch.candidateCount = result.candidateSegmentCount;
    batch.sampleCount = result.sampleRecords.size();
    batch.partial = result.partial || result.cancelled || result.timedOut;
    bool replaced = false;
    for (PreprocessBatchRecord &existing : state.preprocessBatches) {
        if (existing.batchId == batch.batchId && !batch.batchId.isEmpty()) {
            existing = batch;
            replaced = true;
            break;
        }
    }
    if (!replaced && !batch.batchId.isEmpty()) {
        state.preprocessBatches.append(batch);
    }
    store.save(state);
}

void PreprocessPage::setBusy(bool busy)
{
    if (!busy) {
        m_cancelRequested = false;
    }
    m_runButton->setEnabled(!busy);
    m_cancelButton->setEnabled(busy && !m_cancelRequested);
    m_cancelButton->setText(m_cancelRequested ? QStringLiteral("正在保留结果") : QStringLiteral("取消并保留结果"));
    m_previewButton->setEnabled(!busy);

    m_capPathEdit->setEnabled(!busy);
    m_outputDirEdit->setEnabled(!busy);
    const QVector<QWidget *> controls = {
        m_detectionModeCombo, m_payloadEndianCombo, m_exportSpectrogramCheck,
        m_advancedParamsToggle, m_resetProfileButton, m_saveProfileButton, m_loadProfileButton,
        m_windowSamplesEdit, m_chunkSamplesEdit, m_frequencySpanEdit, m_energyOffsetEdit,
        m_targetBandwidthEdit, m_minBandwidthEdit, m_minWidebandFillEdit, m_minWidebandSnrEdit,
        m_morphFrequencyCloseEdit, m_shapeSplitQuantileEdit, m_shapeSplitMergeGapEdit,
        m_maxSegmentDurationEdit, m_bandWindowEdit, m_bandWindowOverlapEdit, m_minBandScoreEdit,
        m_cvColumnQuantileEdit, m_cvOccupancyEdit, m_cvMorphFrequencyEdit, m_cvMorphTimeEdit,
        m_minDurationEdit, m_mergeGapEdit, m_npersegEdit, m_noverlapEdit,
        m_spectrogramNpersegEdit, m_spectrogramNoverlapEdit,
    };
    for (QWidget *control : controls) {
        if (control) {
            control->setEnabled(!busy);
        }
    }
}

void PreprocessPage::setValidationMessage(const QString &message, const QString &kind)
{
    m_validationMessage->setText(message);
    m_validationMessage->setProperty("kind", kind);
    m_validationMessage->style()->unpolish(m_validationMessage);
    m_validationMessage->style()->polish(m_validationMessage);
}

void PreprocessPage::setRunState(const QString &message, const QString &state)
{
    m_runStateLabel->setText(message);
    m_runStateLabel->setProperty("state", state);
    m_runStateLabel->style()->unpolish(m_runStateLabel);
    m_runStateLabel->style()->polish(m_runStateLabel);
}

void PreprocessPage::appendLog(const QString &line)
{
    if (m_logOutput) {
        m_logOutput->appendPlainText(line);
    }
}

void PreprocessPage::updateCapSummary(const CapProbeResult &result)
{
    m_versionLabel->setText(result.version.isEmpty() ? QStringLiteral("-") : result.version);
    m_headerLengthLabel->setText(result.headerLength > 0 ? QStringLiteral("0x%1").arg(result.headerLength, 0, 16) : QStringLiteral("-"));
    m_bandwidthLabel->setText(mhz(result.bandwidthHz));
    m_sampleRateLabel->setText(mhz(result.sampleRateHz));
    m_centerFrequencyLabel->setText(mhz(result.centerFrequencyHz));
    m_sampleCountLabel->setText(result.frameSampleCount > 0 ? QString::number(result.frameSampleCount)
                                                            : QString::number(result.statistics.sampleCount));
}

bool PreprocessPage::buildRequest(const QString &inputPath, PreprocessRequest *request, QString *error) const
{
    if (!request) {
        return false;
    }
    PreprocessRequest result;
    result.inputFilePath = inputPath;
    result.sampleOutputDir = m_outputDirEdit->text().trimmed();
    result.detectionMode = m_detectionModeCombo->currentData().toString();
    result.payloadEndian = m_payloadEndianCombo->currentData().toString();
    result.exportSpectrogramPng = m_exportSpectrogramCheck->isChecked();
    result.computeBackend = QStringLiteral("cuda");
    result.gpuDevice = 0;
    result.allowCpuFallback = false;
    if (inputPath.isEmpty() || !QFileInfo::exists(inputPath)) {
        if (error) *error = QStringLiteral("输入文件不存在。");
        return false;
    }
    const QString suffix = QFileInfo(inputPath).suffix().toLower();
    if (suffix != QStringLiteral("cap") && suffix != QStringLiteral("iqraw")) {
        if (error) *error = QStringLiteral("输入文件必须是 .cap 或 .iqraw。");
        return false;
    }
    if (!readInt(m_windowSamplesEdit, QStringLiteral("窗口采样点"), 1024, 262144, &result.windowSamples, error)
        || !readInt(m_chunkSamplesEdit, QStringLiteral("分块采样点"), 1024, 20000000, &result.chunkSamples, error)
        || !readDouble(m_frequencySpanEdit, QStringLiteral("频率跨度"), 0.1, 200.0, &result.frequencySpanMhz, error)
        || !readDouble(m_energyOffsetEdit, QStringLiteral("能量偏移"), 0.0, 80.0, &result.energyOffsetDb, error)
        || !readDouble(m_targetBandwidthEdit, QStringLiteral("目标带宽"), 0.1, 200.0, &result.targetBandwidthMhz, error)
        || !readDouble(m_minBandwidthEdit, QStringLiteral("最小带宽"), 0.0, 200.0, &result.minBandwidthMhz, error)
        || !readDouble(m_minWidebandFillEdit, QStringLiteral("最小宽带填充"), 0.0, 1.0, &result.minWidebandFill, error)
        || !readDouble(m_minWidebandSnrEdit, QStringLiteral("最小宽带 SNR"), -40.0, 80.0, &result.minWidebandSnrDb, error)
        || !readDouble(m_morphFrequencyCloseEdit, QStringLiteral("频向闭运算"), 0.0, 200.0, &result.morphFrequencyCloseMhz, error)
        || !readDouble(m_shapeSplitQuantileEdit, QStringLiteral("形状分割分位数"), 0.0, 100.0, &result.shapeSplitQuantile, error)
        || !readDouble(m_shapeSplitMergeGapEdit, QStringLiteral("形状合并间隔"), 0.0, 10000.0, &result.shapeSplitMergeGapMs, error)
        || !readDouble(m_maxSegmentDurationEdit, QStringLiteral("最大片段时长"), 0.0, 100000.0, &result.maxSegmentDurationMs, error)
        || !readDouble(m_bandWindowEdit, QStringLiteral("频带窗口"), 0.0, 10000.0, &result.bandWindowMs, error)
        || !readDouble(m_bandWindowOverlapEdit, QStringLiteral("窗口重叠"), 0.0, 10000.0, &result.bandWindowOverlapMs, error)
        || !readDouble(m_minBandScoreEdit, QStringLiteral("最小频带评分"), 0.0, 1.0, &result.minBandScore, error)
        || !readDouble(m_cvColumnQuantileEdit, QStringLiteral("CV 列分位数"), 0.0, 100.0, &result.cvColumnQuantile, error)
        || !readDouble(m_cvOccupancyEdit, QStringLiteral("CV 最小占空"), 0.0, 1.0, &result.cvMinWindowOccupancy, error)
        || !readDouble(m_cvMorphFrequencyEdit, QStringLiteral("CV 频向形态"), 0.0, 200.0, &result.cvMorphFrequencyMhz, error)
        || !readDouble(m_cvMorphTimeEdit, QStringLiteral("CV 时向形态"), 0.0, 10000.0, &result.cvMorphTimeMs, error)
        || !readDouble(m_minDurationEdit, QStringLiteral("最小时长"), 0.0, 10000.0, &result.minDurationMs, error)
        || !readDouble(m_mergeGapEdit, QStringLiteral("片段合并间隔"), 0.0, 10000.0, &result.mergeGapMs, error)
        || !readInt(m_npersegEdit, QStringLiteral("STFT nperseg"), 16, 262144, &result.nperseg, error)
        || !readInt(m_noverlapEdit, QStringLiteral("STFT noverlap"), 0, 262143, &result.noverlap, error)
        || !readInt(m_spectrogramNpersegEdit, QStringLiteral("PNG nperseg"), 16, 262144, &result.spectrogramNperseg, error)
        || !readInt(m_spectrogramNoverlapEdit, QStringLiteral("PNG noverlap"), 0, 262143, &result.spectrogramNoverlap, error)) {
        return false;
    }
    if (result.noverlap >= result.nperseg || result.spectrogramNoverlap >= result.spectrogramNperseg) {
        if (error) *error = QStringLiteral("重叠采样点必须小于对应的窗口采样点。");
        return false;
    }
    result.timeoutMs = result.exportSpectrogramPng ? 7200000 : 3600000;
    *request = result;
    return true;
}
