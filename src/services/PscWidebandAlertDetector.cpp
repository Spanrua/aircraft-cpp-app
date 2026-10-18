#include "PscWidebandAlertDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool isValidBandwidthRange(double minimumBandwidthHz, double maximumBandwidthHz)
{
    return std::isfinite(minimumBandwidthHz)
        && std::isfinite(maximumBandwidthHz)
        && minimumBandwidthHz >= 0.0
        && maximumBandwidthHz >= minimumBandwidthHz;
}

double median(QVector<double> values)
{
    if (values.isEmpty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const int middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values.at(middle - 1) + values.at(middle)) / 2.0 : values.at(middle);
}

double lowerQuantile(QVector<double> values, double quantile)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const int index = (std::max)(0, (std::min)(values.size() - 1,
                                                static_cast<int>(std::floor((values.size() - 1) * quantile))));
    return values.at(index);
}

} // namespace

PscWidebandAlertDetector::PscWidebandAlertDetector(double hitThresholdDbuv,
                                                   double minimumBandwidthHz,
                                                   double maximumBandwidthHz,
                                                   int requiredConsecutiveFrames)
    : m_requiredConsecutiveFrames((std::max)(2, requiredConsecutiveFrames))
{
    Q_UNUSED(hitThresholdDbuv)
    if (minimumBandwidthHz == DefaultMinimumBandwidthHz && maximumBandwidthHz == DefaultMaximumBandwidthHz) {
        m_rules = defaultRules();
    } else if (isValidBandwidthRange(minimumBandwidthHz, maximumBandwidthHz)) {
        m_rules = {{QStringLiteral("自定义"), minimumBandwidthHz, maximumBandwidthHz}};
    } else {
        m_rules = defaultRules();
    }
}

QVector<PscWidebandRule> PscWidebandAlertDetector::defaultRules()
{
    return {
        {QStringLiteral("10M"), 8000000.0, 12000000.0},
        {QStringLiteral("20M"), 16000000.0, 24000000.0},
        {QStringLiteral("40M"), 32000000.0, 48000000.0}
    };
}

