#pragma once

#include "services/CeyearPscSweep.h"

#include <QVector>

struct PscWidebandAlert
{
    double startHz = 0.0;
    double stopHz = 0.0;
    double centerHz = 0.0;
    double bandwidthHz = 0.0;
    double peakLevelDbuv = 0.0;
    int consecutiveFrameCount = 0;
    double centerToleranceHz = 25000.0;
};

class PscWidebandAlertDetector
{
public:
    static constexpr double DefaultHitThresholdDbuv = -60.0;
    static constexpr double DefaultMinimumBandwidthHz = 16000000.0;
    static constexpr double DefaultMaximumBandwidthHz = 24000000.0;

    // Non-finite thresholds and invalid bandwidth ranges fall back to the defaults above.
    explicit PscWidebandAlertDetector(double hitThresholdDbuv = DefaultHitThresholdDbuv,
                                      double minimumBandwidthHz = DefaultMinimumBandwidthHz,
                                      double maximumBandwidthHz = DefaultMaximumBandwidthHz,
                                      int requiredConsecutiveFrames = 2);

    QVector<PscWidebandAlert> update(const CeyearPscFrame &frame);
    void reset();

private:
    double m_hitThresholdDbuv = DefaultHitThresholdDbuv;
    double m_minimumBandwidthHz = DefaultMinimumBandwidthHz;
    double m_maximumBandwidthHz = DefaultMaximumBandwidthHz;
    int m_requiredConsecutiveFrames = 2;
    QVector<PscWidebandAlert> m_previousAlerts;
};
