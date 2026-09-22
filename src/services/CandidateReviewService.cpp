#include "services/CandidateReviewService.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace
{
const QString kAlgorithmVersion = QStringLiteral("rule-review-v1-95pct-target");

QString normalizedSourcePath(const SampleRecord &sample)
{
    QString path = sample.rawFilePath.trimmed();
    if (path.isEmpty()) {
        path = sample.sampleFilePath.trimmed();
    }
    if (path.isEmpty()) {
        return QStringLiteral("<missing-source>");
    }
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).replace(QLatin1Char('\\'), QLatin1Char('/')).toLower();
}

double clamp01(double value)
{
    return qBound(0.0, value, 1.0);
}

double mean(const QVector<double> &values)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    double total = 0.0;
    for (const double value : values) {
        total += value;
    }
    return total / static_cast<double>(values.size());
}

double standardDeviation(const QVector<double> &values, double average)
{
    if (values.size() < 2) {
        return 0.0;
    }
    double squared = 0.0;
    for (const double value : values) {
        const double delta = value - average;
        squared += delta * delta;
    }
    return std::sqrt(squared / static_cast<double>(values.size()));
}

QString sampleReviewState(const QVector<SampleRecord> &samples, const QVector<int> &indices)
{
    bool allApproved = !indices.isEmpty();
    bool anyRejected = false;
    for (const int index : indices) {
        if (index < 0 || index >= samples.size()) {
            continue;
        }
        const SampleRecord &sample = samples.at(index);
        const QString quality = sample.qualityStatus.trimmed().isEmpty()
            ? sample.status.trimmed()
            : sample.qualityStatus.trimmed();
        anyRejected = anyRejected || quality == QStringLiteral("已剔除");
        allApproved = allApproved
            && (quality == QStringLiteral("复核通过") || quality == QStringLiteral("已标注"));
    }
    if (anyRejected) {
        return QStringLiteral("已剔除");
    }
    if (allApproved) {
        return QStringLiteral("复核通过");
    }
    return QStringLiteral("待复核");
}

QString modeSummary(const QVector<SampleRecord> &samples, const QVector<int> &indices)
{
    QSet<QString> modes;
    for (const int index : indices) {
        if (index >= 0 && index < samples.size()) {
            const QString mode = samples.at(index).sourceType.trimmed();
            if (!mode.isEmpty()) {
                modes.insert(mode);
            }
        }
    }
    return modes.values().join(QStringLiteral("、"));
}