QVector<PscWidebandAlert> PscWidebandAlertDetector::classifyFrame(const CeyearPscFrame &frame) const
{
    QVector<CeyearPscBin> bins;
    bins.reserve(frame.bins.size());
    for (const CeyearPscBin &bin : frame.bins) {
        if (std::isfinite(bin.frequencyHz) && std::isfinite(bin.levelDbuv)) {
            bins.append(bin);
        }
    }
    std::sort(bins.begin(), bins.end(), [](const CeyearPscBin &left, const CeyearPscBin &right) {
        return left.frequencyHz < right.frequencyHz;
    });

    QVector<double> frameLevels;
    frameLevels.reserve(bins.size());
    for (const CeyearPscBin &bin : bins) {
        frameLevels.append(bin.levelDbuv);
    }
    std::sort(frameLevels.begin(), frameLevels.end());
    const int lowestCount = (std::max)(1, static_cast<int>(std::ceil(frameLevels.size() * 0.2)));
    const double fallbackNoiseDbuv = median(frameLevels.mid(0, lowestCount));

    double binStepHz = std::numeric_limits<double>::infinity();
    for (int index = 1; index < bins.size(); ++index) {
        const double gapHz = bins.at(index).frequencyHz - bins.at(index - 1).frequencyHz;
        if (gapHz > 0.0) {
            binStepHz = (std::min)(binStepHz, gapHz);
        }
    }

    QVector<PscWidebandAlert> currentAlerts;
    if (std::isfinite(binStepHz)) {
        const double maximumGapHz = binStepHz * 1.5;
        bool hasBand = false;
        PscWidebandAlert band;
        double lastFrequencyHz = 0.0;

        const auto appendBand = [&]() {
            if (!hasBand) {
                return;
            }

            band.stopHz = lastFrequencyHz;
            band.centerHz = (band.startHz + band.stopHz) / 2.0;
            band.bandwidthHz = band.stopHz - band.startHz + binStepHz;
            QVector<double> leftSideband;
            QVector<double> rightSideband;
            QVector<double> leftSidebandFrequencies;
            QVector<double> rightSidebandFrequencies;
            const double coverageToleranceHz = binStepHz / 2.0;
            for (const CeyearPscBin &sample : bins) {
                if (sample.frequencyHz >= band.startHz - SidebandNoiseWindowHz && sample.frequencyHz < band.startHz) {
                    leftSideband.append(sample.levelDbuv);
                    leftSidebandFrequencies.append(sample.frequencyHz);
                } else if (sample.frequencyHz > band.stopHz && sample.frequencyHz <= band.stopHz + SidebandNoiseWindowHz) {
                    rightSideband.append(sample.levelDbuv);
                    rightSidebandFrequencies.append(sample.frequencyHz);
                }
            }
            const auto hasNoInternalGap = [&](const QVector<double> &frequencies) {
                for (int index = 1; index < frequencies.size(); ++index) {
                    if (frequencies.at(index) - frequencies.at(index - 1) > maximumGapHz) {
                        return false;
                    }
                }
                return true;
            };
            const bool leftSidebandCoversWindow = !leftSidebandFrequencies.isEmpty()
                && leftSidebandFrequencies.first() <= band.startHz - SidebandNoiseWindowHz + coverageToleranceHz
                && leftSidebandFrequencies.last() >= band.startHz - binStepHz
                && hasNoInternalGap(leftSidebandFrequencies);
            const bool rightSidebandCoversWindow = !rightSidebandFrequencies.isEmpty()
                && rightSidebandFrequencies.first() <= band.stopHz + binStepHz
                && rightSidebandFrequencies.last() >= band.stopHz + SidebandNoiseWindowHz - coverageToleranceHz
                && hasNoInternalGap(rightSidebandFrequencies);
            const double sidebandNoiseDbuv = leftSideband.isEmpty() || rightSideband.isEmpty()
                    || !leftSidebandCoversWindow || !rightSidebandCoversWindow
                ? fallbackNoiseDbuv
                : median(leftSideband + rightSideband);
            if (band.peakLevelDbuv < sidebandNoiseDbuv + RelativeNoiseRiseDb) {
                return;
            }
            for (const PscWidebandRule &rule : m_rules) {
                if (band.bandwidthHz >= rule.minimumBandwidthHz && band.bandwidthHz <= rule.maximumBandwidthHz) {
                    band.bandwidthClass = rule.bandwidthClass;
                    currentAlerts.append(band);
                    break;
                }
            }
        };

        for (const CeyearPscBin &bin : bins) {
            if (bin.levelDbuv < fallbackNoiseDbuv + RelativeNoiseRiseDb) {
                continue;
            }

            if (!hasBand || bin.frequencyHz - lastFrequencyHz > maximumGapHz) {
                appendBand();
                band = {};
                band.startHz = bin.frequencyHz;
                band.peakLevelDbuv = bin.levelDbuv;
                hasBand = true;
            } else {
                band.peakLevelDbuv = (std::max)(band.peakLevelDbuv, bin.levelDbuv);
            }
            lastFrequencyHz = bin.frequencyHz;
        }
        appendBand();
    }

    const double centerToleranceHz = (std::max)(25000.0, binStepHz * 2.0);
    for (PscWidebandAlert &alert : currentAlerts) {
        alert.centerToleranceHz = centerToleranceHz;
    }
    return currentAlerts;
}

