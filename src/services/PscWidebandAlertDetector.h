#pragma once

#include "services/CeyearPscSweep.h"

#include <QVector>

struct PscWidebandRule
{
    QString bandwidthClass;
    double minimumBandwidthHz = 0.0;
    double maximumBandwidthHz = 0.0;
};

struct PscWidebandAlert
{
    double startHz = 0.0;
    double stopHz = 0.0;
    double centerHz = 0.0;
    double bandwidthHz = 0.0;
    double peakLevelDbuv = 0.0;
    int consecutiveFrameCount = 0;
    double centerToleranceHz = 25000.0;
    QString bandwidthClass;
};

class PscWidebandAlertDetector
{
public:
    static constexpr double DefaultHitThresholdDbuv = -60.0;
    static constexpr double DefaultMinimumBandwidthHz = 16000000.0;
    static constexpr double DefaultMaximumBandwidthHz = 24000000.0;
    static QVector<PscWidebandRule> defaultRules();

    // Non-finite thresholds and invalid bandwidth ranges fall back to the defaults above.
    explicit PscWidebandAlertDetector(double hitThresholdDbuv = DefaultHitThresholdDbuv,
                                      double minimumBandwidthHz = DefaultMinimumBandwidthHz,
                                      double maximumBandwidthHz = DefaultMaximumBandwidthHz,
                                      int requiredConsecutiveFrames = 2);

    QVector<PscWidebandAlert> update(const CeyearPscFrame &frame);
    void reset();

private:
    double m_hitThresholdDbuv = DefaultHitThresholdDbuv;
    QVector<PscWidebandRule> m_rules;
    int m_requiredConsecutiveFrames = 2;
    QVector<PscWidebandAlert> m_previousAlerts;
};
