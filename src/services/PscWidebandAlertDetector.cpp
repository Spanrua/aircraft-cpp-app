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