CandidateReviewRecord summarize(
    const QVector<SampleRecord> &samples,
    const QVector<int> &indices,
    const CandidateReviewRecord *persisted)
{
    CandidateReviewRecord record;
    if (persisted) {
        record = *persisted;
    }
    record.algorithmVersion = kAlgorithmVersion;
    record.windowCount = indices.size();
    record.validWindowCount = 0;

    QVector<double> scores;
    QVector<double> snrs;
    QVector<double> occupancies;
    QVector<double> bandwidths;
    QStringList reasons;
    double scoreTotal = 0.0;
    double snrTotal = 0.0;
    double occupancyTotal = 0.0;
    double bandwidthTotal = 0.0;
    int bandwidthCount = 0;
    bool hasWidebandMode = false;

    for (const int index : indices) {
        if (index < 0 || index >= samples.size()) {
            continue;
        }
        const SampleRecord &sample = samples.at(index);
        if (record.sourceFilePath.isEmpty()) {
            record.sourceFilePath = sample.rawFilePath;
        }
        if (record.batchId.isEmpty()) {
            record.batchId = sample.batchId;
        }
        if (record.candidateId.isEmpty()) {
            record.candidateId = sample.candidateId;
        }
        if (record.candidatePreviewPath.isEmpty()) {
            record.candidatePreviewPath = sample.candidatePreviewFilePath;
        }
        if (record.labelType.isEmpty()) {
            record.labelType = sample.labelType;
        }
        if (sample.sampleFilePath.trimmed().isEmpty() || !QFileInfo::exists(sample.sampleFilePath)) {
            reasons.append(QStringLiteral("存在缺失样本文件"));
            continue;
        }

        ++record.validWindowCount;
        scores.append(sample.score);
        snrs.append(sample.snrDb);
        occupancies.append(sample.occupancy);
        scoreTotal += sample.score;
        snrTotal += sample.snrDb;
        occupancyTotal += sample.occupancy;
        if (sample.bandwidthHz > 0.0) {
            bandwidthTotal += sample.bandwidthHz / 1000000.0;
            ++bandwidthCount;
        }
        const QString mode = sample.sourceType.toLower();
        hasWidebandMode = hasWidebandMode
            || mode.contains(QStringLiteral("cv"))
            || mode.contains(QStringLiteral("wideband"));
    }

    if (record.validWindowCount > 0) {
        record.averageScore = scoreTotal / record.validWindowCount;
        record.averageSnrDb = snrTotal / record.validWindowCount;
        record.averageOccupancy = occupancyTotal / record.validWindowCount;
        record.minimumScore = *std::min_element(scores.cbegin(), scores.cend());
        record.maximumScore = *std::max_element(scores.cbegin(), scores.cend());
        record.ruleScore =
            0.30 * clamp01((record.averageScore - 0.20) / 0.65)
            + 0.25 * clamp01((record.averageOccupancy - 0.30) / 0.65)
            + 0.20 * clamp01((record.averageSnrDb - 3.0) / 20.0)
            + 0.15 * (bandwidthCount == 0
                           ? 0.0
                           : (bandwidthTotal / bandwidthCount >= 14.0
                                  && bandwidthTotal / bandwidthCount <= 26.0
                              ? 1.0
                              : 0.35))
            + 0.10 * (hasWidebandMode ? 1.0 : 0.35);

        const double scoreConsistency = 1.0 - clamp01(standardDeviation(scores, record.averageScore) / 0.20);
        record.ruleScore = clamp01(record.ruleScore * 0.85 + scoreConsistency * 0.15);
    }

    if (record.validWindowCount == 0) {
        record.recommendation = QStringLiteral("规则建议剔除");
        reasons.append(QStringLiteral("没有可读取的样本窗口"));
    } else if (record.validWindowCount < record.windowCount) {
        record.recommendation = QStringLiteral("待人工复核");
        reasons.append(QStringLiteral("部分窗口文件不可用"));
    } else if (record.ruleScore >= 0.78
               && record.averageOccupancy >= 0.72
               && record.averageSnrDb >= 5.0
               && record.minimumScore >= 0.55) {
        record.recommendation = QStringLiteral("规则建议通过");
        reasons.append(QStringLiteral("宽带占用、SNR、候选评分和窗口一致性良好"));
    } else if (record.ruleScore < 0.28
               || record.averageOccupancy < 0.25
               || record.averageSnrDb < 1.0) {
        record.recommendation = QStringLiteral("规则建议剔除");
        reasons.append(QStringLiteral("宽带形态或信噪指标不足"));
    } else {
        record.recommendation = QStringLiteral("待人工复核");
        reasons.append(QStringLiteral("处于规则边界，需查看候选总览图"));
    }

    if (record.reviewState.isEmpty()) {
        record.reviewState = sampleReviewState(samples, indices);
    }
    if (record.reviewState == QStringLiteral("待复核")) {
        const QString derivedState = sampleReviewState(samples, indices);
        if (derivedState != QStringLiteral("待复核")) {
            record.reviewState = derivedState;
        }
    }
    if (record.reason.isEmpty()) {
        record.reason = reasons.join(QStringLiteral("；"));
    }
    if (record.candidateId.isEmpty()) {
        record.candidateId = QStringLiteral("未命名候选");
    }
    if (record.labelType.isEmpty()) {
        record.labelType = QStringLiteral("未分类");
    }
    Q_UNUSED(modeSummary)
    return record;
}
} // namespace

