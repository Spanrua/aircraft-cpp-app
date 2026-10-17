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
};

class PscWidebandAlertDetector
{
public:
    explicit PscWidebandAlertDetector(double hitThresholdDbuv = -60.0,
                                      double minimumBandwidthHz = 16000000.0,
                                      double maximumBandwidthHz = 24000000.0,
                                      int requiredConsecutiveFrames = 2);

    QVector<PscWidebandAlert> update(const CeyearPscFrame &frame);
    void reset();

private:
    double m_hitThresholdDbuv = -60.0;
    double m_minimumBandwidthHz = 16000000.0;
    double m_maximumBandwidthHz = 24000000.0;
    int m_requiredConsecutiveFrames = 2;
    QVector<PscWidebandAlert> m_previousAlerts;
};
