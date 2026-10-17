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

} // namespace

PscWidebandAlertDetector::PscWidebandAlertDetector(double hitThresholdDbuv,
                                                   double minimumBandwidthHz,
                                                   double maximumBandwidthHz,
                                                   int requiredConsecutiveFrames)
    : m_hitThresholdDbuv(std::isfinite(hitThresholdDbuv) ? hitThresholdDbuv : DefaultHitThresholdDbuv)
    , m_minimumBandwidthHz(minimumBandwidthHz)
    , m_maximumBandwidthHz(maximumBandwidthHz)
    , m_requiredConsecutiveFrames((std::max)(2, requiredConsecutiveFrames))
{
    if (!isValidBandwidthRange(m_minimumBandwidthHz, m_maximumBandwidthHz)) {
        m_minimumBandwidthHz = DefaultMinimumBandwidthHz;
        m_maximumBandwidthHz = DefaultMaximumBandwidthHz;
    }
}

QVector<PscWidebandAlert> PscWidebandAlertDetector::update(const CeyearPscFrame &frame)
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
            if (band.bandwidthHz >= m_minimumBandwidthHz && band.bandwidthHz <= m_maximumBandwidthHz) {
                currentAlerts.append(band);
            }
        };

        for (const CeyearPscBin &bin : bins) {
            if (bin.levelDbuv < m_hitThresholdDbuv) {
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
    QVector<bool> previousMatched(m_previousAlerts.size(), false);
    for (PscWidebandAlert &alert : currentAlerts) {
        int closestPrevious = -1;
        double closestDifferenceHz = std::numeric_limits<double>::infinity();
        for (int index = 0; index < m_previousAlerts.size(); ++index) {
            if (previousMatched.at(index)) {
                continue;
            }

            const double differenceHz = std::abs(alert.centerHz - m_previousAlerts.at(index).centerHz);
            if (differenceHz <= centerToleranceHz && differenceHz < closestDifferenceHz) {
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