QVector<PscWidebandAlert> PscWidebandAlertDetector::classifyOfflineSliceSequence(
    const QVector<CeyearPscFrame> &frames) const
{
    struct SliceHit {
        int frameIndex = 0;
        double startHz = 0.0;
        double stopHz = 0.0;
        double peakLevelDbuv = 0.0;
        double noiseDbuv = 0.0;
    };
    struct Event {
        QVector<SliceHit> hits;
        int lastFrameIndex = 0;
    };

    QVector<QVector<CeyearPscBin>> slices;
    QVector<double> sliceStarts;
    double typicalStepHz = 0.0;
    for (int frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        QVector<CeyearPscBin> bins;
        for (const CeyearPscBin &bin : frames.at(frameIndex).bins) {
            if (std::isfinite(bin.frequencyHz) && std::isfinite(bin.levelDbuv)) {
                bins.append(bin);
            }
        }
        std::sort(bins.begin(), bins.end(), [](const CeyearPscBin &left, const CeyearPscBin &right) {
            return left.frequencyHz < right.frequencyHz;
        });
        if (bins.size() < 2) {
            continue;
        }
        slices.append(bins);
        sliceStarts.append(bins.first().frequencyHz);
        QVector<double> gaps;
        for (int index = 1; index < bins.size(); ++index) {
            const double gapHz = bins.at(index).frequencyHz - bins.at(index - 1).frequencyHz;
            if (gapHz > 0.0) {
                gaps.append(gapHz);
            }
        }
        const double stepHz = median(gaps);
        if (!std::isfinite(stepHz) || stepHz <= 0.0) {
            continue;
        }
        typicalStepHz = typicalStepHz > 0.0 ? (std::min)(typicalStepHz, stepHz) : stepHz;
    }

    if (slices.size() < 2 || sliceStarts.size() < 2 || typicalStepHz <= 0.0) {
        return {};
    }
    int cycleLength = 1;
    const double startToleranceHz = (std::max)(typicalStepHz * 2.0, 100000.0);
    for (int period = 1; period <= (std::min)(32, sliceStarts.size() / 2); ++period) {
        int matches = 0;
        const int comparisons = sliceStarts.size() - period;
        for (int index = period; index < sliceStarts.size(); ++index) {
            if (qAbs(sliceStarts.at(index) - sliceStarts.at(index - period)) <= startToleranceHz) {
                ++matches;
            }
        }
        if (comparisons > 0 && matches * 100 >= comparisons * 95) {
            cycleLength = period;
            break;
        }
    }

    QVector<SliceHit> hits;
    const double maximumGapHz = (std::max)(typicalStepHz * 1.5, 25000.0);
    double minimumRuleBandwidthHz = std::numeric_limits<double>::infinity();
    for (const PscWidebandRule &rule : m_rules) {
        minimumRuleBandwidthHz = (std::min)(minimumRuleBandwidthHz, rule.minimumBandwidthHz);
    }
    int cycleIndex = 0;
    for (int cycleStart = 0; cycleStart + cycleLength <= slices.size(); cycleStart += cycleLength, ++cycleIndex) {
        QVector<CeyearPscBin> cycleBins;
        for (int sliceOffset = 0; sliceOffset < cycleLength; ++sliceOffset) {
            cycleBins += slices.at(cycleStart + sliceOffset);
        }
        std::sort(cycleBins.begin(), cycleBins.end(), [](const CeyearPscBin &left, const CeyearPscBin &right) {
            return left.frequencyHz < right.frequencyHz;
        });
        QVector<double> levels;
        levels.reserve(cycleBins.size());
        for (const CeyearPscBin &bin : cycleBins) {
            levels.append(bin.levelDbuv);
        }
        const double noiseDbuv = lowerQuantile(levels, 0.20);
        const bool isWideSweep = cycleBins.last().frequencyHz - cycleBins.first().frequencyHz >= 50000000.0;
        QVector<CeyearPscBin> run;
        const auto appendRun = [&]() {
            const double runBandwidthHz = run.isEmpty() ? 0.0 : run.last().frequencyHz - run.first().frequencyHz;
            if (run.size() >= 2
                && (!isWideSweep || runBandwidthHz >= minimumRuleBandwidthHz)) {
                const auto peak = std::max_element(run.cbegin(), run.cend(), [](const CeyearPscBin &left, const CeyearPscBin &right) {
                    return left.levelDbuv < right.levelDbuv;
                });
                hits.append({cycleIndex, run.first().frequencyHz, run.last().frequencyHz, peak->levelDbuv, noiseDbuv});
            }
            run.clear();
        };
        for (const CeyearPscBin &bin : cycleBins) {
            if (bin.levelDbuv < noiseDbuv + RelativeNoiseRiseDb) {
                appendRun();
                continue;
            }
            if (!run.isEmpty() && bin.frequencyHz - run.last().frequencyHz > maximumGapHz) {
                appendRun();
            }
            run.append(bin);
        }
        appendRun();
    }

    if (hits.isEmpty()) {
        return {};
    }

    QVector<Event> events;
    const int maximumFrameGap = 2;
    const double frequencyToleranceHz = (std::max)(typicalStepHz * 2.0, 250000.0);
    for (const SliceHit &hit : hits) {
        int bestEvent = -1;
        double bestGapHz = std::numeric_limits<double>::infinity();
        double bestOverlapHz = -std::numeric_limits<double>::infinity();
        for (int index = 0; index < events.size(); ++index) {
            const Event &event = events.at(index);
            if (hit.frameIndex <= event.lastFrameIndex
                || hit.frameIndex - event.lastFrameIndex > maximumFrameGap) {
                continue;
            }
            const SliceHit &previousHit = event.hits.last();
            const double overlapHz = (std::min)(hit.stopHz, previousHit.stopHz)
                - (std::max)(hit.startHz, previousHit.startHz);
            const double gapHz = hit.startHz > previousHit.stopHz ? hit.startHz - previousHit.stopHz
                : previousHit.startHz > hit.stopHz ? previousHit.startHz - hit.stopHz : 0.0;
            if (gapHz <= frequencyToleranceHz
                && (overlapHz > bestOverlapHz
                    || (overlapHz == bestOverlapHz && gapHz < bestGapHz))) {
                bestOverlapHz = overlapHz;
                bestGapHz = gapHz;
                bestEvent = index;
            }
        }
        if (bestEvent < 0) {
            events.append({{hit}, hit.frameIndex});
        } else {
            events[bestEvent].hits.append(hit);
            events[bestEvent].lastFrameIndex = hit.frameIndex;
        }
    }

    QVector<PscWidebandAlert> alerts;
    for (const Event &event : events) {
        if (event.hits.size() < 3) {
            continue;
        }
        double startHz = event.hits.first().startHz;
        double stopHz = event.hits.first().stopHz;
        double peakLevelDbuv = event.hits.first().peakLevelDbuv;
        double marginTotalDb = 0.0;
        QVector<double> hitCentersHz;
        hitCentersHz.reserve(event.hits.size());
        for (const SliceHit &hit : event.hits) {
            startHz = (std::min)(startHz, hit.startHz);
            stopHz = (std::max)(stopHz, hit.stopHz);
            peakLevelDbuv = (std::max)(peakLevelDbuv, hit.peakLevelDbuv);
            marginTotalDb += hit.peakLevelDbuv - hit.noiseDbuv;
            hitCentersHz.append((hit.startHz + hit.stopHz) / 2.0);
        }
        const double observedBandwidthHz = stopHz - startHz;
        if (observedBandwidthHz < 1000000.0
            || marginTotalDb / event.hits.size() < RelativeNoiseRiseDb) {
            continue;
        }
        const double observedBandwidthMhz = observedBandwidthHz / 1000000.0;
        const PscWidebandRule *matchedRule = nullptr;
        if (observedBandwidthMhz < 8.0) {
            for (const PscWidebandRule &rule : m_rules) {
                if (rule.bandwidthClass == QStringLiteral("20M")) {
                    matchedRule = &rule;
                    break;
                }
            }
        }
        for (const PscWidebandRule &rule : m_rules) {
            if (!matchedRule && observedBandwidthHz >= rule.minimumBandwidthHz
                && observedBandwidthHz <= rule.maximumBandwidthHz) {
                matchedRule = &rule;
            }
        }
        double bestDistanceMhz = std::numeric_limits<double>::infinity();
        for (const PscWidebandRule &rule : m_rules) {
            if (matchedRule) {
                break;
            }
            const double nominalMhz = (rule.minimumBandwidthHz + rule.maximumBandwidthHz) / 2000000.0;
            const double distanceMhz = qAbs(observedBandwidthMhz - nominalMhz);
            if (distanceMhz < bestDistanceMhz) {
                bestDistanceMhz = distanceMhz;
                matchedRule = &rule;
            }
        }
        if (!matchedRule) {
            continue;
        }
        const double nominalBandwidthHz = (matchedRule->minimumBandwidthHz + matchedRule->maximumBandwidthHz) / 2.0;
        const double centerHz = median(hitCentersHz);
        alerts.append({centerHz - nominalBandwidthHz / 2.0,
                       centerHz + nominalBandwidthHz / 2.0,
                       centerHz,
                       nominalBandwidthHz,
                       peakLevelDbuv,
                       event.hits.size(),
                       (std::max)(25000.0, typicalStepHz * 2.0),
                       matchedRule->bandwidthClass});
    }
    QVector<PscWidebandAlert> rankedAlerts;
    rankedAlerts.reserve(alerts.size());
    while (!alerts.isEmpty()) {
        int bestIndex = 0;
        for (int index = 1; index < alerts.size(); ++index) {
            const PscWidebandAlert &candidate = alerts.at(index);
            const PscWidebandAlert &best = alerts.at(bestIndex);
            if (candidate.consecutiveFrameCount > best.consecutiveFrameCount
                || (candidate.consecutiveFrameCount == best.consecutiveFrameCount
                    && (candidate.peakLevelDbuv > best.peakLevelDbuv
                        || (candidate.peakLevelDbuv == best.peakLevelDbuv
                            && candidate.centerHz < best.centerHz)))) {
                bestIndex = index;
            }
        }
        rankedAlerts.append(alerts.at(bestIndex));
        alerts.removeAt(bestIndex);
    }
    return rankedAlerts;
}