QString CandidateReviewService::reviewKey(const SampleRecord &sample)
{
    const QString candidate = sample.candidateId.trimmed().isEmpty()
        ? sample.sampleId.trimmed()
        : sample.candidateId.trimmed();
    return normalizedSourcePath(sample)
        + QChar(0x1f)
        + sample.batchId.trimmed()
        + QChar(0x1f)
        + candidate;
}

QVector<CandidateReviewGroup> CandidateReviewService::buildGroups(
    const QVector<SampleRecord> &samples,
    const QVector<CandidateReviewRecord> &persistedReviews)
{
    QMap<QString, QVector<int>> groupedIndices;
    for (int index = 0; index < samples.size(); ++index) {
        groupedIndices[reviewKey(samples.at(index))].append(index);
    }

    QHash<QString, CandidateReviewRecord> persistedByKey;
    for (const CandidateReviewRecord &review : persistedReviews) {
        if (!review.reviewKey.trimmed().isEmpty()) {
            persistedByKey.insert(review.reviewKey, review);
        }
    }

    QVector<CandidateReviewGroup> groups;
    groups.reserve(groupedIndices.size());
    for (auto it = groupedIndices.cbegin(); it != groupedIndices.cend(); ++it) {
        CandidateReviewGroup group;
        group.sampleIndices = it.value();
        const CandidateReviewRecord persisted = persistedByKey.value(it.key());
        group.review = summarize(
            samples,
            group.sampleIndices,
            persisted.reviewKey.isEmpty() ? nullptr : &persisted);
        group.review.reviewKey = it.key();

        QVector<int> ranked = group.sampleIndices;
        std::sort(ranked.begin(), ranked.end(), [&samples](int left, int right) {
            const SampleRecord &leftSample = samples.at(left);
            const SampleRecord &rightSample = samples.at(right);
            if (!qFuzzyCompare(leftSample.score + 1.0, rightSample.score + 1.0)) {
                return leftSample.score > rightSample.score;
            }
            return leftSample.sampleId < rightSample.sampleId;
        });
        if (!ranked.isEmpty()) {
            group.review.representativeSampleId = samples.at(ranked.first()).sampleId;
            group.representativeSampleIndices.append(ranked.first());
            if (ranked.size() > 2) {
                group.representativeSampleIndices.append(ranked.at(ranked.size() / 2));
            }
            if (ranked.size() > 1) {
                group.representativeSampleIndices.append(ranked.last());
            }
        }
        groups.append(group);
    }

    std::stable_sort(groups.begin(), groups.end(), [](const CandidateReviewGroup &left, const CandidateReviewGroup &right) {
        auto priority = [](const CandidateReviewGroup &group) {
            if (group.review.reviewState == QStringLiteral("待复核")
                && group.review.recommendation == QStringLiteral("规则建议剔除")) {
                return 0;
            }
            if (group.review.reviewState == QStringLiteral("待复核")) {
                return 1;
            }
            if (group.review.reviewState == QStringLiteral("待抽检")) {
                return 2;
            }
            return 3;
        };
        const int leftPriority = priority(left);
        const int rightPriority = priority(right);
        if (leftPriority != rightPriority) {
            return leftPriority < rightPriority;
        }
        if (!qFuzzyCompare(left.review.ruleScore + 1.0, right.review.ruleScore + 1.0)) {
            return left.review.ruleScore < right.review.ruleScore;
        }
        return left.review.reviewKey < right.review.reviewKey;
    });
    return groups;
}

bool CandidateReviewService::matchesFilter(const CandidateReviewGroup &group, const QString &filter)
{
    if (filter.isEmpty() || filter == QStringLiteral("all")) {
        return true;
    }
    return group.review.reviewState == filter
        || group.review.recommendation == filter;
}