QVector<PscWidebandAlert> PscWidebandAlertDetector::update(const CeyearPscFrame &frame)
{
    QVector<PscWidebandAlert> currentAlerts = classifyFrame(frame);
    QVector<bool> previousMatched(m_previousAlerts.size(), false);
    for (PscWidebandAlert &alert : currentAlerts) {
        const double currentToleranceHz = alert.centerToleranceHz;
        int closestPrevious = -1;
        double closestDifferenceHz = std::numeric_limits<double>::infinity();
        for (int index = 0; index < m_previousAlerts.size(); ++index) {
            if (previousMatched.at(index)) {
                continue;
            }

            const double differenceHz = std::abs(alert.centerHz - m_previousAlerts.at(index).centerHz);
            const double previousToleranceHz = m_previousAlerts.at(index).centerToleranceHz;
            const double matchingToleranceHz = (std::max)(currentToleranceHz, previousToleranceHz);
            if (alert.bandwidthClass == m_previousAlerts.at(index).bandwidthClass
                && differenceHz <= matchingToleranceHz && differenceHz < closestDifferenceHz) {
                closestPrevious = index;
                closestDifferenceHz = differenceHz;
            }
        }
        if (closestPrevious >= 0) {
            previousMatched[closestPrevious] = true;
            alert.consecutiveFrameCount = m_previousAlerts.at(closestPrevious).consecutiveFrameCount + 1;
        } else {
            alert.consecutiveFrameCount = 1;
        }
    }

    m_previousAlerts = currentAlerts;

    QVector<PscWidebandAlert> confirmedAlerts;
    for (const PscWidebandAlert &alert : currentAlerts) {
        if (alert.consecutiveFrameCount >= m_requiredConsecutiveFrames) {
            confirmedAlerts.append(alert);
        }
    }
    return confirmedAlerts;
}

void PscWidebandAlertDetector::reset()
{
    m_previousAlerts.clear();
}
